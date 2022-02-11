cc_binary(
    name = "brain_fuck",
    srcs = [
        "brain_fuck.cc",
    ],
    copts = [
        "-std=gnu++17",
    ],
)

cc_binary(
    name = "test_yabf_add3",
    srcs = [
        "test_yabf_add3.cpp",
        "yabf.h",
    ],
    copts = [
        "-std=c++17",
        "-fno-omit-frame-pointer",
        "-fsanitize=address",
    ],
    linkopts = [
        "-fsanitize=address",
    ],
)
