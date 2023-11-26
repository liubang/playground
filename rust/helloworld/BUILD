#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/11/26 20:59
# Last Modified: 2023/11/26 20:59
#
#======================================================================
load("@rules_rust//rust:defs.bzl", "rust_binary", "rust_test")

package(default_visibility = ["//visibility:public"])

rust_binary(
    name = "helloworld",
    srcs = [
        "src/main.rs",
    ],
    edition = "2021",
    deps = [
        "@crate_index//:num",
        "@rules_rust//tools/runfiles",
    ],
)

rust_test(
    name = "test_helloworld",
    srcs = [
        "src/main.rs",
    ],
    edition = "2021",
    deps = [
        "@crate_index//:num",
        "@rules_rust//tools/runfiles",
    ],
)
