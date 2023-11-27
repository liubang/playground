#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/11/01 20:28
# Last Modified: 2023/11/01 20:28
#
#======================================================================

load("@rules_ragel//ragel:ragel.bzl", "ragel")
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

# flux v0.193.0
ragel(
    name = "scanner_generated",
    src = "scanner.rl",
    data = [
        "unicode.rl",
    ],
    language = "c++",
    ragel_options = ["-I ."],
)

cc_library(
    name = "scanner",
    srcs = [
        "scanner.cpp",
        ":scanner_generated",
    ],
    hdrs = [
        "scanner.h",
        "token.h",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)

cc_binary(
    name = "scanner_test",
    srcs = [
        "scanner_test.cpp",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":scanner",
    ],
)

cc_library(
    name = "parser",
    srcs = [
        "ast.cpp",
        "parser.cpp",
        "strconv.cpp",
    ],
    hdrs = [
        "ast.h",
        "parser.h",
        "strconv.h",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    local_defines = ["FLUX_ENABLE_DEBUG"],
    deps = [
        ":scanner",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
    ],
)

cc_binary(
    name = "parser_test",
    srcs = [
        "parser_test.cpp",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":parser",
    ],
)
