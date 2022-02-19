_COPTS = [
    "-std=c++20",
    "-fsanitize=address",
    "-fno-omit-frame-pointer",
]

_LINKOPTS = [
    "-fsanitize=address",
]

cc_binary(
    name = "test_coroutines",
    srcs = [
        "test_coroutines.cpp",
    ],
    copts = _COPTS + [
        "-fcoroutines",
    ],
    linkopts = _LINKOPTS,
)
