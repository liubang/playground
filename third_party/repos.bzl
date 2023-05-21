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
    http_archive(
        name = "boost",
        build_file = "//third_party/boost:boost.BUILD",
        sha256 = "5347464af5b14ac54bb945dc68f1dd7c56f0dad7262816b956138fc53bcc0131",
        urls = ["https://boostorg.jfrog.io/artifactory/main/release/1.77.0/source/boost_1_77_0.tar.gz"],
        strip_prefix = "boost_1_77_0",
    )
