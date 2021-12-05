# vim: ft=bzl

load("@cpp//third_party/bison:yacc.bzl", "yacc")
load("@cpp//third_party/flex:flex.bzl", "flex")
load("@cpp//third_party/fbthrift:fbthrift.bzl", "fbthrift_compile")

_common_copts = [
    "-std=gnu++1z",
    "-Wunused-function",
    "-Wdeprecated-declarations",
]

cc_library(
    name = "boost_libs",
    deps = [
        "@boost//:context",
        "@boost//:filesystem",
        "@boost//:headers",
        "@boost//:program_options",
        "@boost//:regex",
        "@boost//:system",
        "@boost//:thread",
    ],
)

yacc(
    name = "thrifty_bison",
    src = "thrift/compiler/parse/thrifty.yy",
    extra_options = [
        "--skeleton=lalr1.cc",
        "-Wno-yacc",
    ],
    extra_outs = [
        "thrift/compiler/stack.hh",
    ],
    header_out = "thrift/compiler/thrifty.hh",
    source_out = "thrift/compiler/thrifty.cc",
)

flex(
    name = "thriftl_flex",
    src = "thrift/compiler/parse/thriftl.ll",
    out = "thrift/compiler/thriftl.cc",
)

cc_library(
    name = "compiler_ast",
    srcs = [
        "thrift/compiler/ast/base_types.cc",
        "thrift/compiler/ast/t_program.cc",
        "thrift/compiler/ast/t_scope.cc",
        "thrift/compiler/ast/t_type.cc",
        "thrift/compiler/ast/t_typedef.cc",
    ],
    hdrs = glob(["thrift/compiler/ast/*.h"]),
    copts = _common_copts,
    includes = ["."],
    deps = [
        "@openssl",
    ],
)

cc_library(
    name = "compiler_base",
    srcs = [
        "thrift/compiler/common.cc",
        "thrift/compiler/parse/parsing_driver.cc",
        "thrift/compiler/platform.cc",
        "thrift/compiler/stack.hh",
        "thrift/compiler/thriftl.cc",
        "thrift/compiler/thrifty.cc",
        "thrift/compiler/thrifty.hh",
        "thrift/compiler/util.cc",
    ],
    hdrs = glob([
        "thrift/compiler/*.h",
        "thrift/compiler/parse/*.h",
    ]),
    copts = _common_copts + [
        "-Wno-deprecated-register",
        "-Wno-register",
    ],
    includes = [
        ".",
        "thrift/compiler",
    ],
    local_defines = [
        "THRIFTY_HH=\\\"thrift/compiler/thrifty.hh\\\"",
    ],
    deps = [
        ":boost_libs",
        ":compiler_ast",
    ],
)

cc_library(
    name = "compiler_lib",
    srcs = [
        "thrift/compiler/lib/cpp2/util.cc",
        "thrift/compiler/lib/java/util.cc",
    ],
    hdrs = [
        "thrift/compiler/lib/cpp2/util.h",
        "thrift/compiler/lib/java/util.h",
        "thrift/compiler/util.h",
    ],
    copts = _common_copts,
    deps = [
        ":boost_libs",
        ":compiler_ast",
    ],
)

cc_library(
    name = "mustache_lib",
    srcs = [
        "thrift/compiler/mustache/mstch.cpp",
        "thrift/compiler/mustache/render_context.cpp",
        "thrift/compiler/mustache/state/in_section.cpp",
        "thrift/compiler/mustache/state/outside_section.cpp",
        "thrift/compiler/mustache/template_type.cpp",
        "thrift/compiler/mustache/token.cpp",
        "thrift/compiler/mustache/utils.cpp",
    ],
    hdrs = glob([
        "thrift/compiler/mustache/**/*.h",
    ]),
    copts = _common_copts,
    deps = [
        ":boost_libs",
    ],
)

cc_binary(
    name = "compiler_generate_build_templates",
    srcs = [
        "thrift/compiler/generate/build_templates.cc",
    ],
    copts = _common_copts,
    local_defines = [
        "DBOOST_ERROR_CODE_HEADER_ONLY",
    ],
    deps = [
        ":boost_libs",
    ],
)

genrule(
    name = "templates",
    srcs = glob([
        "thrift/compiler/generate/templates/**/*.mustache",
    ]),
    outs = [
        "thrift/compiler/generate/templates.cc",
    ],
    cmd = " ".join([
        "$(location :compiler_generate_build_templates)",
        "external/fbthrift/thrift/compiler/generate/templates > $@",
    ]),
    tools = [
        ":compiler_generate_build_templates",
    ],
)

cc_library(
    name = "compiler_generate_templates",
    srcs = [
        ":templates",
    ],
    hdrs = [
        "thrift/compiler/generate/templates.h",
    ],
    copts = _common_copts,
    includes = ["."],
)

cc_library(
    name = "compiler_generators",
    srcs = [
        "thrift/compiler/generate/common.cc",
        "thrift/compiler/generate/t_android_generator.cc",
        "thrift/compiler/generate/t_android_lite_generator.cc",
        "thrift/compiler/generate/t_cocoa_generator.cc",
        "thrift/compiler/generate/t_concat_generator.cc",
        "thrift/compiler/generate/t_csharp_generator.cc",
        "thrift/compiler/generate/t_d_generator.cc",
        "thrift/compiler/generate/t_erl_generator.cc",
        "thrift/compiler/generate/t_generator.cc",
        "thrift/compiler/generate/t_go_generator.cc",
        "thrift/compiler/generate/t_hack_generator.cc",
        "thrift/compiler/generate/t_hs_generator.cc",
        "thrift/compiler/generate/t_html_generator.cc",
        "thrift/compiler/generate/t_java_generator.cc",
        "thrift/compiler/generate/t_js_generator.cc",
        "thrift/compiler/generate/t_json_experimental_generator.cc",
        "thrift/compiler/generate/t_json_generator.cc",
        "thrift/compiler/generate/t_mstch_cpp2_generator.cc",
        "thrift/compiler/generate/t_mstch_generator.cc",
        "thrift/compiler/generate/t_mstch_html_generator.cc",
        "thrift/compiler/generate/t_mstch_objects.cc",
        "thrift/compiler/generate/t_mstch_py3_generator.cc",
        "thrift/compiler/generate/t_mstch_pyi_generator.cc",
        "thrift/compiler/generate/t_mstch_rust_generator.cc",
        "thrift/compiler/generate/t_mstch_swift_generator.cc",
        "thrift/compiler/generate/t_ocaml_generator.cc",
        "thrift/compiler/generate/t_perl_generator.cc",
        "thrift/compiler/generate/t_php_generator.cc",
        "thrift/compiler/generate/t_py_generator.cc",
        "thrift/compiler/generate/t_rb_generator.cc",
        "thrift/compiler/generate/t_st_generator.cc",
    ],
    hdrs = glob([
        "thrift/compiler/generate/*.h",
        "thrift/compiler/lib/**/*.h",
    ]),
    copts = _common_copts,
    deps = [
        ":compiler_base",
        ":compiler_generate_templates",
        ":compiler_lib",
        ":mustache_lib",
        "@openssl",
    ],
    alwayslink = True,
)

cc_binary(
    name = "thrift1",
    srcs = [
        "thrift/compiler/ast/visitor.cc",
        "thrift/compiler/compiler.cc",
        "thrift/compiler/main.cc",
        "thrift/compiler/mutator/mutator.cc",
        "thrift/compiler/mutator/mutator.h",
        "thrift/compiler/validator/validator.cc",
        "thrift/compiler/validator/validator.h",
    ],
    copts = _common_copts,
    visibility = ["//visibility:public"],
    deps = [
        ":boost_libs",
        ":compiler_ast",
        ":compiler_base",
        ":compiler_generators",
    ],
)

######### cpp library #########

fbthrift_compile(
    name = "reflection",
    src = "thrift/lib/thrift/reflection.thrift",
    include_prefix = "thrift/lib/thrift",
    options = [
        "templates",
        "no_metadata",
    ],
    output_path = "thrift/lib/thrift/",
    services = [],
)

fbthrift_compile(
    name = "metadata",
    src = "thrift/lib/thrift/metadata.thrift",
    include_prefix = "thrift/lib/thrift",
    output_path = "thrift/lib/thrift",
)

fbthrift_compile(
    name = "frozen",
    src = "thrift/lib/thrift/frozen.thrift",
    include_prefix = "thrift/lib/thrift",
    output_path = "thrift/lib/thrift",
)

fbthrift_compile(
    name = "RpcMetadata",
    src = "thrift/lib/thrift/RpcMetadata.thrift",
    include_prefix = "thrift/lib/thrift",
    output_path = "thrift/lib/thrift",
)

filegroup(
    name = "cpp_hdrs",
    srcs = glob([
        "thrift/lib/thrift/*.h",
        "thrift/lib/cpp/**/*.h",
        "thrift/lib/cpp/**/*.tcc",
        "thrift/lib/cpp2/**/*.h",
        "thrift/lib/cpp2/**/*.tcc",
    ]),
)

cc_library(
    name = "thrift-core",
    srcs = [
        "thrift/lib/cpp/Thrift.cpp",
        "thrift/lib/cpp2/FieldRef.cpp",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    deps = [
        "@fmt",
        "@folly",
    ],
)

cc_library(
    name = "concurrency",
    srcs = [
        "thrift/lib/cpp/concurrency/Monitor.cpp",
        "thrift/lib/cpp/concurrency/Mutex.cpp",
        "thrift/lib/cpp/concurrency/PosixThreadFactory.cpp",
        "thrift/lib/cpp/concurrency/ThreadManager.cpp",
        "thrift/lib/cpp/concurrency/TimerManager.cpp",
        "thrift/lib/cpp/concurrency/Util.cpp",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    deps = [
        "@folly",
        "@gflags",
        "@glog",
    ],
)

cc_library(
    name = "protocol",
    srcs = [
        "thrift/lib/cpp/protocol/TBase64Utils.cpp",
        "thrift/lib/cpp/protocol/TDebugProtocol.cpp",
        "thrift/lib/cpp/protocol/TJSONProtocol.cpp",
        "thrift/lib/cpp/protocol/TProtocolException.cpp",
        "thrift/lib/cpp/protocol/TSimpleJSONProtocol.cpp",
        ":reflection",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    deps = [
        ":thrift-core",
        "@fmt",
        "@folly",
        "@glog",
    ],
)

cc_library(
    name = "transport",
    srcs = [
        "thrift/lib/cpp/transport/TBufferTransports.cpp",
        "thrift/lib/cpp/transport/TFDTransport.cpp",
        "thrift/lib/cpp/transport/THeader.cpp",
        "thrift/lib/cpp/transport/THttpClient.cpp",
        "thrift/lib/cpp/transport/THttpServer.cpp",
        "thrift/lib/cpp/transport/THttpTransport.cpp",
        "thrift/lib/cpp/transport/TSocket.cpp",
        "thrift/lib/cpp/transport/TTransportException.cpp",
        "thrift/lib/cpp/transport/TZlibTransport.cpp",
        "thrift/lib/cpp/util/PausableTimer.cpp",
        "thrift/lib/cpp/util/THttpParser.cpp",
        "thrift/lib/cpp/util/VarintUtils.cpp",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    deps = [
        ":concurrency",
        ":thrift-core",
        "@folly",
        "@openssl",
        "@zlib",
        "@zstd",
    ],
)

cc_library(
    name = "async",
    srcs = [
        "thrift/lib/cpp/ContextStack.cpp",
        "thrift/lib/cpp/EventHandlerBase.cpp",
        "thrift/lib/cpp/async/TBinaryAsyncChannel.cpp",
        "thrift/lib/cpp/async/TFramedAsyncChannel.cpp",
        "thrift/lib/cpp/async/THttpAsyncChannel.cpp",
        "thrift/lib/cpp/async/TUnframedAsyncChannel.cpp",
        "thrift/lib/cpp/async/TZlibAsyncChannel.cpp",
        "thrift/lib/cpp/server/TServerObserver.cpp",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    deps = [
        ":boost_libs",
        ":concurrency",
        ":transport",
        "@folly",
        "@glog",
        "@openssl",
    ],
)

cc_library(
    name = "thrift",
    deps = [
        ":async",
        ":concurrency",
        ":protocol",
        ":transport",
        "@folly",
        "@glog",
    ],
)

cc_library(
    name = "thriftmetadata",
    srcs = [
        ":frozen",
        ":metadata",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    deps = [
        "@folly",
    ],
)

cc_library(
    name = "thriftfrozen2",
    srcs = [
        "thrift/lib/cpp2/frozen/Frozen.cpp",
        "thrift/lib/cpp2/frozen/FrozenUtil.cpp",
        "thrift/lib/cpp2/frozen/schema/MemorySchema.cpp",
        ":frozen",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    deps = [
        ":thriftmetadata",
        "@folly",
        "@gflags",
        "@glog",
    ],
)

cc_library(
    name = "thriftprotocol",
    srcs = [
        "thrift/lib/cpp2/protocol/BinaryProtocol.cpp",
        "thrift/lib/cpp2/protocol/CompactProtocol.cpp",
        "thrift/lib/cpp2/protocol/CompactV1Protocol.cpp",
        "thrift/lib/cpp2/protocol/DebugProtocol.cpp",
        "thrift/lib/cpp2/protocol/JSONProtocol.cpp",
        "thrift/lib/cpp2/protocol/JSONProtocolCommon.cpp",
        "thrift/lib/cpp2/protocol/Serializer.cpp",
        "thrift/lib/cpp2/protocol/VirtualProtocol.cpp",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    deps = [
        ":thrift",
        "@folly",
        "@wangle",
    ],
)

fbthrift_compile(
    name = "rsocket-cpp2",
    src = "thrift/lib/cpp2/transport/rsocket/Config.thrift",
    include_prefix = "thrift/lib/cpp2/transport/rsocket",
    output_path = "thrift/lib/cpp2/transport/rsocket",
)

cc_library(
    name = "thriftcpp2",
    srcs = [
        "thrift/lib/cpp2/FrozenTApplicationException.cpp",
        "thrift/lib/cpp2/GeneratedCodeHelper.cpp",
        "thrift/lib/cpp2/async/AsyncClient.cpp",
        "thrift/lib/cpp2/async/AsyncProcessor.cpp",
        "thrift/lib/cpp2/async/Cpp2Channel.cpp",
        "thrift/lib/cpp2/async/DuplexChannel.cpp",
        "thrift/lib/cpp2/async/FramingHandler.cpp",
        "thrift/lib/cpp2/async/HeaderChannel.cpp",
        "thrift/lib/cpp2/async/HeaderChannelTrait.cpp",
        "thrift/lib/cpp2/async/HeaderClientChannel.cpp",
        "thrift/lib/cpp2/async/HeaderServerChannel.cpp",
        "thrift/lib/cpp2/async/RequestChannel.cpp",
        "thrift/lib/cpp2/async/ResponseChannel.cpp",
        "thrift/lib/cpp2/async/RocketClientChannel.cpp",
        "thrift/lib/cpp2/async/RpcTypes.cpp",
        "thrift/lib/cpp2/security/extensions/ThriftParametersClientExtension.cpp",
        "thrift/lib/cpp2/security/extensions/ThriftParametersContext.cpp",
        "thrift/lib/cpp2/security/extensions/Types.cpp",
        "thrift/lib/cpp2/server/BaseThriftServer.cpp",
        "thrift/lib/cpp2/server/Cpp2ConnContext.cpp",
        "thrift/lib/cpp2/server/Cpp2Connection.cpp",
        "thrift/lib/cpp2/server/Cpp2Worker.cpp",
        "thrift/lib/cpp2/server/RequestsRegistry.cpp",
        "thrift/lib/cpp2/server/ServerInstrumentation.cpp",
        "thrift/lib/cpp2/server/ThriftServer.cpp",
        "thrift/lib/cpp2/server/peeking/TLSHelper.cpp",
        "thrift/lib/cpp2/transport/core/RpcMetadataUtil.cpp",
        "thrift/lib/cpp2/transport/core/ThriftClient.cpp",
        "thrift/lib/cpp2/transport/core/ThriftClientCallback.cpp",
        "thrift/lib/cpp2/transport/core/ThriftProcessor.cpp",
        "thrift/lib/cpp2/transport/rocket/PayloadUtils.cpp",
        "thrift/lib/cpp2/transport/rocket/Types.cpp",
        "thrift/lib/cpp2/transport/rocket/client/RequestContext.cpp",
        "thrift/lib/cpp2/transport/rocket/client/RequestContextQueue.cpp",
        "thrift/lib/cpp2/transport/rocket/client/RocketClient.cpp",
        "thrift/lib/cpp2/transport/rocket/client/RocketStreamServerCallback.cpp",
        "thrift/lib/cpp2/transport/rocket/framing/ErrorCode.cpp",
        "thrift/lib/cpp2/transport/rocket/framing/Frames.cpp",
        "thrift/lib/cpp2/transport/rocket/framing/Serializer.cpp",
        "thrift/lib/cpp2/transport/rocket/framing/Util.cpp",
        "thrift/lib/cpp2/transport/rocket/server/RocketServerConnection.cpp",
        "thrift/lib/cpp2/transport/rocket/server/RocketServerFrameContext.cpp",
        "thrift/lib/cpp2/transport/rocket/server/RocketSinkClientCallback.cpp",
        "thrift/lib/cpp2/transport/rocket/server/RocketStreamClientCallback.cpp",
        "thrift/lib/cpp2/transport/rocket/server/RocketThriftRequests.cpp",
        "thrift/lib/cpp2/transport/rocket/server/ThriftRocketServerHandler.cpp",
        "thrift/lib/cpp2/transport/rsocket/server/RSRoutingHandler.cpp",
        "thrift/lib/cpp2/util/Checksum.cpp",
        "thrift/lib/cpp2/util/ScopedServerInterfaceThread.cpp",
        "thrift/lib/cpp2/util/ScopedServerThread.cpp",
        ":RpcMetadata",
        ":rsocket-cpp2",
    ],
    hdrs = [
        ":cpp_hdrs",
    ],
    copts = _common_copts,
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":thrift",
        ":thriftfrozen2",
        ":thriftmetadata",
        ":thriftprotocol",
    ],
)

cc_library(
    name = "http2",
    srcs = [
        "thrift/lib/cpp2/transport/http2/client/H2ClientConnection.cpp",
        "thrift/lib/cpp2/transport/http2/client/ThriftTransactionHandler.cpp",
        "thrift/lib/cpp2/transport/http2/common/H2Channel.cpp",
        "thrift/lib/cpp2/transport/http2/common/HTTP2RoutingHandler.cpp",
        "thrift/lib/cpp2/transport/http2/common/SingleRpcChannel.cpp",
        "thrift/lib/cpp2/transport/http2/server/ThriftRequestHandler.cpp",
    ],
    hdrs = glob(["thrift/lib/cpp2/transport/http2/**/*.h"]),
    copts = _common_copts,
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":thriftcpp2",
        "@proxygen//:proxygenhttpserver",
        "@rsocket-cpp//:ReactiveSocket",
    ],
)

cc_library(
    name = "thriftperfutil",
    srcs = glob([
        "thrift/perf/cpp2/util/*.cpp",
    ]),
    hdrs = glob([
        "thrift/perf/cpp2/util/*.h",
    ]),
    copts = _common_copts,
    visibility = ["//visibility:public"],
    deps = [
        ":thriftcpp2",
        "@proxygen//:proxygenhttpserver",
    ],
)
