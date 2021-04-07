[
    cc_binary(
        name = "%s" % f[0:f.rfind(".")],
        srcs = [
            "%s" % f,
        ] + glob([
            "includes/*.h",
        ]),
        includes = [
            "includes",
        ],
        copts = [
            "-std=gnu++17",
            "-w",
        ],
    )
    for f in glob(["test_*.cc"])
]

[
    cc_test(
        name = "%s" % f[0:f.rfind(".")],
        srcs = [
            "%s" % f,
        ] + glob([
            "includes/*.h",
        ]),
        includes = [
            "includes",
        ],
        copts = [
            "-std=gnu++17",
            "-w",
        ],
        deps = [
            "@gtest//:gtest_main",
        ],
    )
    for f in glob(["*_test.cc"])
]
