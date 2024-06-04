#======================================================================
#
# repos.bzl -
#
# Created by liubang on 2023/05/21 23:54
# Last Modified: 2023/05/21 23:54
#
#======================================================================

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def external_repositories():
    http_archive(
        name = "rules_ragel",
        urls = ["https://github.com/jmillikin/rules_ragel/archive/07490ea288899d816bddadfb2ae1393d6a9b9c1c.zip"],
        sha256 = "9891b1925a0a539bd4d5ab1e0997f42fa72b50a0483b3f2bdf39861e44f16df0",
        strip_prefix = "rules_ragel-07490ea288899d816bddadfb2ae1393d6a9b9c1c",
    )
