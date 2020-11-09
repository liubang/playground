cc_binary(
    name = "echo_client",
    srcs = [
        "echo_client.cc",
    ],
    copts = [
        "-std=c++14",
    ],
    deps = [
        "//test_thrift/if:echo_lib",
        "@folly",
        "@fbthrift//:http2",
        "@fbthrift//:thriftperfutil",
    ],
)
