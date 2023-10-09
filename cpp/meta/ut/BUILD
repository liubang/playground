#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 00:59
# Last Modified: 2023/05/21 00:59
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "expression_test",
    srcs = [
        "expression_test.cpp",
    ],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "traits_test",
    srcs = [
        "traits_test.cpp",
        "traits_test.h",
    ],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "utils_test",
    srcs = [
        "utils_test.cpp",
        "utils_test.h",
    ],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "tuple_iteration_apply_test",
    srcs = ["tuple_iteration_apply_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

# cc_test(
#     name = "tuple_iteration_basics_test",
#     srcs = ["tuple_iteration_basics_test.cpp"],
#     copts = ["-std=c++20"] + TEST_COPTS,
#     linkopts = DEFAULT_LINKOPTS,
#     deps = [
#         "//cpp/meta",
#         "@catch2//:catch2_main",
#     ],
# )

cc_test(
    name = "type_list_test",
    srcs = ["type_list_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "function_test",
    srcs = ["function_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)
