class IOSVersion:
    def __init__(self, parts):
        self.parts = parts

    def render_dots(self):
        return ".".join(map(str, self.parts))


class ArchVariant:
    def __init__(self, name, is_custom=False):
        self.name = name
        self.is_custom = is_custom

    def render(self):
        extra_parts = ["custom"] if self.is_custom else []
        return "_".join([self.name] + extra_parts)


class IOSJob:
    def __init__(self, ios_version, arch_variant, is_org_member_context=True, extra_props=None):
        self.ios_version = ios_version
        self.arch_variant = arch_variant
        self.is_org_member_context = is_org_member_context
        self.extra_props = extra_props

    def gen_name_parts(self, with_version_dots):
        version_string_parts = list(map(str, self.ios_version.parts))
        version_parts = [self.ios_version.render_dots()] if with_version_dots else version_string_parts
        build_variant_suffix = "_".join([self.arch_variant.render(), "build"])

        return [
            "pytorch",
            "ios",
        ] + version_parts + [
            build_variant_suffix,
        ]

    def gen_tree(self):

        platform_name = "SIMULATOR" if self.arch_variant.name == "x86_64" else "OS"

        props_dict = {
            "build_environment": "-".join(self.gen_name_parts(True)),
            "ios_arch": self.arch_variant.name,
            "ios_platform": platform_name,
            "name": "_".join(self.gen_name_parts(False)),
            "requires": ["setup"],
        }

        if self.is_org_member_context:
            props_dict["context"] = "org-member"

        if self.extra_props:
            props_dict.update(self.extra_props)

        return [{"pytorch_ios_build": props_dict}]


WORKFLOW_DATA = [
    IOSJob(IOSVersion([11, 2, 1]), ArchVariant("x86_64"), is_org_member_context=False),
    IOSJob(IOSVersion([11, 2, 1]), ArchVariant("arm64")),
    IOSJob(IOSVersion([11, 2, 1]), ArchVariant("arm64", True), extra_props={"op_list": "mobilenetv2.yaml"}),
]


def get_workflow_jobs():
    return [item.gen_tree() for item in WORKFLOW_DATA]
