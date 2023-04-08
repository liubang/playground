# vim: ft=bzl
load("@rules_foreign_cc//foreign_cc:defs.bzl", "boost_build")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

boost_build(
    name = "headers",
    lib_source = ":all",
    out_headers_only = True,
    user_options = ["--with-headers"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "regex",
    lib_source = ":all",
    out_static_libs = ["libboost_regex.a"],
    user_options = ["--with-regex"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "filesystem",
    lib_source = ":all",
    out_static_libs = ["libboost_filesystem.a"],
    user_options = ["--with-filesystem"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "context",
    lib_source = ":all",
    out_static_libs = ["libboost_context.a"],
    user_options = ["--with-context"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "chrono",
    lib_source = ":all",
    out_static_libs = ["libboost_chrono.a"],
    user_options = ["--with-chrono"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "date_time",
    lib_source = ":all",
    out_static_libs = ["libboost_date_time.a"],
    user_options = ["--with-date_time"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "program_options",
    lib_source = ":all",
    out_static_libs = ["libboost_program_options.a"],
    user_options = ["--with-program_options"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "system",
    lib_source = ":all",
    out_static_libs = ["libboost_system.a"],
    user_options = ["--with-system"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "thread",
    lib_source = ":all",
    out_static_libs = ["libboost_thread.a"],
    user_options = ["--with-thread"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "random",
    lib_source = ":all",
    out_static_libs = ["libboost_random.a"],
    user_options = ["--with-random"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "atomic",
    lib_source = ":all",
    out_static_libs = ["libboost_atomic.a"],
    user_options = ["--with-atomic"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "fiber",
    lib_source = ":all",
    out_static_libs = ["libboost_fiber.a"],
    user_options = ["--with-fiber"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "timer",
    lib_source = ":all",
    out_static_libs = ["libboost_timer.a"],
    user_options = ["--with-timer"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "iostreams",
    lib_source = ":all",
    out_static_libs = ["libboost_iostreams.a"],
    user_options = ["--with-iostreams"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "contract",
    lib_source = ":all",
    out_static_libs = ["libboost_contract.a"],
    user_options = ["--with-contract"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "coroutine",
    lib_source = ":all",
    out_static_libs = ["libboost_coroutine.a"],
    user_options = ["--with-coroutine"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "exception",
    lib_source = ":all",
    out_static_libs = ["libboost_exception.a"],
    user_options = ["--with-exception"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "graph",
    lib_source = ":all",
    out_static_libs = ["libboost_graph.a"],
    user_options = ["--with-graph"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "locale",
    lib_source = ":all",
    out_static_libs = ["libboost_locale.a"],
    user_options = ["--with-locale"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "log",
    lib_source = ":all",
    out_static_libs = ["libboost_log.a"],
    user_options = ["--with-log"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "nowide",
    lib_source = ":all",
    out_static_libs = ["libboost_nowide.a"],
    user_options = ["--with-nowide"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "serialization",
    lib_source = ":all",
    out_static_libs = ["libboost_serialization.a"],
    user_options = ["--with-serialization"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "type_erasure",
    lib_source = ":all",
    out_static_libs = ["libboost_type_erasure.a"],
    user_options = ["--with-type_erasure"],
    visibility = ["//visibility:public"],
)

boost_build(
    name = "wave",
    lib_source = ":all",
    out_static_libs = ["libboost_wave.a"],
    user_options = ["--with-wave"],
    visibility = ["//visibility:public"],
)
