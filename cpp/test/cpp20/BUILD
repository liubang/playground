_COPTS = [
    "-std=c++17",
    "-fsanitize=address",
    "-fno-omit-frame-pointer",
]

_LINKOPTS = [
    "-fsanitize=address",
    "-pthread",
]

cc_binary(
    name = "test_coroutines",
    srcs = [
        "test_coroutines.cpp",
    ],
    copts = _COPTS,
    linkopts = _LINKOPTS,
    deps = [
        "@gtest//:gtest_main",
    ],
)

# cc_test(
#     name = "test_coroutines",
#     srcs = [
#         "test_coroutines.cpp",
#     ],
#     copts = _COPTS,
#     linkopts = _LINKOPTS,
#     deps = [
#         "@gtest//:gtest_main",
#     ],
# )
