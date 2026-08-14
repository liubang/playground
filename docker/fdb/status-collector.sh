#!/bin/bash
# FDB 状态采集器：周期性执行 `fdbcli status json`，原子写入共享卷 /status/status.json，
# 供 fdb-exporter 容器读取并转换为 Prometheus 指标。
# 使用 FDB 官方镜像运行（镜像自带 fdbcli），失败时静默重试，不退出容器。
set -uo pipefail

INTERVAL="${STATUS_INTERVAL:-15}"

while true; do
    if out="$(fdbcli -C /var/fdb/fdb.cluster --exec 'status json' 2>/dev/null)" && [ -n "$out" ]; then
        # 同目录 tmp + mv 保证 exporter 永远读到完整 JSON
        printf '%s\n' "$out" >/status/.status.json.tmp
        mv /status/.status.json.tmp /status/status.json
    else
        echo "[status-collector] fdbcli status json 失败，${INTERVAL}s 后重试" >&2
    fi
    sleep "$INTERVAL"
done
