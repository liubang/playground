# vim: ft=bzl

package(default_visibility = ["//visibility:public"])

# [
#     cc_test(
#         name = "%s" % f[0:f.rfind(".")],
#         srcs = [
#             "%s" % f,
#         ] + glob([
#             "include/*.h",
#         ]),
#         copts = [
#             "-std=gnu++17",
#             "-fno-omit-frame-pointer",
#             "-fsanitize=address",
#         ],
#         linkopts = [
#             "-fsanitize=address",
#         ],
#         deps = [
#             "@com_google_googletest//:gtest_main",
#         ],
#     )
#     for f in glob(["*.cc"])
# ]
#

_common_copts = [
    "-fno-omit-frame-pointer",
    "-fsanitize=address",
]

_common_linkopts = [
    "-fsanitize=address",
]

cc_test(
    name = "test_call_once",
    srcs = [
        "test_call_once.cpp",
    ],
    copts = _common_copts + [
        "-std=c++17",
    ],
    linkopts = _common_linkopts + [
        "-pthread",
    ],
    deps = [
        "@gtest//:gtest_main",
    ],
)

cc_test(
    name = "test_detect_class_member",
    srcs = [
        "test_detect_class_member.cpp",
        "detect_class_member.h",
    ],
    copts = _common_copts + [
        "-std=c++11",
    ],
    linkopts = _common_linkopts,
    deps = [
        "@gtest//:gtest_main",
    ],
)
