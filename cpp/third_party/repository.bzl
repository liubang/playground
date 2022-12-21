#======================================================================
#
# repository.bzl -
#
# Created by liubang on 2022/12/21 23:10
# Last Modified: 2022/12/21 23:10
#
#======================================================================

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def clean_dep(dep):
    return str(Label(dep))

def external_dependencies():
    http_archive(
        name = "rules_foreign_cc",
        strip_prefix = "rules_foreign_cc-0.9.0",
        url = "https://github.com/bazelbuild/rules_foreign_cc/archive/0.9.0.tar.gz",
        sha256 = "2a4d07cd64b0719b39a7c12218a3e507672b82a97b98c6a89d38565894cf7c51",
    )

def external_repositories(path_prefix = "", repo_name = ""):
    http_archive(
        name = "cimg",
        urls = ["https://github.com/dtschump/CImg/archive/refs/tags/v.2.9.9.tar.gz"],
        strip_prefix = "CImg-v.2.9.9",
        sha256 = "94c27f697826bff965ebbccb50e3e45f1f5602400e74ed586e77d9b4dcbc64d2",
    )

    # libunwind
    http_archive(
        name = "libunwind",
        build_file = clean_dep("//third_party/libunwind:libunwind.BUILD"),
        urls = ["https://github.com/libunwind/libunwind/releases/download/v1.5/libunwind-1.5.0.tar.gz"],
        sha256 = "90337653d92d4a13de590781371c604f9031cdb50520366aa1e3a91e1efb1017",
        strip_prefix = "libunwind-1.5.0",
    )

    # liblzma
    http_archive(
        name = "xz-utils",
        build_file = clean_dep("//third_party/xz-utils:xz-utils.BUILD"),
        urls = ["https://tukaani.org/xz/xz-5.2.5.tar.gz"],
        sha256 = "f6f4910fd033078738bd82bfba4f49219d03b17eb0794eb91efbae419f4aba10",
        strip_prefix = "xz-5.2.5",
    )

    # boost
    http_archive(
        name = "boost",
        build_file = clean_dep("//third_party/boost:boost.BUILD"),
        sha256 = "5347464af5b14ac54bb945dc68f1dd7c56f0dad7262816b956138fc53bcc0131",
        urls = ["https://boostorg.jfrog.io/artifactory/main/release/1.77.0/source/boost_1_77_0.tar.gz"],
        strip_prefix = "boost_1_77_0",
    )

    # openssl
    http_archive(
        name = "openssl",
        build_file = clean_dep("//third_party/openssl:openssl.BUILD"),
        sha256 = "ddb04774f1e32f0c49751e21b67216ac87852ceb056b75209af2443400636d46",
        urls = ["https://www.openssl.org/source/openssl-1.1.1g.tar.gz"],
        strip_prefix = "openssl-1.1.1g",
    )

    # zlib
    http_archive(
        name = "zlib",
        build_file = clean_dep("//third_party/zlib:zlib.BUILD"),
        sha256 = "c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1",
        strip_prefix = "zlib-1.2.11",
        urls = ["https://zlib.net/zlib-1.2.11.tar.gz"],
    )

    # jemalloc
    http_archive(
        name = "jemalloc",
        build_file = clean_dep("//third_party/jemalloc:jemalloc.BUILD"),
        sha256 = "34330e5ce276099e2e8950d9335db5a875689a4c6a56751ef3b1d8c537f887f6",
        strip_prefix = "jemalloc-5.2.1",
        urls = ["https://github.com/jemalloc/jemalloc/releases/download/5.2.1/jemalloc-5.2.1.tar.bz2"],
    )

    # snappy
    http_archive(
        name = "snappy",
        build_file = clean_dep("//third_party/snappy:snappy.BUILD"),
        sha256 = "3dfa02e873ff51a11ee02b9ca391807f0c8ea0529a4924afa645fbf97163f9d4",
        strip_prefix = "snappy-1.1.7",
        urls = ["https://github.com/google/snappy/archive/1.1.7.tar.gz"],
    )

    # zstd
    http_archive(
        name = "zstd",
        build_file = clean_dep("//third_party/zstd:zstd.BUILD"),
        urls = ["https://github.com/facebook/zstd/releases/download/v1.4.4/zstd-1.4.4.tar.gz"],
        sha256 = "59ef70ebb757ffe74a7b3fe9c305e2ba3350021a918d168a046c6300aeea9315",
        strip_prefix = "zstd-1.4.4",
    )

    # libaio
    http_archive(
        name = "libaio",
        build_file = clean_dep("//third_party/libaio:libaio.BUILD"),
        urls = ["https://pagure.io/libaio/archive/libaio-0.3.111/libaio-libaio-0.3.111.tar.gz"],
        sha256 = "e6bc17cba66e59085e670fea238ad095766b412561f90b354eb4012d851730ba",
        strip_prefix = "libaio-libaio-0.3.111",
        patches = [
            "//third_party/libaio:libaio.patch",
        ],
        patch_args = [
            "-p1",
        ],
    )

    # lz4
    http_archive(
        name = "lz4",
        build_file = clean_dep("//third_party/lz4:lz4.BUILD"),
        urls = ["https://github.com/lz4/lz4/archive/v1.9.2.tar.gz"],
        sha256 = "658ba6191fa44c92280d4aa2c271b0f4fbc0e34d249578dd05e50e76d0e5efcc",
        strip_prefix = "lz4-1.9.2",
    )

    # bzip2
    http_archive(
        name = "bzip2",
        build_file = clean_dep("//third_party/bzip2:bzip2.BUILD"),
        urls = ["https://versaweb.dl.sourceforge.net/project/bzip2/bzip2-1.0.6.tar.gz"],
        sha256 = "a2848f34fcd5d6cf47def00461fcb528a0484d8edef8208d6d2e2909dc61d9cd",
        strip_prefix = "bzip2-1.0.6",
    )

    # libdwarf
    http_archive(
        name = "dwarf",
        build_file = clean_dep("//third_party/dwarf:dwarf.BUILD"),
        urls = ["https://www.prevanders.net/libdwarf-20191104.tar.gz"],
        sha256 = "86119a9f7c409dc31e02d12c1b3906b1fce0dcb4db3d7e65ebe1bae585cf08f8",
        strip_prefix = "libdwarf-20191104",
    )

    # rocksdb
    http_archive(
        name = "rocksdb",
        build_file = clean_dep("//third_party/rocksdb:rocksdb.BUILD"),
        urls = ["https://github.com/facebook/rocksdb/archive/v6.8.1.tar.gz"],
        sha256 = "ca192a06ed3bcb9f09060add7e9d0daee1ae7a8705a3d5ecbe41867c5e2796a2",
        strip_prefix = "rocksdb-6.8.1",
    )

    # fmt
    http_archive(
        name = "fmt",
        build_file = clean_dep("//third_party/fmt:fmt.BUILD"),
        urls = ["https://github.com/fmtlib/fmt/archive/6.1.2.tar.gz"],
        sha256 = "1cafc80701b746085dddf41bd9193e6d35089e1c6ec1940e037fcb9c98f62365",
        strip_prefix = "fmt-6.1.2",
    )

    # double-conversion
    http_archive(
        name = "double-conversion",
        urls = ["https://github.com/google/double-conversion/archive/v3.1.5.tar.gz"],
        build_file = clean_dep("//third_party/double-conversion:double-conversion.BUILD"),
        sha256 = "a63ecb93182134ba4293fd5f22d6e08ca417caafa244afaa751cbfddf6415b13",
        strip_prefix = "double-conversion-3.1.5",
    )

    # libevent
    http_archive(
        name = "libevent",
        build_file = clean_dep("//third_party/libevent:libevent.BUILD"),
        urls = ["https://github.com/libevent/libevent/releases/download/release-2.1.11-stable/libevent-2.1.11-stable.tar.gz"],
        sha256 = "a65bac6202ea8c5609fd5c7e480e6d25de467ea1917c08290c521752f147283d",
        strip_prefix = "libevent-2.1.11-stable",
    )

    # liburing
    http_archive(
        name = "liburing",
        build_file = clean_dep("//third_party/liburing:liburing.BUILD"),
        urls = ["https://github.com/axboe/liburing/archive/liburing-0.6.tar.gz"],
        sha256 = "cf718a0a60c3a54da7ec82a0ca639a8e55d683f931b9aba9da603b849db185de",
        strip_prefix = "liburing-liburing-0.6",
        patch_args = [
            "-p0",
        ],
        patches = [
            clean_dep("//third_party/liburing:liburing.patch"),
        ],
    )

    # folly
    http_archive(
        name = "folly",
        build_file = clean_dep("//third_party/folly:folly.BUILD"),
        urls = ["https://github.com/facebook/folly/archive/v2020.04.13.00.tar.gz"],
        sha256 = "369d17a6603c1dc53db006bd5d613461b76db089bd90a85a713565c263497082",
        strip_prefix = "folly-2020.04.13.00",
    )

    http_archive(
        name = "rsocket-cpp",
        build_file = clean_dep("//third_party/rsocket-cpp:rsocket-cpp.BUILD"),
        urls = ["https://github.com/rsocket/rsocket-cpp/archive/e377f18abb03a885196385fada0329b50379c8ae.zip"],
        sha256 = "06e4aae7a6eeafdc1ec17f2c73941e799dedb482e870acc0576d05c74138dcbf",
        strip_prefix = "rsocket-cpp-e377f18abb03a885196385fada0329b50379c8ae",
    )

    http_archive(
        name = "flex",
        build_file = clean_dep("//third_party/flex:flex.BUILD"),
        urls = ["https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz"],
        sha256 = "e87aae032bf07c26f85ac0ed3250998c37621d95f8bd748b31f15b33c45ee995",
        strip_prefix = "flex-2.6.4",
    )

    http_archive(
        name = "bison",
        build_file = clean_dep("//third_party/bison:bison.BUILD"),
        urls = ["https://mirrors.aliyun.com/gnu/bison/bison-3.7.4.tar.gz"],
        sha256 = "fbabc7359ccd8b4b36d47bfe37ebbce44805c052526d5558b95eda125d1677e2",
        strip_prefix = "bison-3.7.4",
    )

    # fbthrift
    http_archive(
        name = "fbthrift",
        build_file = clean_dep("//third_party/fbthrift:fbthrift.BUILD"),
        sha256 = "0fc6cc1673209f4557e081597b2311f6c9f153840c4e55ac61a669e10207e2ee",
        strip_prefix = "fbthrift-2020.04.13.00",
        url = "https://github.com/facebook/fbthrift/archive/v2020.04.13.00.tar.gz",
    )

    # proxygen
    http_archive(
        name = "proxygen",
        build_file = clean_dep("//third_party/proxygen:proxygen.BUILD"),
        urls = ["https://github.com/facebook/proxygen/archive/v2020.04.13.00.tar.gz"],
        sha256 = "c8ac12aed526c3e67b9424a358dac150958e727feb2b3d1b8b3407ea0d53e315",
        strip_prefix = "proxygen-2020.04.13.00",
    )

    # wangle
    http_archive(
        name = "wangle",
        build_file = clean_dep("//third_party/wangle:wangle.BUILD"),
        urls = ["https://github.com/facebook/wangle/archive/v2020.04.13.00.tar.gz"],
        strip_prefix = "wangle-2020.04.13.00",
        sha256 = "a046dfea92f453bd12b28dad287a7eb86e782c4db9518b90c33c5320b3868f0b",
    )

    # fizz
    http_archive(
        name = "fizz",
        build_file = clean_dep("//third_party/fizz:fizz.BUILD"),
        strip_prefix = "fizz-2020.04.13.00",
        urls = ["https://github.com/facebookincubator/fizz/archive/v2020.04.13.00.tar.gz"],
        sha256 = "62d3f5ff24c32e373771ee33a7c4f394b56536d941ac476f774f62b6189d6ce5",
    )

    # libsodium
    http_archive(
        name = "libsodium",
        url = "https://github.com/jedisct1/libsodium/archive/1.0.16.tar.gz",
        build_file = clean_dep("//third_party/libsodium:libsodium.BUILD"),
        strip_prefix = "libsodium-1.0.16",
        sha256 = "0c14604bbeab2e82a803215d65c3b6e74bb28291aaee6236d65c699ccfe1a98c",
    )

    # argon2
    http_archive(
        name = "argon2",
        build_file = clean_dep("//third_party/argon2:argon2.BUILD"),
        sha256 = "eaea0172c1f4ee4550d1b6c9ce01aab8d1ab66b4207776aa67991eb5872fdcd8",
        strip_prefix = "phc-winner-argon2-20171227",
        url = "https://github.com/P-H-C/phc-winner-argon2/archive/20171227.tar.gz",
    )

    # glog
    http_archive(
        name = "glog",
        build_file = clean_dep("//third_party/glog:glog.BUILD"),
        urls = ["https://github.com/google/glog/archive/v0.4.0.tar.gz"],
        sha256 = "f28359aeba12f30d73d9e4711ef356dc842886968112162bc73002645139c39c",
        strip_prefix = "glog-0.4.0",
    )

    # gflags
    http_archive(
        name = "gflags",
        build_file = clean_dep("//third_party/gflags:gflags.BUILD"),
        urls = ["https://github.com/gflags/gflags/archive/v2.2.2.tar.gz"],
        sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
        strip_prefix = "gflags-2.2.2",
    )

    http_archive(
        name = "cityhash",
        build_file = clean_dep("//third_party/cityhash:cityhash.BUILD"),
        urls = ["https://github.com/google/cityhash/archive/8af9b8c2b889d80c22d6bc26ba0df1afb79a30db.zip"],
        sha256 = "3524f5ed43143974a29fddeeece29c8b6348f05db08dd180452da01a2837ddce",
        strip_prefix = "cityhash-8af9b8c2b889d80c22d6bc26ba0df1afb79a30db",
    )

    http_archive(
        name = "com_google_absl",
        urls = ["https://github.com/abseil/abseil-cpp/archive/98eb410c93ad059f9bba1bf43f5bb916fc92a5ea.zip"],
        strip_prefix = "abseil-cpp-98eb410c93ad059f9bba1bf43f5bb916fc92a5ea",
    )

    # GoogleTest/GoogleMock
    http_archive(
        name = "gtest",
        sha256 = "81964fe578e9bd7c94dfdb09c8e4d6e6759e19967e397dbea48d1c10e45d0df2",
        strip_prefix = "googletest-release-1.12.1",
        urls = ["https://github.com/google/googletest/archive/refs/tags/release-1.12.1.tar.gz"],
    )

    # Google benchmark
    http_archive(
        name = "com_github_google_benchmark",
        sha256 = "6430e4092653380d9dc4ccb45a1e2dc9259d581f4866dc0759713126056bc1d7",
        strip_prefix = "benchmark-1.7.1",
        urls = ["https://github.com/google/benchmark/archive/refs/tags/v1.7.1.tar.gz"],
    )

    http_archive(
        name = "msgpack",
        build_file = clean_dep("//third_party/msgpack:msgpack.BUILD"),
        urls = ["https://github.com/msgpack/msgpack-c/releases/download/cpp-3.3.0/msgpack-3.3.0.tar.gz"],
        strip_prefix = "msgpack-3.3.0",
        sha256 = "6e114d12a5ddb8cb11f669f83f32246e484a8addd0ce93f274996f1941c1f07b",
    )
