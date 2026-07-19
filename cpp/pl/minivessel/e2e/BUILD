load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_proto_grpc_cpp//:defs.bzl", "cpp_proto_library")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS", "TEST_COPTS")

package(default_visibility = ["//visibility:public"])

proto_library(
    name = "e2e_proto",
    srcs = ["minivessel_e2e.proto"],
)

cpp_proto_library(
    name = "e2e_cc_proto",
    protos = [":e2e_proto"],
)

cc_library(
    name = "object_layout",
    srcs = ["object_layout.cpp"],
    hdrs = ["object_layout.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minivessel:shared_wal",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "remote_shared_wal",
    srcs = ["remote_shared_wal.cpp"],
    hdrs = ["remote_shared_wal.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":e2e_cc_proto",
        "//cpp/pl/minivessel:shared_wal",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@brpc",
    ],
)

cc_library(
    name = "counter_state_machine",
    srcs = ["counter_state_machine.cpp"],
    hdrs = ["counter_state_machine.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minivessel:replica_runtime",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_binary(
    name = "shared_log_server",
    srcs = ["shared_log_server.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":e2e_cc_proto",
        ":object_layout",
        "//cpp/pl/minivessel:minidfs_object_store",
        "//cpp/pl/minivessel:types",
        "@abseil-cpp//absl/status",
        "@brpc",
        "@gflags",
    ],
)

cc_binary(
    name = "counter_replica",
    srcs = ["counter_replica.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":counter_state_machine",
        ":e2e_cc_proto",
        ":remote_shared_wal",
        "//cpp/pl/minivessel:replica_runtime",
        "@brpc",
        "@gflags",
    ],
)

cc_binary(
    name = "control",
    srcs = ["control.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":e2e_cc_proto",
        "@brpc",
        "@gflags",
    ],
)

cc_test(
    name = "object_layout_test",
    srcs = ["object_layout_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":object_layout",
        "@googletest//:gtest_main",
    ],
)
