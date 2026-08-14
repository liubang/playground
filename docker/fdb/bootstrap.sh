#!/usr/bin/env bash
# FoundationDB 模块：启动 3 节点集群（每节点 fdbmonitor 托管 2 个 fdbserver），
# 初始化数据库（double 副本 + ssd 引擎），并生成客户端可用的 fdb.cluster。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/bootstrap-common.sh
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

bootstrap_check_dependencies
bootstrap_handle_reset

echo "==> 准备 .env..."
if [ -f .env ]; then
    log_skip ".env 已存在，不覆盖"
else
    FDB_CLUSTER_ID="$(gen_secret 16)"
    write_env_file .env \
        FDB_VERSION "7.3.63" \
        FDB_CLUSTER_ID "$FDB_CLUSTER_ID"
    log_ok ".env 已生成"
fi

# .env 由 write_env_file 生成（已校验无换行/$），可安全 source
# shellcheck disable=SC1091
source .env

echo "==> 生成客户端 cluster 文件 fdb.cluster..."
printf 'docker:%s@172.28.11.11:4500,172.28.11.12:4500,172.28.11.13:4500\n' "$FDB_CLUSTER_ID" >fdb.cluster
log_ok "fdb.cluster 已生成（容器网络内客户端使用）"

fdbcli_exec() {
    docker compose exec -T fdb-node-1 fdbcli -C /var/fdb/fdb.cluster --exec "$1" 2>&1
}

# fdb-exporter 通过外部网络 monitor_monitor 暴露给 Prometheus 抓取，
# 该网络由 monitor 模块创建；不存在时自动拉起 monitor 模块（其 bootstrap 幂等）。
ensure_monitor_network() {
    if docker network inspect monitor_monitor >/dev/null 2>&1; then
        log_skip "monitor_monitor 网络已存在"
    else
        echo "==> 监控依赖 monitor 模块（Prometheus + Grafana），正在启动..."
        "$SCRIPT_DIR/../monitor/bootstrap.sh"
    fi
}

cluster_available() {
    fdbcli_exec "status minimal" 2>/dev/null | grep -q "The database is available"
}

ensure_monitor_network

if [ "$START_SERVICES" = true ]; then
    echo "==> 启动 FDB 集群（3 节点 × 2 进程）..."
    docker compose up -d

    echo "==> 初始化数据库（configure new double ssd）..."
    configured=false
    for _ in $(seq 1 60); do
        out="$(fdbcli_exec "configure new double ssd" || true)"
        if grep -q "Database created" <<<"$out"; then
            configured=true
            log_ok "数据库已创建（double 副本，ssd 引擎）"
            break
        fi
        if grep -qi "already" <<<"$out"; then
            configured=true
            log_skip "数据库已初始化，跳过 configure new"
            break
        fi
        sleep 2
    done
    [ "$configured" = true ] || die "configure new 失败，请查看日志: docker compose logs"

    wait_until "数据库可用" 90 2 cluster_available || die "请查看日志: docker compose logs"

    show_compose_status
    printf '\n==> 连接方式（FDB 客户端需直连每个进程，请通过容器网络访问）:\n'
    echo "  交互式 fdbcli（推荐）:"
    echo "    docker compose run --rm client"
    echo "  单条命令:"
    echo "    docker compose run --rm client --exec \"status\""
    echo "  在节点容器内:"
    echo "    docker compose exec fdb-node-1 fdbcli -C /var/fdb/fdb.cluster"
    printf '\n==> Trace 日志（排障用，TraceEvent 即文档）:\n'
    echo "    docker compose exec fdb-node-1 ls /var/fdb/logs/4500/"
    printf '\n==> 监控（指标由 fdb-exporter:9189 暴露，经 monitor 模块采集）:\n'
    echo "  Prometheus    http://localhost:9090  (job: foundationdb)"
    echo "  Grafana       http://localhost:3000  (认证信息见 ../monitor/.env，Dashboard: FoundationDB Overview)"
    echo "  Exporter      curl http://fdb-exporter:9189/metrics  (容器网络内)"
    echo ""
    echo "完成！运行 ./tests/e2e.sh all 执行验收测试。"
else
    print_no_start
fi
