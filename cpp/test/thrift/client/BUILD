cc_binary(
    name = "echo_client",
    srcs = [
        "echo_client.cc",
    ],
    copts = [
        "-std=c++14",
    ],
    deps = [
        "//test/thrift/if:echo_lib",
        "@folly",
        "@fbthrift//:http2",
        "@fbthrift//:thriftperfutil",
    ],
)
