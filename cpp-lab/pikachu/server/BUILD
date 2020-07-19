cc_binary(
    name = "pikachu",
    srcs = [
        "pikachu.cc",
        "pikachu_service.cc",
        "pikachu_service.h",
    ],
    copts = [
        "-std=c++14",
    ],
    deps = [
        "//pikachu/if:pikachu_thrift",
        "@folly",
        "@glog",
        "@gflags",
        "@proxygen//:proxygenhttpserver",
    ],
)
