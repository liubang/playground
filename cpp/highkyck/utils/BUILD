package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "ip_test",
    srcs = [
        "ip.h",
        "ip_test.cpp",
    ],
    copts = [
        "-std=c++17",
        "-fsanitize=address",
        "-fno-omit-frame-pointer",
    ],
    linkopts = [
        "-fsanitize=address",
    ],
)
