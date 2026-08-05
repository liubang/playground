#!/bin/bash
# 每个 FDB 节点容器启动时执行：
#   1. 按环境变量生成 fdb.cluster（3 个 coordinator，所有节点内容一致）
#   2. 生成 foundationdb.conf（fdbmonitor 托管 2 个 fdbserver 进程）
#   3. 启动 fdbmonitor（生产同款进程监管方式）
#
# 必需环境变量（见 docker-compose.yml）：
#   NODE_NAME        节点名，用作 locality zoneid/machineid（保证副本跨节点分布）
#   NODE_IP          节点在 compose 网络中的固定 IP
#   FDB_CLUSTER_ID   集群 ID（bootstrap.sh 生成，所有节点一致）
#   FDB_COORDINATORS 3 个 coordinator 的容器内地址
#
# 注意：fdbserver 最多支持 2 个 public address，且必须分属不同 TLS 状态
# （一个 TLS、一个非 TLS），因此无法用"双非 TLS 地址 + 端口映射"让
# macOS 宿主机客户端直连。客户端请通过容器网络访问（见 bootstrap.sh 输出）。
set -euo pipefail

: "${NODE_NAME:?NODE_NAME is required}"
: "${NODE_IP:?NODE_IP is required}"
: "${FDB_CLUSTER_ID:?FDB_CLUSTER_ID is required}"
: "${FDB_COORDINATORS:?FDB_COORDINATORS is required}"

# fdbserver 不会自建 datadir/logdir（$ID 会被 fdbmonitor 替换为端口号），需预先创建
mkdir -p /var/fdb/data/4500 /var/fdb/data/4501 /var/fdb/logs/4500 /var/fdb/logs/4501
chown -R fdb:fdb /var/fdb/data /var/fdb/logs
echo "docker:${FDB_CLUSTER_ID}@${FDB_COORDINATORS}" > /var/fdb/fdb.cluster

cat > /var/fdb/foundationdb.conf <<EOF
[fdbmonitor]
user = fdb

[general]
cluster-file = /var/fdb/fdb.cluster
restart-delay = 5

[fdbserver]
command = /usr/bin/fdbserver
datadir = /var/fdb/data/\$ID
logdir = /var/fdb/logs/\$ID
memory = 1GiB
cache-memory = 256MiB
knob_disable_posix_kernel_aio = 1
locality-zoneid = ${NODE_NAME}
locality-machineid = ${NODE_NAME}

[fdbserver.4500]
public-address = ${NODE_IP}:4500
listen-address = 0.0.0.0:4500

[fdbserver.4501]
public-address = ${NODE_IP}:4501
listen-address = 0.0.0.0:4501
EOF

exec /usr/bin/fdbmonitor --conffile /var/fdb/foundationdb.conf
