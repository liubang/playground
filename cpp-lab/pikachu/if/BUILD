load("//bazel:fbthrift.bzl", "fbthrift_cc_library", "fbthrift_compile")

fbthrift_cc_library(
    name = "pikachu_thrift",
    src = "pikachu.thrift",
    services = [
        "PikachuService",
    ],
    copts = [
        "-std=c++14",
    ],
    deps = [
        "@fbthrift//:thriftcpp2",
    ],
    visibility = ["//visibility:public"],
)
