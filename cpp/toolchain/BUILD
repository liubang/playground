package(default_visibility = ["//visibility:public"])

load(":cc_toolchain_config.bzl", "cc_toolchain_config")

cc_toolchain_suite(
    name = "gcc11_suite",
    toolchains = {
        "darwin_arm64": ":darwin_arm64_toolchain",
    },
)

filegroup(name = "empty")

cc_toolchain_config(name = "darwin_arm64_toolchain_config")

cc_toolchain(
    name = "darwin_arm64_toolchain",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
    toolchain_config = ":darwin_arm64_toolchain_config",
    toolchain_identifier = "darwin_arm64-toolchain",
)
