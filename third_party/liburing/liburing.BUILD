#======================================================================
#
# liburing.BUILD -
#
# Created by liubang on 2023/05/22 23:00
# Last Modified: 2023/05/22 23:00
#
#======================================================================
genrule(
    name = "compat_h",
    outs = [
        "src/include/liburing/compat.h",
    ],
    cmd = "./$(location configure) --compat=$@",
    tools = [
        "configure",
    ],
)

cc_library(
    name = "liburing",
    srcs = glob(["src/*.c"]) + [
        "src/syscall.h",
    ],
    hdrs = [
        "src/include/liburing.h",
        "src/include/liburing/barrier.h",
        "src/include/liburing/io_uring.h",
        ":compat_h",
    ],
    copts = ["-w"],
    includes = ["src/include"],
    visibility = ["//visibility:public"],
)
