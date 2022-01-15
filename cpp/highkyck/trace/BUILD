cc_binary(
    name = "test_tracing",
    srcs = [
        "tracing.cpp",
        "test_tracing.cpp",
    ],
    copts = [
        "-std=c++14",
        "-finstrument-functions",
        "-rdynamic",
    ],
    linkopts = [
        "-ldl",
    ],
)
