#======================================================================
#
# repos.bzl -
#
# Created by liubang on 2023/05/21 23:54
# Last Modified: 2023/05/21 23:54
#
#======================================================================

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

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

    http_archive(
        name = "rules_perl",  # 2021-09-23T03:21:58Z
        sha256 = "55fbe071971772758ad669615fc9aac9b126db6ae45909f0f36de499f6201dd3",
        strip_prefix = "rules_perl-2f4f36f454375e678e81e5ca465d4d497c5c02da",
        urls = [
            "https://github.com/bazelbuild/rules_perl/archive/2f4f36f454375e678e81e5ca465d4d497c5c02da.tar.gz",
        ],
    )
