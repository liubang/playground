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

cc_binary(
    name = "print_any_container",
    srcs = [
        "print_any_container.cpp",
    ],
    copts = _COPTS + [
        "-fcoroutines",
    ],
    linkopts = _LINKOPTS,
)

cc_binary(
    name = "vertical_symmetry",
    srcs = [
        "vertical_symmetry.cpp",
    ],
    copts = _COPTS,
    linkopts = _LINKOPTS,
)
