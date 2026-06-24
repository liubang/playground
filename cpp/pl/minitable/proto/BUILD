load("@rules_proto_grpc_cpp//:defs.bzl", "cpp_proto_library")

package(default_visibility = ["//cpp/pl/minitable:__subpackages__"])

proto_library(
    name = "common_proto",
    srcs = ["common.proto"],
)

cpp_proto_library(
    name = "common_cc_proto",
    protos = [":common_proto"],
)

proto_library(
    name = "master_proto",
    srcs = [
        "master.proto",
        "common.proto",
    ],
)

cpp_proto_library(
    name = "master_cc_proto",
    protos = [":master_proto"],
)

proto_library(
    name = "admin_proto",
    srcs = [
        "admin.proto",
        "common.proto",
    ],
)

cpp_proto_library(
    name = "admin_cc_proto",
    protos = [":admin_proto"],
)

proto_library(
    name = "unit_proto",
    srcs = [
        "unit.proto",
        "common.proto",
    ],
)

cpp_proto_library(
    name = "unit_cc_proto",
    protos = [":unit_proto"],
)

proto_library(
    name = "data_proto",
    srcs = [
        "data.proto",
        "common.proto",
    ],
)

cpp_proto_library(
    name = "data_cc_proto",
    protos = [":data_proto"],
)
