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
    hdrs = glob(["*.h"]),
    linkopts = ["-pthread"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "boost_headers",
    srcs = ["boost.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@boost//:headers",
    ],
)
