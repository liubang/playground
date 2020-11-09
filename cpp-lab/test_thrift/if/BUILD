load("//bazel:fbthrift.bzl", "fbthrift_cc_library", "fbthrift_compile")

fbthrift_compile(
    name = "echo",
    src = "echo.thrift",
    services = [
        "EchoService",
    ],
    options = [
        "reflection",
    ],
)

fbthrift_cc_library(
    name = "echo_lib",
    src = "echo.thrift",
    services = [
        "EchoService",
    ],
    options = [
        "reflection",
    ],
    copts = [
        "-std=c++14",
    ],
    deps = [
        "@fbthrift//:thriftcpp2",
    ],
    visibility = ["//visibility:public"],
)
