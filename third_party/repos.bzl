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
    # boost
    # http_archive(
    #     name = "boost",
    #     build_file = "//third_party/boost:boost.BUILD",
    #     urls = ["https://boostorg.jfrog.io/artifactory/main/release/1.82.0/source/boost_1_82_0.tar.gz"],
    #     sha256 = "66a469b6e608a51f8347236f4912e27dc5c60c60d7d53ae9bfe4683316c6f04c",
    #     strip_prefix = "boost_1_82_0",
    # )

    http_archive(
        name = "com_github_nelhage_rules_boost",
        url = "https://github.com/nelhage/rules_boost/archive/929f5412553c5295d30b16858da7cbefba0d0870.tar.gz",
        strip_prefix = "rules_boost-929f5412553c5295d30b16858da7cbefba0d0870",
    )

    # Google benchmark.
    http_archive(
        name = "com_github_google_benchmark",  # 2023-01-10T16:48:17Z
        sha256 = "ede6830512f21490eeea1f238f083702eb178890820c14451c1c3d69fd375b19",
        strip_prefix = "benchmark-a3235d7b69c84e8c9ff8722a22b8ac5e1bc716a6",
        urls = ["https://github.com/google/benchmark/archive/a3235d7b69c84e8c9ff8722a22b8ac5e1bc716a6.zip"],
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
