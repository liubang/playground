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
        name = "nanobench",
        build_file = "//third_party/nanobench:nanobench.BUILD",
        strip_prefix = "nanobench-4.3.11",
        sha256 = "53a5a913fa695c23546661bf2cd22b299e10a3e994d9ed97daf89b5cada0da70",
        urls = ["https://github.com/martinus/nanobench/archive/refs/tags/v4.3.11.tar.gz"],
    )

    http_archive(
        name = "sqlite",
        build_file = "//third_party/sqlite:sqlite.BUILD",
        urls = ["https://sqlite.org/2023/sqlite-src-3420000.zip"],
        sha256 = "38ca56a317be37fb00bd92bc280d9b9209bd4008b297d483c41ec1f6079bfb6d",
        strip_prefix = "sqlite-src-3420000",
    )

    http_archive(
        name = "rules_ragel",
        urls = ["https://github.com/jmillikin/rules_ragel/archive/07490ea288899d816bddadfb2ae1393d6a9b9c1c.zip"],
        sha256 = "9891b1925a0a539bd4d5ab1e0997f42fa72b50a0483b3f2bdf39861e44f16df0",
        strip_prefix = "rules_ragel-07490ea288899d816bddadfb2ae1393d6a9b9c1c",
    )

    http_archive(
        name = "colm",
        build_file = "//third_party/colm:colm.BUILD",
        urls = ["https://www.colm.net/files/colm/colm-0.14.7.tar.gz"],
        strip_prefix = "colm-0.14.7",
        sha256 = "6037b31c358dda6f580f7321f97a182144a8401c690b458fcae055c65501977d",
    )

    http_archive(
        name = "s2geometry",
        build_file = "//third_party/s2geometry:s2geometry.BUILD",
        urls = ["https://github.com/google/s2geometry/archive/refs/tags/v0.11.1.tar.gz"],
        strip_prefix = "s2geometry-0.11.1",
        sha256 = "bdbeb8ebdb88fa934257caf81bb44b55711617a3ab4fdec2c3cfd6cc31b61734",
    )

    # http_archive(
    #     name = "ragel",
    #     build_file = "//third_party/ragel:ragel.BUILD",
    #     urls = ["https://www.colm.net/files/ragel/ragel-7.0.4.tar.gz"],
    #     strip_prefix = "ragel-7.0.4",
    #     sha256 = "84b1493efe967e85070c69e78b04dc55edc5c5718f9d6b77929762cb2abed278",
    # )
