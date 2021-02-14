cc_library(
    name = "plugin1",
    srcs = [
        "plugin1.cc",
    ],
    hdrs = [
        "singleton.h",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_library(
    name = "plugin2",
    srcs = [
        "plugin1.cc",
    ],
    hdrs = [
        "singleton.h",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_binary(
    name = "test_dlopen",
    srcs = [
        "main.cc",
    ],
    copts = [
        "-std=gnu++14",
    ],
    linkopts = [
        "-ldl",
    ],
)
