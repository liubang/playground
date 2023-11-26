#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/11/26 20:59
# Last Modified: 2023/11/26 20:59
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
        deps = ["@rules_rust//tools/runfiles"],
    )
    for f in glob(["src/*.rs"])
]
