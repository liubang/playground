#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 00:32
# Last Modified: 2023/05/21 00:32
#
#======================================================================

cc_library(
    name = "tools",
    srcs = glob(["*.cpp"]),
    hdrs = glob(["*.h"]),
    copts = ["-std=c++20"],
    linkopts = ["-pthread"],
    visibility = ["//visibility:public"],
    deps = [
        "@boost//:headers",
    ],
)
