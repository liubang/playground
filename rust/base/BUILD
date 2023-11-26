#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/11/26 14:51
# Last Modified: 2023/11/26 14:51
#
#======================================================================
load("@rules_rust//rust:defs.bzl", "rust_binary")

package(default_visibility = ["//visibility:public"])

[
    rust_binary(
        name = "%s" % f[:f.rfind(".")].replace("/", "_"),
        srcs = [
            "%s" % f,
        ],
        edition = "2021",
        deps = [
            "@crate_index//:rand",
            "@rules_rust//tools/runfiles",
        ],
    )
    for f in glob(["src/bin/*.rs"])
]
