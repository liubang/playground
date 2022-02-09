cc_binary(
    name = "test_call_once",
    srcs = [
        "test_call_once.cpp",
    ],
    copts = [
        "-std=c++17",
    ],
    linkopts = [
        "-pthread",
    ],
)

cc_test(
    name = "test_detect_class_member",
    srcs = [
        "test_detect_class_member.cpp",
        "detect_class_member.h",
    ],
    copts = [
        "-std=c++11",
    ],
    deps = [
        "@gtest//:gtest_main",
    ],
)
