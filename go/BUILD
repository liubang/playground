#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 01:46
# Last Modified: 2023/05/21 01:46
#
#======================================================================

load("@bazel_gazelle//:def.bzl", "gazelle")

# gazelle:prefix github.com/playground/go
gazelle(name = "gazelle")

gazelle(
    name = "gazelle-update-repos",
    args = [
        "-from_file=go.mod",
        "-to_macro=go_deps.bzl%go_dependencies",
        "-prune",
    ],
    command = "update-repos",
)
