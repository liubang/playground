_COMMON_ATTR = {
    "src": attr.label(
        mandatory = True,
        allow_single_file = [
            ".thrift",
        ],
    ),
    "services": attr.string_list(
        allow_empty = True,
    ),
    "options": attr.string_list(
    ),
    "include_prefix": attr.string(
    ),
    "_thrift1": attr.label(
        default = Label("@fbthrift//:thrift1"),
        executable = True,
        cfg = "host",
    ),
}

def _fbthrift_attrs(rule_attrs):
    rule_attrs.update(_COMMON_ATTR)
    return rule_attrs

def _get_file_path(file):
    idx = file.rfind("/")
    return file[0:idx]

def _get_file_name_no_ext(file):
    idx = file.rfind("/")
    return file[idx + 1:].split(".")[0]

def _get_output_dir(input_dir, build_file_dir):
    idx = input_dir.find(build_file_dir)
    return input_dir[idx + len(build_file_dir):]

def _fbthrift_common(ctx, lang):
    src = ctx.files.src[0]
    outputs = []

    # input_dir = _get_file_path(src.path)
    file_name = _get_file_name_no_ext(src.path)
    input_dir = _get_file_path(src.path) + "/"
    build_file_dir = _get_file_path(ctx.build_file_path) + "/"
    sub_dir = _get_output_dir(input_dir, build_file_dir)
    out_dir = "{}gen-{}".format(sub_dir, lang)

    headers = [
        ctx.actions.declare_file("{}/{}_constants.h".format(out_dir, file_name)),
        ctx.actions.declare_file("{}/{}_data.h".format(out_dir, file_name)),
        ctx.actions.declare_file("{}/{}_types.h".format(out_dir, file_name)),
        ctx.actions.declare_file("{}/{}_types.tcc".format(out_dir, file_name)),
    ]

    sources = [
        ctx.actions.declare_file("{}/{}_constants.cpp".format(out_dir, file_name)),
        ctx.actions.declare_file("{}/{}_data.cpp".format(out_dir, file_name)),
        ctx.actions.declare_file("{}/{}_types.cpp".format(out_dir, file_name)),
    ]

    for service in ctx.attr.services:
        headers += [
            ctx.actions.declare_file("{}/{}.h".format(out_dir, service)),
            ctx.actions.declare_file("{}/{}.tcc".format(out_dir, service)),
            ctx.actions.declare_file("{}/{}AsyncClient.h".format(out_dir, service)),
            ctx.actions.declare_file("{}/{}_custom_protocol.h".format(out_dir, service)),
        ]
        sources += [
            ctx.actions.declare_file("{}/{}.cpp".format(out_dir, service)),
            ctx.actions.declare_file("{}/{}AsyncClient.cpp".format(out_dir, service)),
        ]

    # Construct the arguments.
    args = ctx.actions.args()
    options = []
    has_metadata = True
    for opt in ctx.attr.options:
        options.append(opt)
        if opt == "no_metadata":
            has_metadata = False
    if has_metadata:
        sources += [
            ctx.actions.declare_file("{}/{}_metadata.cpp".format(out_dir, file_name)),
        ]
        headers += [
            ctx.actions.declare_file("{}/{}_metadata.h".format(out_dir, file_name)),
        ]

    if ctx.attr.include_prefix:
        options.append("include_prefix=%s" % ctx.attr.include_prefix)
    else:
        options.append("include_prefix=%s" % sub_dir)
    if lang == "cpp2":
        lang = "mstch_cpp2"
    args.add("--gen", "%s:%s" % (lang, ",".join(options)))
    args.add("-out", _get_file_path(headers[0].path))
    args.add(src.path)
    outputs = headers + sources
    ctx.actions.run(
        executable = ctx.executable._thrift1,
        arguments = [args],
        inputs = depset(direct = ctx.files.src),
        outputs = outputs,
        mnemonic = "Fbthrift",
        progress_message = "Fbthrift compile done.",
    )

    return struct(
        headers = headers,
        sources = sources,
        outputs = depset(direct = outputs),
    )

def _fbthrift_compile_impl(ctx):
    result = _fbthrift_common(ctx, ctx.attr.lang)
    return [
        DefaultInfo(files = result.outputs),
    ]

fbthrift_compile = rule(
    implementation = _fbthrift_compile_impl,
    attrs = _fbthrift_attrs({
        "lang": attr.string(
            default = "cpp2",
        ),
    }),
    provides = [
        DefaultInfo,
    ],
)

def _cc_library(ctx, fbthrift_result):
    cc_toolchain = ctx.attr._cc_toolchain[cc_common.CcToolchainInfo]

    cc_deps = cc_common.merge_cc_infos(cc_infos = [
        dep[CcInfo]
        for dep in ctx.attr.deps
    ])

    cc_feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.attr.features,
    )

    (cc_compilation_context, cc_compilation_outputs) = cc_common.compile(
        name = ctx.attr.name,
        actions = ctx.actions,
        cc_toolchain = cc_toolchain,
        feature_configuration = cc_feature_configuration,
        srcs = fbthrift_result.sources,
        public_hdrs = fbthrift_result.headers,
        includes = [ctx.genfiles_dir.path],
        user_compile_flags = ctx.attr.copts,
        compilation_contexts = [cc_deps.compilation_context],
    )

    (cc_linking_context, cc_linking_outputs) = cc_common.create_linking_context_from_compilation_outputs(
        name = ctx.attr.name,
        actions = ctx.actions,
        feature_configuration = cc_feature_configuration,
        cc_toolchain = cc_toolchain,
        compilation_outputs = cc_compilation_outputs,
        linking_contexts = [cc_deps.linking_context],
    )

    outs = []
    if cc_linking_outputs.library_to_link.static_library:
        outs.append(cc_linking_outputs.library_to_link.static_library)
    if cc_linking_outputs.library_to_link.dynamic_library:
        outs.append(cc_linking_outputs.library_to_link.dynamic_library)

    return struct(
        outs = depset(direct = outs),
        cc_info = CcInfo(
            compilation_context = cc_compilation_context,
            linking_context = cc_linking_context,
        ),
    )

def _fbthrift_cc_library_impl(ctx):
    result = _fbthrift_common(ctx, "cpp2")
    cc_lib = _cc_library(ctx, result)
    return [
        DefaultInfo(files = cc_lib.outs),
        cc_lib.cc_info,
    ]

fbthrift_cc_library = rule(
    implementation = _fbthrift_cc_library_impl,
    attrs = _fbthrift_attrs({
        "deps": attr.label_list(
            providers = [CcInfo],
        ),
        "copts": attr.string_list(
        ),
        "_cc_toolchain": attr.label(
            default = "@bazel_tools//tools/cpp:current_cc_toolchain",
        ),
    }),
    provides = [
        CcInfo,
        DefaultInfo,
    ],
    fragments = ["cpp"],
)
