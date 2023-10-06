#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 00:36
# Last Modified: 2023/05/21 00:36
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "ip_test",
    srcs = [
        "ip_test.cpp",
    ],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

# the thread pool can only be running >= c++11 and <= c++17
cc_test(
    name = "thread_pool_test",
    srcs = ["thread_pool_test.cpp"],
    copts = ["-std=c++14"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "numbers_test",
    srcs = ["numbers_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "binary_test",
    srcs = ["binary_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "status_test",
    srcs = ["status_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "scope_test",
    srcs = ["scope_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "encoding_test",
    srcs = ["encoding_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "demangle_test",
    srcs = ["demangle_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "//cpp/tools",
    ],
)
