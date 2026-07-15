# MiniDFS 测试指南

本文档用于手动复现 `tests/e2e.sh` 覆盖的全部测试场景，包括 Bazel 单元测试、Docker 镜像构建、集群健康检查、所有 MiniDFS CLI 命令、多级目录与 `ls` 选项、文件大小边界、多块三副本读写、追加、截断、覆盖写、副本数调整和递归清理。

> **执行目录**：除非特别说明，所有命令均从仓库的 `docker/minidfs/` 目录执行。
>
> **数据安全**：本文的全新集群启动步骤会执行 `docker compose down -v`，删除 MiniDFS MySQL 元数据和三个 DataNode 数据卷。请勿对需要保留数据的环境执行。

## 目录

- [0. 前置条件与命令约定](#0-前置条件与命令约定)
- [1. 构建与单元测试](#1-构建与单元测试)
- [2. 启动全新集群](#2-启动全新集群)
- [3. 集群状态与管理命令](#3-集群状态与管理命令)
- [4. 多级目录与 Ls 测试](#4-多级目录与-ls-测试)
- [5. 文件尺寸矩阵与三副本读写](#5-文件尺寸矩阵与三副本读写)
- [6. Append 与 Truncate](#6-append-与-truncate)
- [7. 覆盖写、Setrep 与递归清理](#7-覆盖写setrep-与递归清理)
- [8. 自动化脚本对照](#8-自动化脚本对照)
- [9. 清理与排障](#9-清理与排障)

---

## 0. 前置条件与命令约定

### 0.1 前置条件

宿主机需要安装并可运行：

- Docker 和 Docker Compose v2
- Bazel 8.7.0（仓库通过 `.bazelversion` 锁定版本）
- Bash、`cmp`、`dd`、`wc`、`grep`
- 至少约 4 GiB 可用内存，以及足够的镜像和 Bazel 缓存空间

```bash
cd docker/minidfs

docker info >/dev/null
docker compose version
bazel --version
```

预期：Docker daemon 可访问，Compose 和 Bazel 均正常输出版本。

首次执行时创建本地配置：

```bash
cp -n .env.example .env
chmod 600 .env

docker compose config --quiet
```

默认仅绑定宿主机环回地址：

| 服务 | 容器内地址 | 宿主机地址 |
| --- | --- | --- |
| MySQL | `mysql:3306` | `127.0.0.1:13306` |
| NameNode | `namenode:9000` | `127.0.0.1:19000` |
| DataNode 1/2/3 | `datanode1/2/3:9100` | 不映射到宿主机 |

### 0.2 定义手动测试辅助变量

下面的辅助函数只简化命令输入，不执行断言或隐藏测试逻辑。后续章节均假定它们仍存在于当前 Shell 中。

```bash
# 确保当前目录为 docker/minidfs
MINIDFS_DIR="$PWD"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidfs-manual.XXXXXX")"

# 在一次性 CLI 容器中调用 MiniDFS，并将测试目录挂载为 /work。
cli() {
    docker compose run --rm -T \
        -v "$WORK_DIR:/work" \
        cli --namenode=namenode:9000 "$@"
}

printf '测试文件目录: %s\n' "$WORK_DIR"
```

> MiniDFS 使用 gflags 解析全局参数。子命令自身带短选项时，必须使用 `--` 结束全局参数解析，例如 `cli -- put -f ...` 和 `cli -- rm -r ...`。

---

## 1. 构建与单元测试

### 1.1 运行 MiniDFS 全量 Bazel 测试

从仓库根目录执行：

```bash
cd ../..
bazel test //cpp/pl/minidfs/... --test_output=errors
cd docker/minidfs
```

预期：所有 MiniDFS 测试通过，无失败目标。

如只需回归 Append 版本 CAS 修复，可执行：

```bash
cd ../..
bazel test //cpp/pl/minidfs/namenode/tests:namespace_manager_test --test_output=errors
bazel test //cpp/pl/minidfs/client:dfs_output_stream_test --test_output=errors
cd docker/minidfs
```

### 1.2 构建通用 Bazel Builder 镜像

如果本机尚无 `liubang/bazel-builder:8.7.0`：

```bash
docker build \
    --build-arg BAZEL_VERSION=8.7.0 \
    -f ../bazel-builder/Dockerfile \
    -t liubang/bazel-builder:8.7.0 \
    ../bazel-builder

docker run --rm liubang/bazel-builder:8.7.0 bazel --version
```

预期：最后输出 `bazel 8.7.0`。

### 1.3 构建 MiniDFS Runtime 镜像

```bash
BUILDER_IMAGE=liubang/bazel-builder:8.7.0 docker compose build namenode
docker image inspect liubang/minidfs:local >/dev/null
```

构建使用 BuildKit cache 保存 Bazel repository cache 和 output tree。首次构建耗时较长，后续未修改的 action 应大量命中缓存。

如果容器无法从官方站点下载某个 Bazel 外部依赖，可将对应原始归档放入当前目录；Dockerfile 已通过 `--distdir=/src/docker/minidfs` 优先查找本地归档。`*.tar.xz` 已被 `.gitignore` 忽略，不应提交到仓库。

---

## 2. 启动全新集群

以下命令会删除旧容器、网络及命名卷，然后启动 1 个 MySQL、1 个 NameNode 和 3 个 DataNode：

```bash
docker compose down -v --remove-orphans
docker compose up -d mysql namenode datanode1 datanode2 datanode3
```

等待所有服务健康：

```bash
for i in $(seq 1 90); do
    unhealthy="$(docker compose ps --format json | grep -Ec 'starting|unhealthy' || true)"
    running="$(docker compose ps --services --status running | wc -l | tr -d ' ')"
    if [ "$unhealthy" -eq 0 ] && [ "$running" -eq 5 ]; then
        break
    fi
    sleep 2
done

docker compose ps
```

预期：`mysql`、`namenode`、`datanode1`、`datanode2`、`datanode3` 均显示 `Up ... (healthy)`。

也可以分别检查 HTTP 健康端点：

```bash
curl -fsS http://127.0.0.1:19000/status >/dev/null

docker compose exec -T datanode1 curl -fsS http://127.0.0.1:9100/status >/dev/null
docker compose exec -T datanode2 curl -fsS http://127.0.0.1:9100/status >/dev/null
docker compose exec -T datanode3 curl -fsS http://127.0.0.1:9100/status >/dev/null

echo "所有健康端点正常"
```

---

## 3. 集群状态与管理命令

### 3.1 查看文件系统概况

```bash
cli fsinfo
```

预期：输出包含 `Live DataNodes`，数量为 `3`。

可执行显式断言：

```bash
fsinfo="$(cli fsinfo)"
printf '%s\n' "$fsinfo"
grep -Eq 'Live DataNodes[^0-9]*3' <<<"$fsinfo"
```

### 3.2 查看 DataNode 列表

```bash
datanodes="$(cli datanodes)"
printf '%s\n' "$datanodes"

for node in datanode1 datanode2 datanode3; do
    grep -q "$node" <<<"$datanodes" || {
        echo "缺少已注册节点: $node" >&2
        exit 1
    }
done
```

预期：列表中恰有 `datanode1`、`datanode2`、`datanode3`，状态均为 `live`。

### 3.3 覆盖 `datanodes --all` 和 `datanode <id>`

```bash
all_datanodes="$(cli -- datanodes --all)"
printf '%s\n' "$all_datanodes"
grep -q 'datanode1' <<<"$all_datanodes"

datanode_id="$(awk '$2 ~ /^datanode[123]$/ {print $1; exit}' <<<"$datanodes")"
[[ "$datanode_id" =~ ^[0-9]+$ ]]

datanode_detail="$(cli datanode "$datanode_id")"
printf '%s\n' "$datanode_detail"
grep -q 'DataNode ID' <<<"$datanode_detail"
grep -Eq 'datanode[123]' <<<"$datanode_detail"
```

预期：`--all` 输出包含全部节点，按 ID 查询能返回单个 DataNode 的 UUID、地址、容量、状态和心跳时间。

---

## 4. 多级目录与 Ls 测试

先清理可能由上次中断测试遗留的目录：

```bash
if cli stat /e2e >/dev/null 2>&1; then
    cli -- rm -r /e2e
fi
```

一次创建多级目录并检查类型：

```bash
cli mkdir /e2e
cli mkdir /e2e/level1/level2/level3

stat_output="$(cli stat /e2e/level1/level2/level3)"
printf '%s\n' "$stat_output"
grep -q 'directory' <<<"$stat_output"
```

移动最深层目录并检查结果：

```bash
cli mv /e2e/level1/level2/level3 /e2e/level1/level2/data

ls_output="$(cli ls /e2e/level1/level2)"
printf '%s\n' "$ls_output"
grep -q '/e2e/level1/level2/data' <<<"$ls_output"
```

覆盖默认路径以及 `ls` 的全部选项：

```bash
# 默认列出根目录
cli ls | grep -q '/e2e'

# 目录自身、递归、人类可读大小、时间排序、反向排序
cli -- ls -d /e2e/level1 | grep -q '/e2e/level1'
cli -- ls -R /e2e | grep -q '/e2e/level1/level2/data'
cli -- ls -h /e2e/level1/level2 | grep -q '/e2e/level1/level2/data'
cli -- ls -t /e2e/level1/level2
cli -- ls -r /e2e/level1/level2

# 组合短选项：human-readable + recursive + time sort
cli -- ls -hRt /e2e
```

上传尺寸矩阵后，第 5 节还会验证 `ls -S`、`ls -Sr` 的大小排序，以及 `ls -h` 的大小格式。

---

## 5. 文件尺寸矩阵与三副本读写

### 5.1 生成跨 Block 边界的测试矩阵

统一使用 1 MiB Block，覆盖空文件、极小文件、Block 边界前后以及多 Block 文件：

| 用例 | 字节数 | 目的 |
| --- | ---: | --- |
| `empty` | 0 | 空文件 |
| `one-byte` | 1 | 最小非空文件 |
| `one-kib` | 1024 | 小文件 |
| `block-minus-one` | 1048575 | Block 边界前 1 字节 |
| `exact-block` | 1048576 | 恰好 1 Block |
| `block-plus-one` | 1048577 | 跨越到第 2 Block |
| `multi-block` | 2097185 | 2 个完整 Block 加 33 字节 |

```bash
block_size=1048576
make_test_file() {
    local path="$1" size="$2"
    dd if=/dev/zero of="$path" bs=1 count="$size" status=none
    if ((size > 0)); then
        printf '\x5a' | dd of="$path" bs=1 seek=$((size - 1)) conv=notrunc status=none
    fi
}

size_cases=(
    "empty:0"
    "one-byte:1"
    "one-kib:1024"
    "block-minus-one:$((block_size - 1))"
    "exact-block:$block_size"
    "block-plus-one:$((block_size + 1))"
    "multi-block:$((2 * block_size + 33))"
)

for case_spec in "${size_cases[@]}"; do
    case_name="${case_spec%%:*}"
    case_size="${case_spec##*:}"
    make_test_file "$WORK_DIR/$case_name.bin" "$case_size"
done
```

### 5.2 逐个执行三副本 Put/Get 与完整性校验

```bash
for case_spec in "${size_cases[@]}"; do
    case_name="${case_spec%%:*}"
    case_size="${case_spec##*:}"

    cli --block_size="$block_size" --replication=3 \
        put "/work/$case_name.bin" "/e2e/level1/level2/data/$case_name.bin"
    cli get "/e2e/level1/level2/data/$case_name.bin" "/work/$case_name.download.bin"

    cmp "$WORK_DIR/$case_name.bin" "$WORK_DIR/$case_name.download.bin"
    actual_size="$(wc -c <"$WORK_DIR/$case_name.download.bin" | tr -d ' ')"
    [ "$actual_size" = "$case_size" ]
done
```

### 5.3 验证 `ls` 大小排序与格式化

```bash
size_sorted="$(cli -- ls -S /e2e/level1/level2/data)"
printf '%s\n' "$size_sorted"
# 预期：multi-block.bin 出现在 one-byte.bin 之前

reverse_size_sorted="$(cli -- ls -Sr /e2e/level1/level2/data)"
printf '%s\n' "$reverse_size_sorted"
# 预期：one-byte.bin 出现在 multi-block.bin 之前

cli -- ls -h /e2e/level1/level2/data
# 预期：输出中出现 1.0K、1.0M、2.0M 等人类可读大小
```

### 5.4 覆盖 `inode`、`blocks` 和 `block`

```bash
target_path=/e2e/level1/level2/data/multi-block.bin

inode_by_path="$(cli inode "$target_path")"
printf '%s\n' "$inode_by_path"
inode_id="$(awk '$1 == "Inode" && $2 == "ID" {print $3; exit}' <<<"$inode_by_path")"
[[ "$inode_id" =~ ^[0-9]+$ ]]

# inode 同时支持路径和 ID
cli inode "$inode_id" | grep -q 'multi-block.bin'

# blocks 同时支持路径和 inode ID
blocks="$(cli blocks "$target_path")"
printf '%s\n' "$blocks"
cli blocks "$inode_id" | grep -q 'Block ID'

grep -q 'datanode1:9100' <<<"$blocks"
grep -q 'datanode2:9100' <<<"$blocks"
grep -q 'datanode3:9100' <<<"$blocks"

# 从 blocks 表格首行提取 Block ID，再查询单 Block 详情
block_id="$(awk '$1 ~ /^[0-9]+$/ {print $1; exit}' <<<"$blocks")"
block_detail="$(cli block "$block_id")"
printf '%s\n' "$block_detail"
grep -q 'Replicas:' <<<"$block_detail"
```

预期：多块文件包含 3 个 Block，每个 Block 的 locations/replicas 能看到三个 DataNode。

---

## 6. Append 与 Truncate

### 6.1 Append

```bash
printf 'appended payload\n' >"$WORK_DIR/append.txt"

target_path=/e2e/level1/level2/data/multi-block.bin
cli append /work/append.txt "$target_path"
cli get "$target_path" /work/appended.bin

cat "$WORK_DIR/multi-block.bin" "$WORK_DIR/append.txt" >"$WORK_DIR/expected-appended.bin"
cmp "$WORK_DIR/expected-appended.bin" "$WORK_DIR/appended.bin"
echo "append 内容一致"
```

预期：下载文件严格等于原文件与 `append.txt` 的拼接结果。

### 6.2 Truncate

将文件截断为恰好 1 MiB：

```bash
cli truncate 1048576 "$target_path"
cli get "$target_path" /work/truncated.bin

actual_size="$(wc -c <"$WORK_DIR/truncated.bin" | tr -d ' ')"
[ "$actual_size" = 1048576 ]

cmp \
    <(dd if="$WORK_DIR/multi-block.bin" bs=1048576 count=1 status=none) \
    "$WORK_DIR/truncated.bin"

echo "truncate 长度及内容一致"
```

预期：截断后长度为 `1048576`，内容等于原始文件的前 1 MiB。

---

## 7. 覆盖写、Setrep 与删除

### 7.1 覆盖已有文件

```bash
printf 'replacement\n' >"$WORK_DIR/replacement.txt"

# -- 用于结束 gflags 全局参数解析，使 -f 传给 put 子命令。
cli -- put -f /work/replacement.txt "$target_path"
```

### 7.2 调整副本数

```bash
cli setrep 2 "$target_path"
cli stat "$target_path"
```

预期：命令成功，文件元数据中的 replication 变为 `2`。副本增删由后台任务异步完成，因此本测试只要求元数据更新成功，不要求命令返回瞬间物理副本数已经收敛。

### 7.3 校验覆盖写内容

```bash
cli get "$target_path" /work/replacement-download.txt
cmp "$WORK_DIR/replacement.txt" "$WORK_DIR/replacement-download.txt"
echo "overwrite 内容一致"
```

### 7.4 文件移动与非递归删除

```bash
cli mv /e2e/level1/level2/data/one-kib.bin /e2e/level1/level2/data/moved.bin
cli stat /e2e/level1/level2/data/moved.bin | grep -q 'file'
cli rm /e2e/level1/level2/data/moved.bin

if cli stat /e2e/level1/level2/data/moved.bin >/dev/null 2>&1; then
    echo "非递归 rm 未删除 moved.bin" >&2
    exit 1
fi
```

### 7.5 递归删除

```bash
# -- 用于结束 gflags 全局参数解析，使 -r 传给 rm 子命令。
cli -- rm -r /e2e

if cli stat /e2e >/dev/null 2>&1; then
    echo "/e2e 在递归删除后仍然存在" >&2
    exit 1
fi

echo "递归清理成功"
```

至此，`mkdir`、`ls`、`stat`、`rm`、`mv`、`put`、`append`、`truncate`、`setrep`、`get`、`fsinfo`、`datanodes`、`datanode`、`inode`、`blocks`、`block` 全部 CLI 命令及其关键选项均已手动复现。

清理宿主机临时文件：

```bash
rm -rf "$WORK_DIR"
unset WORK_DIR MINIDFS_DIR
unset -f cli
```

---

## 8. 自动化脚本对照

手动验证完成后，可运行自动化脚本进行对照：

```bash
# 单元测试 + Builder/Runtime 构建 + 全新集群 + 所有 E2E 断言
./tests/e2e.sh all
```

支持的子命令：

| 子命令 | 行为 |
| --- | --- |
| `build` | 运行全部 MiniDFS Bazel 测试，并构建 Builder/Runtime 镜像 |
| `start` | 删除旧容器和数据卷，启动全新集群并验证 3 个 DataNode 注册 |
| `test` | 在当前集群执行全部 E2E 断言；开始时清理遗留 `/e2e` |
| `down` | 停止并删除容器，保留命名卷 |
| `reset` | 停止集群，并删除容器、命名卷和孤儿容器 |
| `all` | 依次执行 `build`、`start`、`test` |

测试失败时脚本会保留容器和命名卷，以便查看现场。可以调整等待超时：

```bash
E2E_TIMEOUT_SECONDS=600 ./tests/e2e.sh all
```

也可指定其他 Builder 镜像：

```bash
BAZEL_BUILDER_IMAGE=liubang/bazel-builder:8.7.0 ./tests/e2e.sh build
```

---

## 9. 清理与排障

### 9.1 停止或完全重置

```bash
# 停止容器但保留数据卷
docker compose down

# 删除容器及全部 MiniDFS 数据卷
docker compose down -v --remove-orphans
```

也可以使用脚本：

```bash
./tests/e2e.sh down
./tests/e2e.sh reset
```

### 9.2 查看状态和日志

```bash
docker compose ps

docker compose logs --tail=200 mysql
docker compose logs --tail=200 namenode
docker compose logs --tail=200 datanode1 datanode2 datanode3

docker compose logs -f namenode datanode1 datanode2 datanode3
```

### 9.3 检查 MySQL 元数据

```bash
MYSQL_ROOT_PASSWORD="$(grep '^MYSQL_ROOT_PASSWORD=' .env | cut -d= -f2-)"
docker compose exec -T \
    -e MYSQL_PWD="$MYSQL_ROOT_PASSWORD" \
    mysql mysql -uroot -e 'USE minidfs; SHOW TABLES;'
```

### 9.4 常见问题

| 问题 | 原因 | 处理方式 |
| --- | --- | --- |
| Builder 下载 Bazel 或外部依赖超时 | 容器网络无法访问依赖站点 | 重试，配置 Docker 网络，或把 Bazel 所需原始归档放入 `docker/minidfs/` 供 `--distdir` 使用 |
| NameNode 一直不健康 | MySQL 未就绪、连接参数错误或 schema 初始化失败 | 查看 `docker compose logs mysql namenode`，并检查 `.env` |
| DataNode 健康但 `datanodes` 缺节点 | 注册或心跳异常 | 查看对应 DataNode 和 NameNode 日志，必要时执行 `docker compose restart datanode1 datanode2 datanode3` |
| `unknown command line flag 'f'` | `-f` 被 gflags 当成全局参数 | 使用 `cli -- put -f ...` |
| `unknown command line flag 'r'` | `-r` 被 gflags 当成全局参数 | 使用 `cli -- rm -r ...` |
| `destination 'data' already exists` | 上一次手动测试未清理 `/e2e` | 执行 `cli -- rm -r /e2e` 后重试 |
| Append 报 version conflict | Runtime 镜像不是当前源码构建结果 | 重新执行 `docker compose build namenode`，再重建全新集群 |
| 宿主机端口冲突 | `13306` 或 `19000` 已被占用 | 在 `.env` 中修改 `MYSQL_PORT` 或 `NAMENODE_PORT` |
