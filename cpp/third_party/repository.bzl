load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def clean_dep(dep):
    return str(Label(dep))

def external_dependencies():
    http_archive(
        name = "rules_foreign_cc",
        strip_prefix = "rules_foreign_cc-74b146dc87d37baa1919da1e8f7b8aafbd32acd9",
        urls = ["https://github.com/bazelbuild/rules_foreign_cc/archive/74b146dc87d37baa1919da1e8f7b8aafbd32acd9.zip"],
        sha256 = "2de65ab702ebd0094da3885aae2a6a370df5edb4c9d0186096de79dffb356dbc",
    )

def external_repositories(path_prefix = "", repo_name = ""):
    # boost
    http_archive(
        name = "boost",
        build_file = clean_dep("//third_party:boost.BUILD"),
        sha256 = "9995e192e68528793755692917f9eb6422f3052a53c5e13ba278a228af6c7acf",
        urls = ["https://dl.bintray.com/boostorg/release/1.73.0/source/boost_1_73_0.tar.gz"],
        strip_prefix = "boost_1_73_0",
    )

    # openssl
    http_archive(
        name = "openssl",
        build_file = clean_dep("//third_party:openssl.BUILD"),
        sha256 = "ddb04774f1e32f0c49751e21b67216ac87852ceb056b75209af2443400636d46",
        urls = ["https://www.openssl.org/source/openssl-1.1.1g.tar.gz"],
        strip_prefix = "openssl-1.1.1g",
    )

    # zlib
    http_archive(
        name = "zlib",
        build_file = clean_dep("//third_party:zlib.BUILD"),
        sha256 = "c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1",
        strip_prefix = "zlib-1.2.11",
        urls = ["https://zlib.net/zlib-1.2.11.tar.gz"],
    )

    # jemalloc
    http_archive(
        name = "jemalloc",
        build_file = clean_dep("//third_party:jemalloc.BUILD"),
        sha256 = "34330e5ce276099e2e8950d9335db5a875689a4c6a56751ef3b1d8c537f887f6",
        strip_prefix = "jemalloc-5.2.1",
        urls = ["https://github.com/jemalloc/jemalloc/releases/download/5.2.1/jemalloc-5.2.1.tar.bz2"],
    )

    # snappy
    http_archive(
        name = "snappy",
        build_file = clean_dep("//third_party:snappy.BUILD"),
        sha256 = "3dfa02e873ff51a11ee02b9ca391807f0c8ea0529a4924afa645fbf97163f9d4",
        strip_prefix = "snappy-1.1.7",
        urls = ["https://github.com/google/snappy/archive/1.1.7.tar.gz"],
    )

    # zstd
    http_archive(
        name = "zstd",
        build_file = clean_dep("//third_party:zstd.BUILD"),
        urls = ["https://github.com/facebook/zstd/releases/download/v1.4.4/zstd-1.4.4.tar.gz"],
        sha256 = "59ef70ebb757ffe74a7b3fe9c305e2ba3350021a918d168a046c6300aeea9315",
        strip_prefix = "zstd-1.4.4",
    )

    # libaio
    http_archive(
        name = "libaio",
        build_file = clean_dep("//third_party:libaio.BUILD"),
        urls = ["https://pagure.io/libaio/archive/libaio-0.3.111/libaio-libaio-0.3.111.tar.gz"],
        sha256 = "e6bc17cba66e59085e670fea238ad095766b412561f90b354eb4012d851730ba",
        strip_prefix = "libaio-libaio-0.3.111",
    )

    # lz4
    http_archive(
        name = "lz4",
        build_file = clean_dep("//third_party:lz4.BUILD"),
        urls = ["https://github.com/lz4/lz4/archive/v1.9.2.tar.gz"],
        sha256 = "658ba6191fa44c92280d4aa2c271b0f4fbc0e34d249578dd05e50e76d0e5efcc",
        strip_prefix = "lz4-1.9.2",
    )

    # bzip2
    http_archive(
        name = "bzip2",
        build_file = clean_dep("//third_party:bzip2.BUILD"),
        urls = ["https://versaweb.dl.sourceforge.net/project/bzip2/bzip2-1.0.6.tar.gz"],
        sha256 = "a2848f34fcd5d6cf47def00461fcb528a0484d8edef8208d6d2e2909dc61d9cd",
        strip_prefix = "bzip2-1.0.6",
    )

    # libdwarf
    http_archive(
        name = "dwarf",
        build_file = clean_dep("//third_party:dwarf.BUILD"),
        urls = ["https://www.prevanders.net/libdwarf-20191104.tar.gz"],
        sha256 = "86119a9f7c409dc31e02d12c1b3906b1fce0dcb4db3d7e65ebe1bae585cf08f8",
        strip_prefix = "libdwarf-20191104",
    )

    # rocksdb
    http_archive(
        name = "rocksdb",
        build_file = clean_dep("//third_party:rocksdb.BUILD"),
        urls = ["https://github.com/facebook/rocksdb/archive/v6.8.1.tar.gz"],
        sha256 = "ca192a06ed3bcb9f09060add7e9d0daee1ae7a8705a3d5ecbe41867c5e2796a2",
        strip_prefix = "rocksdb-6.8.1",
    )

    # fmt
    http_archive(
        name = "fmt",
        build_file = clean_dep("//third_party:fmt.BUILD"),
        urls = ["https://github.com/fmtlib/fmt/archive/6.1.2.tar.gz"],
        sha256 = "1cafc80701b746085dddf41bd9193e6d35089e1c6ec1940e037fcb9c98f62365",
        strip_prefix = "fmt-6.1.2",
    )

    # double-conversion
    http_archive(
        name = "double-conversion",
        urls = ["https://github.com/google/double-conversion/archive/v3.1.5.tar.gz"],
        build_file = clean_dep("//third_party:double-conversion.BUILD"),
        sha256 = "a63ecb93182134ba4293fd5f22d6e08ca417caafa244afaa751cbfddf6415b13",
        strip_prefix = "double-conversion-3.1.5",
    )

    # libevent
    http_archive(
        name = "libevent",
        build_file = clean_dep("//third_party:libevent.BUILD"),
        urls = ["https://github.com/libevent/libevent/releases/download/release-2.1.11-stable/libevent-2.1.11-stable.tar.gz"],
        sha256 = "a65bac6202ea8c5609fd5c7e480e6d25de467ea1917c08290c521752f147283d",
        strip_prefix = "libevent-2.1.11-stable",
    )

    # liburing
    http_archive(
        name = "liburing",
        build_file = clean_dep("//third_party:liburing.BUILD"),
        urls = ["https://github.com/axboe/liburing/archive/liburing-0.6.tar.gz"],
        sha256 = "cf718a0a60c3a54da7ec82a0ca639a8e55d683f931b9aba9da603b849db185de",
        strip_prefix = "liburing-liburing-0.6",
        patch_args = [
            "-p0",
        ],
        patches = [
            clean_dep("//third_party:liburing.patch"),
        ],
    )

    # folly
    http_archive(
        name = "folly",
        build_file = clean_dep("//third_party:folly.BUILD"),
        urls = ["https://github.com/facebook/folly/archive/v2020.04.13.00.tar.gz"],
        sha256 = "369d17a6603c1dc53db006bd5d613461b76db089bd90a85a713565c263497082",
        strip_prefix = "folly-2020.04.13.00",
    )

    http_archive(
        name = "rsocket-cpp",
        build_file = clean_dep("//third_party:rsocket-cpp.BUILD"),
        urls = ["https://github.com/rsocket/rsocket-cpp/archive/e377f18abb03a885196385fada0329b50379c8ae.zip"],
        sha256 = "06e4aae7a6eeafdc1ec17f2c73941e799dedb482e870acc0576d05c74138dcbf",
        strip_prefix = "rsocket-cpp-e377f18abb03a885196385fada0329b50379c8ae",
    )

    # fbthrift
    http_archive(
        name = "fbthrift",
        build_file = clean_dep("//third_party:fbthrift.BUILD"),
        sha256 = "0fc6cc1673209f4557e081597b2311f6c9f153840c4e55ac61a669e10207e2ee",
        strip_prefix = "fbthrift-2020.04.13.00",
        url = "https://github.com/facebook/fbthrift/archive/v2020.04.13.00.tar.gz",
    )

    # proxygen
    http_archive(
        name = "proxygen",
        build_file = clean_dep("//third_party:proxygen.BUILD"),
        urls = ["https://github.com/facebook/proxygen/archive/v2020.04.13.00.tar.gz"],
        sha256 = "c8ac12aed526c3e67b9424a358dac150958e727feb2b3d1b8b3407ea0d53e315",
        strip_prefix = "proxygen-2020.04.13.00",
    )

    # wangle
    http_archive(
        name = "wangle",
        build_file = clean_dep("//third_party:wangle.BUILD"),
        urls = ["https://github.com/facebook/wangle/archive/v2020.04.13.00.tar.gz"],
        strip_prefix = "wangle-2020.04.13.00",
        sha256 = "a046dfea92f453bd12b28dad287a7eb86e782c4db9518b90c33c5320b3868f0b",
    )

    # fizz
    http_archive(
        name = "fizz",
        build_file = clean_dep("//third_party:fizz.BUILD"),
        strip_prefix = "fizz-2020.04.13.00",
        urls = ["https://github.com/facebookincubator/fizz/archive/v2020.04.13.00.tar.gz"],
        sha256 = "62d3f5ff24c32e373771ee33a7c4f394b56536d941ac476f774f62b6189d6ce5",
    )

    # libsodium
    http_archive(
        name = "libsodium",
        url = "https://github.com/jedisct1/libsodium/archive/1.0.16.tar.gz",
        build_file = clean_dep("//third_party:libsodium.BUILD"),
        strip_prefix = "libsodium-1.0.16",
        sha256 = "0c14604bbeab2e82a803215d65c3b6e74bb28291aaee6236d65c699ccfe1a98c",
    )

    # argon2
    http_archive(
        name = "argon2",
        build_file = clean_dep("//third_party:argon2.BUILD"),
        sha256 = "eaea0172c1f4ee4550d1b6c9ce01aab8d1ab66b4207776aa67991eb5872fdcd8",
        strip_prefix = "phc-winner-argon2-20171227",
        url = "https://github.com/P-H-C/phc-winner-argon2/archive/20171227.tar.gz",
    )

    # glog
    http_archive(
        name = "glog",
        build_file = clean_dep("//third_party:glog.BUILD"),
        urls = ["https://github.com/google/glog/archive/v0.4.0.tar.gz"],
        sha256 = "f28359aeba12f30d73d9e4711ef356dc842886968112162bc73002645139c39c",
        strip_prefix = "glog-0.4.0",
    )

    # gflags
    http_archive(
        name = "gflags",
        build_file = clean_dep("//third_party:gflags.BUILD"),
        urls = ["https://github.com/gflags/gflags/archive/v2.2.2.tar.gz"],
        sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
        strip_prefix = "gflags-2.2.2",
    )

    http_archive(
        name = "cityhash",
        build_file = clean_dep("//third_party:cityhash.BUILD"),
        urls = ["https://github.com/google/cityhash/archive/8af9b8c2b889d80c22d6bc26ba0df1afb79a30db.zip"],
        sha256 = "3524f5ed43143974a29fddeeece29c8b6348f05db08dd180452da01a2837ddce",
        strip_prefix = "cityhash-8af9b8c2b889d80c22d6bc26ba0df1afb79a30db",
    )

    # GoogleTest/GoogleMock
    http_archive(
        name = "gtest",
        urls = ["https://github.com/google/googletest/archive/b6cd405286ed8635ece71c72f118e659f4ade3fb.zip"],
        strip_prefix = "googletest-b6cd405286ed8635ece71c72f118e659f4ade3fb",
        sha256 = "ff7a82736e158c077e76188232eac77913a15dac0b22508c390ab3f88e6d6d86",
    )

    # Google benchmark
    http_archive(
        name = "com_github_google_benchmark",
        urls = ["https://github.com/google/benchmark/archive/16703ff83c1ae6d53e5155df3bb3ab0bc96083be.zip"],
        strip_prefix = "benchmark-16703ff83c1ae6d53e5155df3bb3ab0bc96083be",
        sha256 = "59f918c8ccd4d74b6ac43484467b500f1d64b40cc1010daa055375b322a43ba3",
    )
