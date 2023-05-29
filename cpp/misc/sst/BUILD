#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/26 16:34
# Last Modified: 2023/05/26 16:34
#
#======================================================================

cc_library(
    name = "sstable",
    srcs = [
        "block_builder.cpp",
        "table_builder.cpp",
    ],
    hdrs = [
        "block_builder.h",
        "encoding.h",
        "filter_block_builder.h",
        "filter_policy.h",
        "table_builder.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/misc/fs",
        "//cpp/tools",
    ],
)
