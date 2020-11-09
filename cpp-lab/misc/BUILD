cc_binary(
    name = "test_signal",
    srcs = ["test_signal.cc"],
)

cc_binary(
    name = "test_oop",
    srcs = [
        "test_oop.cc",
    ],
)

cc_binary(
    name = "test_file_info",
    srcs = [
        "test_file_info.cc",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_binary(
    name = "test_string",
    srcs = [
        "test_string.cc",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_library(
    name = "plugin1",
    srcs = [
        "dlopen/plugin1.cc",
    ],
    hdrs = [
        "dlopen/singleton.h",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_library(
    name = "plugin2",
    srcs = [
        "dlopen/plugin1.cc",
    ],
    hdrs = [
        "dlopen/singleton.h",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_binary(
    name = "test_dlopen",
    srcs = [
        "dlopen/main.cc",
    ],
    copts = [
        "-std=gnu++14",
    ],
    linkopts = [
        "-ldl",
    ],
)
