#!/usr/bin/env bash
# FoundationDB 模块 E2E 验收测试。
# 用法: ./tests/e2e.sh {all|start|test|fault-test|persistence-test|down|reset}
#   all               启动（如需）并执行全部测试
#   start             启动集群并初始化（幂等，保留已有数据卷）
#   test              集群状态 + 读写 + 独立客户端容器断言
#   fault-test        停掉一个节点验证双副本容错，再恢复
#   persistence-test  全集群重启后验证 ssd 引擎数据持久化
#   down / reset      停止 / 停止并清除数据卷
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FDB_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=../../scripts/bootstrap-common.sh
source "$FDB_DIR/../scripts/bootstrap-common.sh"
COMPOSE=(docker compose --project-directory "$FDB_DIR" -f "$FDB_DIR/docker-compose.yml")

compose() { "${COMPOSE[@]}" "$@"; }

on_error() {
    local status=$?
    log_fail "FDB E2E 失败（exit ${status}）。容器与数据卷已保留。"
    compose ps >&2 || true
    printf '[INFO] 排查: cd %s && docker compose logs --tail=200\n' "$FDB_DIR" >&2
    exit "$status"
}
trap on_error ERR

# 在 node-1 上执行 fdbcli 命令
fdbcli_exec() {
    compose exec -T fdb-node-1 fdbcli -C /var/fdb/fdb.cluster --exec "$1" 2>&1
}

# 在指定节点上执行 fdbcli 命令
fdbcli_on() {
    local node="$1" cmd="$2"
    compose exec -T "$node" fdbcli -C /var/fdb/fdb.cluster --exec "$cmd" 2>&1
}

status_json() {
    compose exec -T fdb-node-1 fdbcli -C /var/fdb/fdb.cluster --exec "status json" 2>/dev/null
}

# 用容器内镜像自带的 jq 解析 status json
status_jq() {
    status_json | compose exec -T -i fdb-node-1 jq -r "$1" 2>/dev/null
}

cluster_available() {
    fdbcli_exec "status minimal" 2>/dev/null | grep -q "The database is available"
}

cluster_healthy() {
    [ "$(status_jq '.cluster.data.state.name // empty')" = "healthy" ]
}

configure_if_needed() {
    local out
    out="$(fdbcli_exec "configure new double ssd" || true)"
    if grep -q "Database created" <<<"$out"; then
        log_ok "数据库已创建（double 副本，ssd 引擎）"
    elif grep -qi "already" <<<"$out"; then
        log_skip "数据库已初始化，跳过 configure new"
    else
        return 1
    fi
}

start_cluster() {
    "$FDB_DIR/bootstrap.sh" --no-start
    log_run "启动 FDB 集群（保留已有数据卷）"
    compose up -d
    wait_until "coordinator 就绪并完成数据库初始化" 90 2 configure_if_needed ||
        die "configure 失败，请查看: docker compose logs"
    wait_until "数据库可用" 90 2 cluster_available || die "请查看: docker compose logs"
}

assert_status() {
    log_run "断言集群状态：6 进程、3 coordinator、double 副本、healthy"
    local processes coordinators replication state
    processes="$(status_jq '.cluster.processes | length')"
    [ "$processes" = "6" ] || die "期望 6 个进程，实际: ${processes}"
    coordinators="$(status_jq '[.client.coordinators.coordinators[] | select(.reachable == true)] | length')"
    [ "$coordinators" = "3" ] || die "期望 3 个可达 coordinator，实际: ${coordinators}"
    replication="$(status_jq '.cluster.configuration.redundancy_mode')"
    [ "$replication" = "double" ] || die "副本模式异常: ${replication}"
    state="$(status_jq '.cluster.data.state.name')"
    [ "$state" = "healthy" ] || die "集群数据状态异常: ${state}"
    log_ok "集群状态断言通过（6 进程 / 3 coordinator / ${replication} / ${state}）"
}

assert_read_write() {
    log_run "断言读写：node-1 写入，node-3 读取（跨节点可见性）"
    fdbcli_exec "writemode on; set e2e/hello fdb-works" >/dev/null
    local got
    got="$(fdbcli_exec "get e2e/hello")"
    grep -q "fdb-works" <<<"$got" || die "node-1 读取异常: ${got}"
    got="$(fdbcli_on fdb-node-3 "get e2e/hello")"
    grep -q "fdb-works" <<<"$got" || die "node-3 读取异常: ${got}"

    fdbcli_exec "writemode on; set e2e/k1 v1; set e2e/k2 v2; set e2e/k3 v3" >/dev/null
    got="$(fdbcli_exec "getrange e2e/k1 e2e/k9")"
    for k in k1 k2 k3; do
        grep -q "e2e/${k}" <<<"$got" || die "范围读缺少 e2e/${k}: ${got}"
    done
    log_ok "读写与范围查询断言通过"
}

assert_external_client() {
    log_run "断言独立客户端容器可访问集群（跨容器客户端路径）"
    grep -Eq '^docker:[A-Za-z0-9]+@172\.28\.11\.11:4500,172\.28\.11\.12:4500,172\.28\.11\.13:4500$' \
        "$FDB_DIR/fdb.cluster" || die "fdb.cluster 内容异常: $(cat "$FDB_DIR/fdb.cluster")"
    local got
    got="$(compose run --rm -T client --exec "get e2e/hello" 2>&1)"
    grep -q "fdb-works" <<<"$got" || die "独立客户端容器读取失败: ${got}"
    log_ok "独立客户端容器（compose run client）读写正常"
}

run_tests() {
    wait_until "数据库可用" 60 2 cluster_available || die "数据库不可用"
    assert_status
    assert_read_write
    assert_external_client
    log_ok "基础功能测试全部通过"
}

run_fault_test() {
    log_run "容错测试：写入数据后停止 fdb-node-3"
    fdbcli_exec "writemode on; set e2e/fault before-stop" >/dev/null
    compose stop fdb-node-3
    wait_until "单节点故障后数据库恢复可用" 120 2 cluster_available ||
        die "停止一个节点后数据库未恢复"
    local got
    got="$(fdbcli_exec "get e2e/fault")"
    grep -q "before-stop" <<<"$got" || die "故障后读取数据丢失: ${got}"
    fdbcli_exec "writemode on; set e2e/fault2 during-outage" >/dev/null
    got="$(fdbcli_exec "get e2e/fault2")"
    grep -q "during-outage" <<<"$got" || die "故障期间写入失败: ${got}"
    log_ok "单节点故障下读写正常（双副本生效）"

    log_run "恢复 fdb-node-3，等待副本重新同步"
    compose start fdb-node-3
    wait_until "数据库恢复可用" 120 2 cluster_available || die "节点恢复后数据库不可用"
    wait_until "集群恢复 healthy" 180 2 cluster_healthy || die "节点恢复后集群未回到 healthy"
    got="$(fdbcli_exec "get e2e/fault")"
    grep -q "before-stop" <<<"$got" || die "节点恢复后数据异常: ${got}"
    log_ok "节点恢复，副本同步完成"
}

run_persistence_test() {
    log_run "持久化测试：写入数据后重启全部容器"
    fdbcli_exec "writemode on; set e2e/persist survive-restart" >/dev/null
    compose restart
    wait_until "全集群重启后数据库恢复可用" 180 2 cluster_available ||
        die "重启后数据库未恢复"
    wait_until "重启后集群恢复 healthy" 180 2 cluster_healthy || die "重启后集群未回到 healthy"
    local got
    got="$(fdbcli_exec "get e2e/persist")"
    grep -q "survive-restart" <<<"$got" || die "重启后数据丢失: ${got}"
    log_ok "全集群重启后数据仍在（ssd 引擎持久化生效）"
}

cleanup_test_data() {
    fdbcli_exec "writemode on; clearrange e2e/ e2e0" >/dev/null 2>&1 || true
}

usage() {
    echo "Usage: $0 {all|start|test|fault-test|persistence-test|down|reset}"
}

main() {
    local command="${1:-all}"
    bootstrap_check_dependencies
    case "$command" in
    all)
        start_cluster
        run_tests
        run_fault_test
        run_persistence_test
        cleanup_test_data
        printf '\n'
        log_ok "FDB E2E 全部通过"
        ;;
    start) start_cluster ;;
    test) run_tests ;;
    fault-test) run_fault_test ;;
    persistence-test) run_persistence_test ;;
    down) compose down ;;
    reset) compose down -v --remove-orphans ;;
    *)
        usage
        exit 2
        ;;
    esac
}

main "$@"
