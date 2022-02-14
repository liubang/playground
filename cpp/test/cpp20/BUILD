_COPTS = [
    "-std=c++20",
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
)

cc_binary(
    name = "test_source_location",
    srcs = [
        "test_source_location.cpp",
    ],
    copts = _COPTS,
    linkopts = _LINKOPTS,
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
