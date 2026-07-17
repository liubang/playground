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
    srcs = ["master.proto"],
    deps = [":common_proto"],
)

cpp_proto_library(
    name = "master_cc_proto",
    protos = [":master_proto"],
    deps = [":common_cc_proto"],
)

proto_library(
    name = "data_proto",
    srcs = ["data.proto"],
    deps = [":common_proto"],
)

cpp_proto_library(
    name = "data_cc_proto",
    protos = [":data_proto"],
    deps = [":common_cc_proto"],
)

proto_library(
    name = "admin_proto",
    srcs = ["admin.proto"],
    deps = [":common_proto"],
)

cpp_proto_library(
    name = "admin_cc_proto",
    protos = [":admin_proto"],
    deps = [":common_cc_proto"],
)

proto_library(
    name = "unit_server_proto",
    srcs = ["unit_server.proto"],
    deps = [":common_proto"],
)

cpp_proto_library(
    name = "unit_server_cc_proto",
    protos = [":unit_server_proto"],
    deps = [":common_cc_proto"],
)

proto_library(
    name = "internal_proto",
    srcs = ["internal.proto"],
    deps = [
        ":admin_proto",
        ":common_proto",
        ":data_proto",
        ":master_proto",
    ],
)

cpp_proto_library(
    name = "internal_cc_proto",
    protos = [":internal_proto"],
    deps = [
        ":admin_cc_proto",
        ":common_cc_proto",
        ":data_cc_proto",
        ":master_cc_proto",
    ],
)
