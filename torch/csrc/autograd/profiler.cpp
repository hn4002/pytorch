#include <torch/csrc/autograd/profiler.h>
#include <torch/csrc/autograd/function.h>
#include <torch/csrc/jit/frontend/code_template.h>

#include <torch/csrc/jit/runtime/operator.h>

#include <ATen/core/op_registration/op_registration.h>

#include <fstream>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <ATen/ThreadLocalDebugInfo.h>

namespace torch { namespace autograd { namespace profiler {

namespace {

CUDAStubs default_stubs;
constexpr CUDAStubs* default_stubs_addr = &default_stubs;
// Constant initialization, so it is guaranteed to be initialized before
// static initialization calls which may invoke registerCUDAMethods
static CUDAStubs* cuda_stubs = default_stubs_addr;

// We decompose the profiler logic into the following components:
//
// ThreadLocalDebugInfo:
//
// ThreadLocalDebugInfo is a thread local mapping from slots into
// the debug information structs.
// ThreadLocalDebugInfo is automatically propagated across thread
// boundaries, including the cases of:
//  - launching async jobs with at::launch
//  - executing JIT continuations
//  - moving from the forward threads into autograd (backward) threads
//
// Entries in ThreadLocalDebugInfo are managed by DebugInfoGuard
// which can be used to add or overwrite an entry in the thread local
// mapping. A corresponding entry is removed when the guard is destroyed,
// potentially revealing the previously set value for the same slot.
//
// For the async tasks, slots previuosly set in the main thread before
// launching of an async task are shared and visible in the async task.
//
// On the other hand, any adding or overwriting of the mapping by the
// async task is not visible to the main thread and any modification
// (including removal of the entries) in the main thread is not visible
// to the async task if it happends after launching the task.
//
// We use ThreadLocalDebugInfo (slot PROFILER) to store profiler config,
// as well as a list of events that happen during profiling.
// An instance of ThreadLocalDebugInfo is created each time we enter
// profiler (i.e. enter profiling context manager/call enableConfig) and
// uniquely identifies a profiling run.
//
// We automatically propagate ThreadLocalDebugInfo into async tasks,
// as well as across JIT continuations and autograd thread, so all
// the operations that happen between profiling start and end
// (not necessarily within the same thread) are recorded.
// Unless the profiling slot is overwritten as in the case of nested
// profiling ranges (in this case events for the subrange are handled
// by the nested profiler)
//
// When we exit a profiling range (either by exiting profiling context
// manager or by calling disableProfiler), we remove the previously set
// profiling entry for the given thread local mapping, and consolidate
// events in the profiling result
//
//
// ThreadLocalState:
//
// ThreadLocalState takes a 'snapshot' of thread local variables
// using provided getters. It is used together with ThreadLocalStateGuard
// to transfer the snapshot across thread boundary and set the thread local
// values as in the parent task.
//
// Profiler uses ThreadLocalState to propagate profiler's thread local state.
// Whenever we try to set the current thread local profiler config,
// we update the corresponding thread local variable and check whether
// we need to push or pop a pair of thread local callbacks.
// Thus making sure that profiling callbacks are set only when needed.
//
//
// RecordFunction and observers
//
// Profiler uses observers mechanism to add a pair of thread local conditional
// callbacks that are executed on a fixed number of predetermined ranges,
// including:
//  - c10/ATen ops
//  - TorchScript functions/methods
//  - user defined named ranges (see `record_function` python context manager)
//
// Profiler setups a pair of callbacks that record profiling events and save them
// into the thread local profiler struct (ThreadLocalDebugInfo, PROFILER slot)
//
//
// Thus, the overall logic is:
//
// enableProfiler:
//  - pushes new ThreadLocalDebugInfo (slot PROFILER) as the profiler config
//    for the current thread
//  - if needed, pushes profiling callbacks for the current thread
//
// disableProfiler:
//  - pops PROFILER slot from the current ThreadLocalDebugInfo and
//    consolidates events
//  - if no one else is using profiler, pops profiling callbacks
//
// ThreadLocalState:
//  - propagates ThreadLocalDebugInfo across threads
//  - propagates profiler_callbacks_pushed_ TLS flag used to push
//    profiling callbacks when they are needed and pop them
//    when profiling is finished
//
// Profiler callbacks:
//  - get the current profiling state (PROFILER slot in ThreadLocalDebugInfo)
//  - saves profiling events into the profiling state
//

// TLS flag keeps track of (potentially nested) profiling ranges
thread_local int64_t profiler_nested_depth_ = 0;

// Profiler state
struct ProfilerThreadLocalState : public at::DebugInfoBase {
  explicit ProfilerThreadLocalState(
      ProfilerState state,
      bool report_input_shapes)
    : config_(state, report_input_shapes) {}
  explicit ProfilerThreadLocalState(
      const ProfilerConfig& config)
    : config_(config) {}

  inline const ProfilerConfig& config() const {
    return config_;
  }

  thread_event_lists consolidate() {
    std::lock_guard<std::mutex> guard(mutex);
    thread_event_lists result;
    for (auto it = event_lists_map_.begin(); it != event_lists_map_.end(); ++it) {
      auto & list = it->second;
      result.emplace_back(list->consolidate());
    }
    return result;
  }

  void mark(
    std::string name,
    bool include_cuda = true) {
    if (config_.state == ProfilerState::Disabled) {
      return;
    }
    if (config_.state == ProfilerState::NVTX) {
      cuda_stubs->nvtxMarkA(name.c_str());
    } else {
      std::lock_guard<std::mutex> guard(mutex);
      auto& list = getEventList();
      list.record(
          EventKind::Mark,
          StringView(std::move(name)),
          RecordFunction::currentThreadId(),
          include_cuda && config_.state == ProfilerState::CUDA);
    }
  }

  void pushRange(
    const StringView& name,
    const char* msg = "",
    int64_t sequence_nr = -1,
    std::vector<std::vector<int64_t>>&& shapes = {}) {
    if (config_.state == ProfilerState::Disabled) {
      return;
    }
    if (config_.state == ProfilerState::NVTX) {
      if (sequence_nr >= 0 || shapes.size() > 0) {
        std::stringstream s;
        if (sequence_nr >= 0) {
          s << name.str() << msg << sequence_nr;
        }
        if (shapes.size() > 0) {
          s << ", sizes = [";
          for (int i = 0; i < shapes.size(); i++) {
            if (shapes[i].size() > 0) {
              s << "[";
              for (int dim = 0; dim < shapes[i].size(); dim++) {
                s << shapes[i][dim];
                if (dim < shapes[i].size() - 1) {
                  s << ", ";
                }
              }
              s << "]";
            } else {
              s << "[]";
            }
            if (i < shapes.size() - 1) {
              s << ", ";
            }
          }
          s << "]";
        }
        cuda_stubs->nvtxRangePushA(s.str().c_str());
      } else {
        cuda_stubs->nvtxRangePushA(name.str());
      }
    } else {
      std::lock_guard<std::mutex> guard(mutex);
      auto& list = getEventList();
      list.record(
          EventKind::PushRange,
          name,
          RecordFunction::currentThreadId(),
          config_.state == ProfilerState::CUDA,
          std::move(shapes));
    }
  }

  void popRange() {
    if (config_.state == ProfilerState::Disabled) {
      return;
    }
    if (config_.state == ProfilerState::NVTX) {
      cuda_stubs->nvtxRangePop();
    } else {
      std::lock_guard<std::mutex> guard(mutex);
      auto& list = getEventList();
      list.record(
          EventKind::PopRange,
          StringView(""),
          RecordFunction::currentThreadId(),
          config_.state == ProfilerState::CUDA);
    }
  }

 private:
  // not thread safe
  RangeEventList& getEventList() {
    auto thread_id = RecordFunction::currentThreadId();
    auto it = event_lists_map_.find(thread_id);
    if (it != event_lists_map_.end()) {
      return *(it->second);
    } else {
      auto event_list = std::make_shared<RangeEventList>();
      event_lists_map_[thread_id] = event_list;
      return *(event_list.get());
    }
  }

  std::mutex mutex;
  std::unordered_map<uint16_t, std::shared_ptr<RangeEventList>>
      event_lists_map_;
  ProfilerConfig config_ = ProfilerConfig(ProfilerState::Disabled, false);
};

void pushProfilingCallbacks(bool needs_inputs = false) {
  pushCallback(
      [needs_inputs](const RecordFunction& fn) {
        const auto& state = at::ThreadLocalDebugInfo::get(at::DebugInfoKind::PROFILER_STATE);
        auto state_ptr = dynamic_cast<ProfilerThreadLocalState*>(state.get());
        if (!state_ptr || state_ptr->config().state == ProfilerState::Disabled) {
          return true;
        }

        auto* msg = (fn.seqNr() >= 0) ? ", seq = " : "";
        if (needs_inputs) {
          std::vector<std::vector<int64_t>> inputSizes;
          inputSizes.reserve(fn.inputs().size());
          for (const c10::IValue& input : fn.inputs()) {
            if (!input.isTensor()) {
              inputSizes.emplace_back();
              continue;
            }
            const at::Tensor& tensor = input.toTensor();
            if (tensor.defined()) {
              inputSizes.push_back(input.toTensor().sizes().vec());
            } else {
              inputSizes.emplace_back();
            }
          }
          state_ptr->pushRange(fn.name(), msg, fn.seqNr(), std::move(inputSizes));
        } else {
          state_ptr->pushRange(fn.name(), msg, fn.seqNr(), {});
        }
        return true;
      },
      [](const RecordFunction& fn) {
        const auto& state = at::ThreadLocalDebugInfo::get(at::DebugInfoKind::PROFILER_STATE);
        auto state_ptr = dynamic_cast<ProfilerThreadLocalState*>(state.get());
        if (!state_ptr || state_ptr->config().state == ProfilerState::Disabled) {
          return;
        }
        state_ptr->popRange();
      },
      needs_inputs,
      /* scopes */ {RecordScope::FUNCTION, RecordScope::USER_SCOPE},
      CallbackKind::PROFILER);
}

void removeProfilingCallbacks() {
  removeCallback(CallbackKind::PROFILER);
}

bool unused_ = []() {
  at::ThreadLocalState::registerThreadLocalSetting(
    at::ThreadLocalSetting::PROFILER,
    []() {
      auto v = at::SettingValue();
      v.value = (int64_t)(profiler_nested_depth_ > 0);
      return v;
    },
    [](at::SettingValue v) {
      // push profiling callbacks in the child task
      // if profiling is enabled in the parent
      auto to_push = (bool)v.value;
      if (to_push) {
        if (profiler_nested_depth_ == 0) {
          pushProfilingCallbacks();
        }
        profiler_nested_depth_++;
      }
      if (!to_push) {
        --profiler_nested_depth_;
        if (profiler_nested_depth_ == 0) {
          removeProfilingCallbacks();
        }
      }
    }
  );
  return true;
}();

const int kCUDAWarmupStart = 5;

} // namespace

void registerCUDAMethods(CUDAStubs* stubs) {
  cuda_stubs = stubs;
}

ProfilerConfig::~ProfilerConfig() = default;

bool profilerEnabled() {
  const auto& state = at::ThreadLocalDebugInfo::get(at::DebugInfoKind::PROFILER_STATE);
  auto state_ptr = dynamic_cast<ProfilerThreadLocalState*>(state.get());
  return state_ptr && state_ptr->config().state != ProfilerState::Disabled;
}

void enableProfiler(const ProfilerConfig& new_config) {
  TORCH_CHECK(new_config.state != ProfilerState::NVTX || cuda_stubs->enabled(),
    "Can't use NVTX profiler - PyTorch was compiled without CUDA");

  auto state = std::make_shared<ProfilerThreadLocalState>(new_config);
  at::ThreadLocalDebugInfo::push(at::DebugInfoKind::PROFILER_STATE, state);

  if (profiler_nested_depth_ == 0) {
    pushProfilingCallbacks(new_config.report_input_shapes);
    c10::impl::tls_set_dispatch_key_included(c10::DispatchKey::Profiler, true);
  }
  ++profiler_nested_depth_;

  if (new_config.state == ProfilerState::CUDA) {
    // event recording appears to have some startup overhead, so we need to
    // to generate some dummy events first before recording synchronization events
    for (int i = 0; i < kCUDAWarmupStart; i++) {
      cuda_stubs->onEachDevice([state](int /* unused */) {
          state->mark("__cuda_startup");
          cuda_stubs->synchronize();
      });
    }

    // cuda events must be on the same device, so we need a start event recorded
    // for each gpu. we then use this event to synchronize time on the GPU
    // with the CPU clock.
    cuda_stubs->onEachDevice([state](int d) {
        state->mark("__cuda_start_event");
    });
  }
  state->mark("__start_profile", false);
}

thread_event_lists disableProfiler() {
  auto state = at::ThreadLocalDebugInfo::pop(at::DebugInfoKind::PROFILER_STATE);
  auto state_ptr = dynamic_cast<ProfilerThreadLocalState*>(state.get());
  TORCH_CHECK(state_ptr && state_ptr->config().state != ProfilerState::Disabled,
      "Can't disable profiler when it's not running");

  --profiler_nested_depth_;
  if (profiler_nested_depth_ == 0) {
    c10::impl::tls_set_dispatch_key_included(c10::DispatchKey::Profiler, false);
    removeProfilingCallbacks();
  }

  if (state_ptr->config().state == ProfilerState::NVTX) {
    return thread_event_lists();
  }

  state_ptr->mark("__stop_profile");

  return state_ptr->consolidate();
}

void Event::record(bool record_cuda) {
  if (record_cuda) {
    cuda_stubs->record(&device_, &event, &cpu_ns_);
    return;
  }
  cpu_ns_ = getTime();
}

double Event::cuda_elapsed_us(const Event & e) {
  if(!e.has_cuda() || !has_cuda()) {
    throw std::logic_error("Events were not recorded for CUDA");
  }
  if(e.device() != device()) {
    throw std::logic_error("Events are not on the same device");
  }
  return cuda_stubs->elapsed(event, e.event);
}

CUDAStubs::~CUDAStubs() = default;


static jit::CodeTemplate event_template(R"(
{
  "name": "${name}",
  "ph": "X",
  "ts": ${ts},
  "dur": ${dur},
  "tid": ${tid},
  "pid": "CPU Functions",
  "args": {}
})");


RecordProfile::RecordProfile(std::ostream& out)
: out_(out) {
  init();
}

RecordProfile::RecordProfile(const std::string& filename)
: file_(new std::ofstream(filename)), out_(*file_) {
  init();
}

void RecordProfile::init() {
  enableProfiler(ProfilerConfig(ProfilerState::CPU, false /* report shapes */));
}

RecordProfile::~RecordProfile() {
  thread_event_lists event_lists = disableProfiler();
  std::vector<Event*> events;
  for(auto& l : event_lists) {
    for(auto& e : l) {
        events.push_back(&e);
    }
  }
  processEvents(events);
  if (file_){
    file_->close();
  }
}

void RecordProfile::processEvents(const std::vector<Event*>& events) {
  TORCH_CHECK(out_, "could not open file");
  Event* start = nullptr;
  for (Event* e : events) {
    if(0 == strcmp(e->name(), "__start_profile")) {
      start = e;
      break;
    }
  }
  TORCH_CHECK(start, "could not find start?");
  std::vector<Event*> stack;
  out_ << "[\n";
  bool first = true;
  for(Event* e : events) {
    if(e->kind() == "push") {
      stack.push_back(e);
    } else if(e->kind() == "pop") {
      if(!first) {
        out_ << ",\n";
      }
      first = false;
      Event* e_start = stack.back();
      stack.pop_back();
      jit::TemplateEnv env;
      env.s("name", e_start->name());
      env.d("ts", start->cpu_elapsed_us(*e_start));
      env.d("dur", e_start->cpu_elapsed_us(*e));
      env.d("tid", e_start->thread_id());
      out_ << event_template.format(env);
    }
  }
  out_ << "]\n";
}

}}}
