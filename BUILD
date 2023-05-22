#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 02:05
# Last Modified: 2023/05/21 02:05
#
#======================================================================

load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

licenses(["notice"])

exports_files([
    "LICENSE",
    "MODULE.bazel",
    "WORKSPACE",
])

refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = {
        "//cpp/...": "",
    },
)
