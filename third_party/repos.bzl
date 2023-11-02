#======================================================================
#
# repos.bzl -
#
# Created by liubang on 2023/05/21 23:54
# Last Modified: 2023/05/21 23:54
#
#======================================================================

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def external_repositories():
    http_archive(
        name = "oneTBB",
        urls = ["https://github.com/oneapi-src/oneTBB/archive/c45684495599d41ba10893effa0682eceb1a3169.zip"],
        strip_prefix = "oneTBB-c45684495599d41ba10893effa0682eceb1a3169",
    )

    http_archive(
        name = "com_github_nelhage_rules_boost",
        url = "https://github.com/nelhage/rules_boost/archive/929f5412553c5295d30b16858da7cbefba0d0870.tar.gz",
        strip_prefix = "rules_boost-929f5412553c5295d30b16858da7cbefba0d0870",
    )

    http_archive(
        name = "nanobench",
        build_file = "//third_party/nanobench:nanobench.BUILD",
        strip_prefix = "nanobench-4.3.11",
        sha256 = "53a5a913fa695c23546661bf2cd22b299e10a3e994d9ed97daf89b5cada0da70",
        urls = ["https://github.com/martinus/nanobench/archive/refs/tags/v4.3.11.tar.gz"],
    )

    # liburing
    http_archive(
        name = "liburing",
        build_file = "//third_party/liburing:liburing.BUILD",
        urls = ["https://github.com/axboe/liburing/archive/liburing-0.6.tar.gz"],
        sha256 = "cf718a0a60c3a54da7ec82a0ca639a8e55d683f931b9aba9da603b849db185de",
        strip_prefix = "liburing-liburing-0.6",
        patch_args = [
            "-p0",
        ],
        patches = [
            "//third_party/liburing:liburing.patch",
        ],
    )

    http_archive(
        name = "snappy",
        build_file = "//third_party/snappy:snappy.BUILD",
        sha256 = "3dfa02e873ff51a11ee02b9ca391807f0c8ea0529a4924afa645fbf97163f9d4",
        strip_prefix = "snappy-1.1.7",
        urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/github.com/google/snappy/archive/1.1.7.tar.gz",
            "https://github.com/google/snappy/archive/1.1.7.tar.gz",
        ],
    )

    http_archive(
        name = "zlib",
        build_file = "//third_party/zlib:zlib.BUILD",
        sha256 = "c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1",
        strip_prefix = "zlib-1.2.11",
        urls = [
            "https://downloads.sourceforge.net/project/libpng/zlib/1.2.11/zlib-1.2.11.tar.gz",
            "https://zlib.net/fossils/zlib-1.2.11.tar.gz",
        ],
    )

    http_archive(
        name = "crc32c",  # 2021-10-05T19:47:30Z
        build_file = "//third_party/crc32c:crc32c.BUILD",
        sha256 = "ac07840513072b7fcebda6e821068aa04889018f24e10e46181068fb214d7e56",
        strip_prefix = "crc32c-1.1.2",
        urls = ["https://github.com/google/crc32c/archive/1.1.2.tar.gz"],
    )

    http_archive(
        name = "openssl",  # 2021-12-14T15:45:01Z
        build_file = "//third_party/openssl:openssl.BUILD",
        sha256 = "f89199be8b23ca45fc7cb9f1d8d3ee67312318286ad030f5316aca6462db6c96",
        strip_prefix = "openssl-1.1.1m",
        urls = [
            "https://www.openssl.org/source/openssl-1.1.1m.tar.gz",
            "https://github.com/openssl/openssl/archive/OpenSSL_1_1_1m.tar.gz",
        ],
    )

    http_archive(
        name = "leveldb",
        build_file = "//third_party/leveldb:leveldb.BUILD",
        sha256 = "9a37f8a6174f09bd622bc723b55881dc541cd50747cbd08831c2a82d620f6d76",
        strip_prefix = "leveldb-1.23",
        urls = [
            "https://github.com/google/leveldb/archive/refs/tags/1.23.tar.gz",
        ],
    )

    http_archive(
        name = "brpc",
        build_file = "//third_party/brpc:brpc.BUILD",
        urls = ["https://github.com/apache/brpc/archive/refs/tags/1.6.0.tar.gz"],
        sha256 = "d286d520ec4d317180d91ea3970494c1b8319c8867229e5c4784998c4536718f",
        strip_prefix = "brpc-1.6.0",
    )

    git_repository(
        name = "rules_perl",
        remote = "https://github.com/bazelbuild/rules_perl.git",
        branch = "main",
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
        name = "ragel",
        build_file = "//third_party/ragel:ragel.BUILD",
        urls = ["https://www.colm.net/files/ragel/ragel-7.0.4.tar.gz"],
        strip_prefix = "ragel-7.0.4",
        sha256 = "84b1493efe967e85070c69e78b04dc55edc5c5718f9d6b77929762cb2abed278",
    )
