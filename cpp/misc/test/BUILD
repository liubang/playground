#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 23:30
# Last Modified: 2023/05/21 23:30
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_binary(
    name = "test_async",
    srcs = ["test_async.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts + [
        "-pthread",
    ],
)

cc_binary(
    name = "test_context",
    srcs = ["test_context.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
)

cc_binary(
    name = "test_named_parameters",
    srcs = ["test_named_parameters.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/tools",
    ],
)

cc_binary(
    name = "test_heap_use_after_free",
    srcs = ["test_heap_use_after_free.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
)

cc_binary(
    name = "test_liburing",
    srcs = ["test_liburing.cpp"],
    copts = common_copts + [
        "-std=c++17",
    ],
    linkopts = common_linkopts,
    deps = select({
        "//:linux_x86_64": ["@liburing"],
        "//conditions:default": [],
    }),
)

cc_proto_library(
    name = "cc_sensor_proto",
    deps = ["//proto/test:sensor"],
)

cc_binary(
    name = "test_pb",
    srcs = ["test_pb.cpp"],
    copts = common_copts + [
        "-std=c++17",
    ],
    linkopts = common_linkopts,
    deps = [":cc_sensor_proto"],
)

cc_binary(
    name = "test_bits",
    srcs = ["test_bits.cpp"],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = ["//cpp/tools"],
)

cc_binary(
    name = "test_block_builder",
    srcs = ["test_block_builder.cpp"],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        "//cpp/misc/sst:sstable",
        "@abseil-cpp//absl/cleanup",
    ],
)

cc_binary(
    name = "test_boost_coroutine2",
    srcs = ["test_boost_coroutine2.cpp"],
    copts = [
        "-std=c++20",
    ],
    # linkopts = common_linkopts,
    deps = [
        "//cpp/misc/sst:sstable",
        "@boost//:coroutine2",
    ],
)

cc_binary(
    name = "test_boost_fiber",
    srcs = ["test_boost_fiber.cpp"],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        "//cpp/misc/sst:sstable",
        "@boost//:fiber",
    ],
)
