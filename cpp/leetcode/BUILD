# vim: ft=bzl
package(default_visibility = ["//visibility:public"])

[
    cc_test(
        name = "%s" % f[0:f.rfind(".")],
        srcs = [
            "%s" % f,
        ],
        copts = [
            "-std=gnu++17",
            "-w",
        ],
        deps = [
            "@gtest//:gtest_main",
        ],
    )
    for f in glob(["*.cc"])
]
