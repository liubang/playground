COPTS_COMMON = [
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion-null",
    "-Wmissing-declarations",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wno-unused-local-typedefs",
    "-Wno-unused-parameter",
    "-Wunused-result",
    "-Wvarargs",
    "-Wvla",
    "-Wwrite-strings",
    "-Wno-missing-field-initializers",
    "-Wno-sign-compare",
    "-msse3",
    "-msse4",
    "-msse4.1",
    "-mpclmul",
    "-fno-builtin-memcmp",
]

C11_OPTS = [
    "-std=gnu++11",
] + COPTS_COMMON

C14_OPTS = [
    "-std=gnu++14",
] + COPTS_COMMON

C2A_OPTS = [
    "-std=gnu++2a",
] + COPTS_COMMON

cc_binary(
    name = "unique_ptr",
    srcs = ["unique_ptr.cc"],
    copts = C14_OPTS,
)

cc_binary(
    name = "weak_ptr",
    srcs = ["weak_ptr.cc"],
    copts = C14_OPTS,
)

cc_binary(
    name = "2.1-nullptr",
    srcs = ["2.1-nullptr.cc"],
    copts = C2A_OPTS,
)

cc_binary(
    name = "7.1-concurrency",
    srcs = ["7.1-concurrency.cc"],
    copts = C2A_OPTS,
    linkopts = ["-pthread"],
)

cc_binary(
    name = "7.4-condition",
    srcs = ["7.4-condition.cc"],
    copts = C2A_OPTS,
    linkopts = ["-pthread"],
)

cc_binary(
    name = "7.5-atomic",
    srcs = ["7.5-atomic.cc"],
    copts = C2A_OPTS,
    linkopts = ["-pthread"],
)

cc_binary(
    name = "7.5-atomic_is_lock_free",
    srcs = ["7.5-atomic_is_lock_free.cc"],
    copts = C2A_OPTS,
    linkopts = ["-latomic"],
)

cc_binary(
    name = "4.1-container-vector",
    srcs = ["4.1-container-vector.cc"],
    copts = C2A_OPTS,
)

cc_binary(
    name = "4.1-container-array",
    srcs = ["4.1-container-array.cc"],
    copts = C2A_OPTS,
)

cc_binary(
    name = "4.3-container-tuple",
    srcs = ["4.3-container-tuple.cc"],
    copts = C2A_OPTS,
)

cc_binary(
    name = "test_thread_pool",
    srcs = [
        "thread-pool.h",
        "test-thread-pool.cc",
    ],
    copts = C14_OPTS,
    linkopts = ["-pthread"],
)
