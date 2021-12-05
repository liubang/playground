# vim: ft=bzl
exports_files(["COPYING.txt"])

namespace = [
    "google",
    "gflags",
]

genrule(
    name = "gflags_declare_h",
    srcs = [
        "src/gflags_declare.h.in",
    ],
    outs = [
        "gflags_declare.h",
    ],
    cmd = ("awk '{ " +
           "gsub(/@GFLAGS_NAMESPACE@/, \"" + namespace[0] + "\"); " +
           "gsub(/@(HAVE_STDINT_H|HAVE_SYS_TYPES_H|HAVE_INTTYPES_H|GFLAGS_INTTYPES_FORMAT_C99)@/, \"1\"); " +
           "gsub(/@([A-Z0-9_]+)@/, \"0\"); " +
           "print; }' $(<) > $(@)"),
)

genrule(
    name = "gflags_gflags_h",
    srcs = [
        "src/gflags_ns.h.in",
    ],
    outs = [
        "gflags_gflags.h",
    ],
    cmd = ("awk '{ " +
           "gsub(/@ns@/, \"gflags\"); " +
           "gsub(/@NS@/, \"GFLAGS\"); " +
           "print; }' $(<) > $(@)"),
)

gflags_ns_h_files = [
    "gflags_gflags.h",
]

genrule(
    name = "gflags_h",
    srcs = [
        "src/gflags.h.in",
    ],
    outs = [
        "gflags.h",
    ],
    cmd = ("awk '{ " +
           "gsub(/@GFLAGS_ATTRIBUTE_UNUSED@/, \"\"); " +
           "gsub(/@INCLUDE_GFLAGS_NS_H@/, \"" + "\n".join(["#include \\\"gflags/{}\\\"".format(hdr) for hdr in gflags_ns_h_files]) + "\"); " +
           "print; }' $(<) > $(@)"),
)

genrule(
    name = "gflags_completions_h",
    srcs = [
        "src/gflags_completions.h.in",
    ],
    outs = [
        "gflags_completions.h",
    ],
    cmd = "awk '{ gsub(/@GFLAGS_NAMESPACE@/, \"" + namespace[0] + "\"); print; }' $(<) > $(@)",
)

cc_library(
    name = "gflags",
    srcs = [
        "src/config.h",
        "src/gflags.cc",
        "src/gflags_completions.cc",
        "src/gflags_reporting.cc",
        "src/mutex.h",
        "src/util.h",
    ],
    hdrs = [
        ":gflags_completions_h",
        ":gflags_declare_h",
        ":gflags_gflags_h",
        ":gflags_h",
    ],
    copts = [
        "-std=gnu++14",
        "-DGFLAGS_BAZEL_BUILD",
        "-DGFLAGS_INTTYPES_FORMAT_C99",
        "-DGFLAGS_IS_A_DLL=0",
        # macros otherwise defined by CMake configured defines.h file
        "-DHAVE_STDINT_H",
        "-DHAVE_SYS_TYPES_H",
        "-DHAVE_INTTYPES_H",
        "-DHAVE_SYS_STAT_H",
        "-DHAVE_STRTOLL",
        "-DHAVE_STRTOQ",
        "-DHAVE_RWLOCK",
        "-DHAVE_UNISTD_H",
        "-DHAVE_FNMATCH_H",
        "-DHAVE_PTHREAD",
    ],
    include_prefix = "gflags",
    linkopts = [
        "-pthread",
    ],
    visibility = ["//visibility:public"],
)
