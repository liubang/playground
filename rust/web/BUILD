#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/11/26 21:00
# Last Modified: 2023/11/26 21:00
#
#======================================================================

load("@rules_rust//rust:defs.bzl", "rust_binary")

package(default_visibility = ["//visibility:public"])

rust_binary(
    name = "web",
    srcs = [
        "src/handlers/index.rs",
        "src/handlers/mod.rs",
        "src/handlers/model.rs",
        "src/main.rs",
    ],
    edition = "2021",
    deps = [
        "@crate_index//:actix-web",
        "@crate_index//:serde",
        "@rules_rust//tools/runfiles",
    ],
)
