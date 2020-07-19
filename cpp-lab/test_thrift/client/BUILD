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
        "@proxygen//:proxygenhttpserver",
        "@fbthrift//:thriftperfutil",
        "@fbthrift//:thriftcpp2",
    ],
)
