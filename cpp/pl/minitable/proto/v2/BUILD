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
        "common.proto",
        "master.proto",
    ],
)

cpp_proto_library(
    name = "master_cc_proto",
    protos = [":master_proto"],
)

proto_library(
    name = "data_proto",
    srcs = [
        "common.proto",
        "data.proto",
    ],
)

cpp_proto_library(
    name = "data_cc_proto",
    protos = [":data_proto"],
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
    name = "unit_server_proto",
    srcs = [
        "common.proto",
        "unit_server.proto",
    ],
)

cpp_proto_library(
    name = "unit_server_cc_proto",
    protos = [":unit_server_proto"],
)

proto_library(
    name = "internal_proto",
    srcs = [
        "admin.proto",
        "common.proto",
        "data.proto",
        "internal.proto",
        "master.proto",
    ],
)

cpp_proto_library(
    name = "internal_cc_proto",
    protos = [":internal_proto"],
)
