# Echo Service — gRPC & brpc 多语言实现

共享 Protobuf 定义：[`echo.proto`](echo.proto)

| 协议 | 传输层 | RPC 数量 | 实现语言 |
|------|--------|----------|----------|
| **gRPC** | HTTP/2 | 5 个（含 3 个流式） | C++ / Go / Java / Python |
| **brpc** | TCP 二进制 | 2 个（仅 unary） | C++ (brpc) / Java (Starlight) |

---

## 一键启动

在项目根目录执行，每个 server 阻塞终端，需要分别在独立 shell 中运行。

### gRPC Server（默认 `:50051`）

> 所有 gRPC server 实现了全部 5 个 RPC。同一时间只能有一个绑定 50051 端口。

```bash
bazel run //cpp/pl/grpc/echo:echo_server     -- --port=50051 --server_id=cpp-grpc
bazel run //go/pl/grpc/echo/server:echo_server  -- 50051 go-server
bazel run //java/pl/grpc/echo:echo_server    -- 50051 java-server
bazel run //python/pl/grpc/echo:echo_server  -- 50051 python-server
```

### brpc / Starlight Server

> 仅实现 `Echo` 和 `HealthCheck` 两个 unary RPC。端口独立，可与 gRPC server 并行运行。

```bash
# C++ brpc (TCP :8000)
bazel run //cpp/pl/brpc/echo:echo_server -- --port=8000

# Java Starlight (TCP :8005)
bazel run //java/pl/brpc/echo:echo_server -- localhost 8005
```

---

## 客户端测试

### gRPC Client → gRPC Server

所有 gRPC client 覆盖 5 个 RPC，默认连接 `localhost:50051`。

```bash
bazel run //cpp/pl/grpc/echo:echo_client     -- --target=localhost:50051
bazel run //go/pl/grpc/echo/client:echo_client  -- localhost:50051
bazel run //java/pl/grpc/echo:echo_client    -- localhost 50051
bazel run //python/pl/grpc/echo:echo_client  -- localhost 50051
```

### brpc / Starlight Client

测试 2 个 unary RPC（`Echo` + `HealthCheck`）。C++ brpc client 与 Java Starlight client 均支持连接两端 server。

```bash
# C++ brpc client
bazel run //cpp/pl/brpc/echo:echo_client -- --server=localhost:8000   # → C++ brpc
bazel run //cpp/pl/brpc/echo:echo_client -- --server=localhost:8005   # → Java Starlight

# Java Starlight client
bazel run //java/pl/brpc/echo:echo_client -- localhost 8005           # → Java Starlight
bazel run //java/pl/brpc/echo:echo_client -- localhost 8000           # → C++ brpc
```

---

## 互通性矩阵

| Client ↓ / Server → | C++ gRPC | Go gRPC | Java gRPC | Py gRPC | C++ brpc | Java Starlight |
|---------------------|:--------:|:-------:|:---------:|:-------:|:--------:|:--------------:|
| C++ gRPC            | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ |
| Go gRPC             | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ |
| Java gRPC           | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ |
| Python gRPC         | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ |
| C++ brpc            | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ |
| Java Starlight      | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ |

- **gRPC** 生态内全语言互通（HTTP/2 + gRPC 协议），支持全部 5 个 RPC
- **brpc / Starlight** 之间互通（TCP + brpc 二进制协议），支持 2 个 unary RPC
- **gRPC ↔ brpc** 协议不同，无法互通

## RPC 覆盖

| RPC 方法 | gRPC | brpc / Starlight |
|----------|:----:|:----------------:|
| `Echo` (unary) | ✅ | ✅ |
| `ServerStream` (server streaming) | ✅ | ❌ |
| `ClientStream` (client streaming) | ✅ | ❌ |
| `Chat` (bidi streaming) | ✅ | ❌ |
| `HealthCheck` (unary) | ✅ | ✅ |

---

## 典型验证流程

```bash
# ===== brpc 侧 =====
# Terminal 1: 启动 C++ brpc server
bazel run //cpp/pl/brpc/echo:echo_server -- --port=8000

# Terminal 2: 用两个 client 分别测试
bazel run //cpp/pl/brpc/echo:echo_client -- --server=localhost:8000
bazel run //java/pl/brpc/echo:echo_client -- localhost 8000

# Terminal 1: 切到 Java Starlight server
bazel run //java/pl/brpc/echo:echo_server -- localhost 8005

# Terminal 2: 再次测试
bazel run //cpp/pl/brpc/echo:echo_client -- --server=localhost:8005
bazel run //java/pl/brpc/echo:echo_client -- localhost 8005

# ===== gRPC 侧 =====
# Terminal 3: 启动任意 gRPC server
bazel run //python/pl/grpc/echo:echo_server -- 50051 python-server

# Terminal 2: 用所有语言 client 测试
bazel run //cpp/pl/grpc/echo:echo_client     -- --target=localhost:50051
bazel run //go/pl/grpc/echo/client:echo_client  -- localhost:50051
bazel run //java/pl/grpc/echo:echo_client    -- localhost 50051
bazel run //python/pl/grpc/echo:echo_client  -- localhost 50051
```

---

## Maven 构建（仅 Java 模块）

Java 模块同时支持 Bazel 和 Maven 构建：

```bash
# 全部 Java 模块
cd java && mvn compile

# 仅 brpc-echo
cd java && mvn -pl pl/brpc/echo compile

# 仅 grpc-echo
cd java && mvn -pl pl/grpc/echo compile

# 运行
cd java && mvn -pl pl/brpc/echo exec:java -Dexec.mainClass=pl.brpc.echo.EchoServer
cd java && mvn -pl pl/brpc/echo exec:java -Dexec.mainClass=pl.brpc.echo.EchoClient
```
