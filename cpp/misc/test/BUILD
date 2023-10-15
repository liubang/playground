#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 23:30
# Last Modified: 2023/05/21 23:30
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_test(
    name = "test_async",
    srcs = ["test_async.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS + ["-pthread"],
)

cc_test(
    name = "test_context",
    srcs = ["test_context.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)

cc_test(
    name = "test_named_parameters",
    srcs = ["test_named_parameters.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@boost//:mpl",
        "@boost//:parameter",
        "@boost//:utility",
    ],
)

cc_binary(
    name = "test_heap_use_after_free",
    srcs = ["test_heap_use_after_free.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)

cc_test(
    name = "test_liburing",
    srcs = ["test_liburing.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = select({
        "//:linux_x86_64": ["@liburing"],
        "//conditions:default": [],
    }),
)

cc_proto_library(
    name = "cc_sensor_proto",
    deps = ["//proto/test:sensor"],
)

cc_test(
    name = "test_pb",
    srcs = ["test_pb.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [":cc_sensor_proto"],
)

cc_test(
    name = "test_bits",
    srcs = ["test_bits.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = ["//cpp/tools"],
)

cc_test(
    name = "test_block_builder",
    srcs = ["test_block_builder.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/misc/sst:sstable",
        "//cpp/tools",
        # "@abseil-cpp//absl/cleanup",
    ],
)

cc_test(
    name = "test_boost_coroutine2",
    srcs = ["test_boost_coroutine2.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@boost//:coroutine2",
    ],
)

cc_test(
    name = "test_boost_fiber",
    srcs = ["test_boost_fiber.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@boost//:fiber",
    ],
)

cc_test(
    name = "test_fs_path",
    srcs = ["test_fs_path.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)

cc_test(
    name = "test_map",
    srcs = ["test_map.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)

cc_test(
    name = "test_bitset",
    srcs = ["test_bitset.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)

cc_test(
    name = "test_pack",
    srcs = ["test_pack.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
    ],
)

cc_test(
    name = "test_multimap",
    srcs = ["test_multimap.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)

cc_test(
    name = "test_template_lazy",
    srcs = ["test_template_lazy.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)
