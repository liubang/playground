load("//third_party/fbthrift:fbthrift.bzl", "fbthrift_cc_library", "fbthrift_compile")

fbthrift_compile(
    name = "echo",
    src = "echo.thrift",
    options = [
        "reflection",
    ],
    services = [
        "EchoService",
    ],
)

fbthrift_cc_library(
    name = "echo_lib",
    src = "echo.thrift",
    copts = [
        "-std=c++14",
    ],
    options = [
        "reflection",
    ],
    services = [
        "EchoService",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@fbthrift//:thriftcpp2",
    ],
)
