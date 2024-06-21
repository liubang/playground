#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/22 17:18
# Last Modified: 2023/08/22 17:18
#
#======================================================================
# genrule(
#     name = "sql_lemon",
#     srcs = [
#         "sql.y",
#         "ttokens.h",
#         "@sqlite//:lempar",
#     ],
#     outs = ["sql.c"],
#     cmd = "$(location @sqlite//:lemon) -T$(location @sqlite//:lempar) -d$(RULEDIR) -s $(location sql.y)",
#     tools = [
#         "@sqlite//:lemon",
#     ],
# )
#
# cc_binary(
#     name = "sql",
#     srcs = [
#         "main.c",
#         ":sql_lemon",
#     ],
#     includes = ["."],
# )
