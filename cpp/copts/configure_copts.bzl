#======================================================================
#
# configure_copts.bzl -
#
# Created by liubang on 2023/06/11 00:18
# Last Modified: 2023/06/11 00:18
#
#======================================================================

load(
    "//cpp:copts/copts.bzl",
    "GCC_FLAGS",
    "GCC_TEST_FLAGS",
    "LLVM_FLAGS",
    "LLVM_TEST_FLAGS",
)

DEFAULT_COPTS = select({
    "//cpp:clang_compiler": LLVM_FLAGS,
    "//cpp:gcc_compiler": GCC_FLAGS,
    "//conditions:default": GCC_FLAGS,
})

TEST_COPTS = select({
    "//cpp:clang_compiler": LLVM_TEST_FLAGS,
    "//cpp:gcc_compiler": GCC_TEST_FLAGS,
    "//conditions:default": GCC_TEST_FLAGS,
})

DEFAULT_LINKOPTS = select({
    "//conditions:default": [
        "-fsanitize=address",
    ],
})
