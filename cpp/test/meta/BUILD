package(default_visibility = ["//visibility:public"])

[
    cc_binary(
        name = "%s" % f[:f.rfind(".")],
        srcs = [
            "%s" % f,
        ] + glob([
            "*.h",
            "include/*.h",
        ]),
        copts = [
            "-std=c++20",
            "-fno-omit-frame-pointer",
            "-fsanitize=address",
        ],
        includes = ["include"],
        linkopts = ["-fsanitize=address"],
    )
    for f in glob(["*_test.cpp"])
]
