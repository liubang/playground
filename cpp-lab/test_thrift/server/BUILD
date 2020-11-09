cc_binary(
    name = "echo_server",
    srcs = [
        "echo_service.cc",
        "echo_service.h",
        "echo_server.cc",
        "demo.h",
        "demo.cc",
    ],
    copts = [
        "-std=c++17",
    ],
    deps = [
        "//test_thrift/if:echo_lib",
        "@folly",
        "@fbthrift//:http2",
    ],
)
