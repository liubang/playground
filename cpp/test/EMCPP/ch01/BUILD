[
    cc_binary(
        name = "%s" % f[0:f.rfind(".")],
        srcs = [
            "%s" % f,
        ] + glob([
            "includes/*.h",
        ]),
        copts = [
            "-std=c++17",
            "-w",
        ],
        includes = [
            "includes",
        ],
        linkopts = [
            "-pthread",
        ],
    )
    for f in glob([
        "*.cpp",
    ])
]
