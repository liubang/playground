def _genlex_impl(ctx):
    """Implementation for genlex rule."""

    # Compute the prefix, if not specified.
    if ctx.attr.prefix:
        prefix = ctx.attr.prefix
    else:
        prefix = ctx.file.src.basename.partition(".")[0]

    # Construct the arguments.
    args = ctx.actions.args()
    args.add("-o", ctx.outputs.out)
    args.add("-P", prefix)
    args.add_all(ctx.attr.lexopts)
    args.add(ctx.file.src)
    outputs = [ctx.outputs.out]
    ctx.actions.run(
        executable = ctx.executable._flex,
        arguments = [args],
        inputs = ctx.files.src + ctx.files.includes,
        outputs = outputs,
        mnemonic = "Flex",
        progress_message = "Generating %s from %s" % (
            ctx.outputs.out.short_path,
            ctx.file.src.short_path,
        ),
    )
    return [
        DefaultInfo(files = depset(direct = outputs)),
    ]

flex = rule(
    _genlex_impl,
    attrs = {
        "src": attr.label(
            mandatory = True,
            allow_single_file = [
                ".l",
                ".ll",
                ".lex",
                ".lpp",
            ],
            doc = "The .lex source file for this rule",
        ),
        "includes": attr.label_list(
            allow_files = True,
            doc = "A list of headers that are included by the .lex file",
        ),
        "out": attr.output(
            mandatory = True,
            doc = "The generated source file",
        ),
        "prefix": attr.string(
            doc = "External symbol prefix for Flex. This string is " +
                  "passed to flex as the -P option, causing the resulting C " +
                  "file to define external functions named 'prefix'text, " +
                  "'prefix'in, etc.  The default is the basename of the source" +
                  "file without the .lex extension.",
        ),
        "lexopts": attr.string_list(
            doc = "A list of options to be added to the flex command line.",
        ),
        "_flex": attr.label(
            default = Label("@flex//:flex_bin"),
            executable = True,
            cfg = "host",
        ),
    },
    provides = [
        DefaultInfo,
    ],
)
