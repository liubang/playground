#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 22:38
# Last Modified: 2023/05/21 22:38
#
#======================================================================
cc_library(
    name = "bloom_filter",
    srcs = glob(["*.cpp"]),
    hdrs = glob(["*.h"]),
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/misc/hash",
    ],
)
