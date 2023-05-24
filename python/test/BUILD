#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/25 00:44
# Last Modified: 2023/05/25 00:44
#
#======================================================================

load("@rules_python//python:defs.bzl", "py_binary")
load("@rules_python//python:proto.bzl", "py_proto_library")

py_proto_library(
    name = "py_sensor_proto",
    deps = [
        "//proto/test:sensor",
    ],
)

py_binary(
    name = "sensor_pb",
    srcs = ["sensor_pb.py"],
    deps = [":py_sensor_proto"],
)
