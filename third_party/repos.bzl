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
        urls = ["https://github.com/google/snappy/archive/refs/tags/1.1.10.tar.gz"],
        sha256 = "49d831bffcc5f3d01482340fe5af59852ca2fe76c3e05df0e67203ebbe0f1d90",
        strip_prefix = "snappy-1.1.10",
        build_file = "//third_party/snappy:snappy.BUILD",
    )
