cc_binary(
    name = "echo_server",
    srcs = [
        "demo.cc",
        "demo.h",
        "echo_server.cc",
        "echo_service.cc",
        "echo_service.h",
    ],
    copts = [
        "-std=c++17",
    ],
    deps = [
        "//test/thrift/if:echo_lib",
        "@fbthrift//:http2",
        "@folly",
    ],
)
