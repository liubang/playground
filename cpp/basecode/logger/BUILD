package(default_visibility = ["//visibility:public"])

cc_library(
    name = "logger",
    srcs = [
        "buffer.cc",
        "file_writer.cc",
        "log_appender.cc",
        "logger.cc",
        "singleton.h",
        "count_down_latch.cc",
    ],
    hdrs = [
        "buffer.h",
        "count_down_latch.h",
        "file_writer.h",
        "log_appender.h",
        "logger.h",
    ],
    copts = [
        "-std=c++17",
    ],
)
