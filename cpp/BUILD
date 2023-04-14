#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/04/14 13:40
# Last Modified: 2023/04/14 13:40
#
#======================================================================
package(
    default_visibility = ["//visibility:public"],
    features = ["fully_static_link"],
)

load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = {
        "//highkyck/misk/...": "",
        "//highkyck/utils/...": "",
        "//highkyck/meta/...": "",
        "//highkyck/trace/...": "",
        "//highkyck/brain_fuck/...": "",
        "//test/cpp14/...": "",
        "//test/cpp17/...": "",
        "//test/cpp20/...": "",
        "//test/meta/...": "",
        "//test/misk/...": "",
    },
)
