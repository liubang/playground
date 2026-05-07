# braft Counter 示例

基于 [braft](https://github.com/baidu/braft) 实现的分布式计数器，演示 Raft 共识协议的基本用法。

## 构建

```bash
bazel build //cpp/pl/braft/...
```

## 运行

启动一个 3 节点集群：

```bash
# 节点 1
bazel-bin/cpp/pl/braft/counter_server \
  --port=8100 \
  --conf="127.0.0.1:8100:0,127.0.0.1:8101:0,127.0.0.1:8102:0" \
  --data_path=./data1

# 节点 2
bazel-bin/cpp/pl/braft/counter_server \
  --port=8101 \
  --conf="127.0.0.1:8100:0,127.0.0.1:8101:0,127.0.0.1:8102:0" \
  --data_path=./data2

# 节点 3
bazel-bin/cpp/pl/braft/counter_server \
  --port=8102 \
  --conf="127.0.0.1:8100:0,127.0.0.1:8101:0,127.0.0.1:8102:0" \
  --data_path=./data3
```

发送请求：

```bash
bazel-bin/cpp/pl/braft/counter_client \
  --conf="127.0.0.1:8100:0,127.0.0.1:8101:0,127.0.0.1:8102:0" \
  --log_each_request
```

## 功能说明

- **counter_server** — 实现 `braft::StateMachine`，通过 Raft 日志复制保证写操作（`fetch_add`）的强一致性，支持 snapshot 持久化与恢复。
- **counter_client** — 利用 braft RouteTable 自动发现 leader，处理 leader 切换与重定向，支持 bthread/pthread 两种并发模式。

## 常用参数

| 参数                    | 默认值 | 说明                                   |
| ----------------------- | ------ | -------------------------------------- |
| `--port`                | 8100   | 监听端口                               |
| `--conf`                | (空)   | 集群节点列表，格式 `ip:port:index,...` |
| `--data_path`           | ./data | 数据存储目录                           |
| `--election_timeout_ms` | 5000   | 选举超时（毫秒）                       |
| `--snapshot_interval`   | 30     | 快照间隔（秒）                         |
| `--thread_num`          | 1      | 客户端并发线程数                       |
| `--added_by`            | 1      | 每次递增的值                           |
