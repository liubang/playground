#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/18 01:00
# Last Modified: 2023/08/18 01:00
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)
load("@rules_proto//proto:defs.bzl", "proto_library")

package(default_visibility = ["//visibility:public"])

proto_library(
    name = "echo_pb",
    srcs = ["echo.proto"],
)

cc_proto_library(
    name = "cc_echo_pb",
    deps = [":echo_pb"],
)

cc_binary(
    name = "echo_server",
    srcs = [
        "echo_server.cpp",
        "echo_service.cpp",
        "echo_service.h",
    ],
    copts = ["-std=c++17"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":cc_echo_pb",
        "@brpc",
    ],
)
