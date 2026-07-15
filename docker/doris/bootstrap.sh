#!/usr/bin/env bash
# Doris 模块：启动 FE/BE、初始化示例数据并保护对外 SQL 接口。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/bootstrap-common.sh
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

bootstrap_check_dependencies curl
bootstrap_handle_reset

echo "==> 准备 .env..."
if [ -f .env ]; then
    log_skip ".env 已存在，不覆盖"
else
    prompt_secret DORIS_ROOT_PASSWORD_VAL "Doris root 密码" 20
    select_bind_mode \
        "仅 MySQL 9030 绑定 0.0.0.0，其余端口仅本机可访问" \
        "数据接口和集群内部端口均绑定 0.0.0.0" \
        "FE 选举、HTTP、BE Web/心跳端口也将公开"
    write_env_file .env \
        DORIS_VERSION "4.0.6" \
        DORIS_ROOT_PASSWORD "$DORIS_ROOT_PASSWORD_VAL" \
        PUBLIC_BIND_ADDR "$PUBLIC_BIND_ADDR" \
        INTERNAL_BIND_ADDR "$INTERNAL_BIND_ADDR"
    log_ok ".env 已生成（模式: ${BIND_MODE}，SQL 接口: ${PUBLIC_BIND_ADDR}，内部端口: ${INTERNAL_BIND_ADDR}）"
    echo "  Doris root 密码已写入 .env，请妥善保管"
fi

fe_ready() {
    curl -sf http://localhost:8030/api/bootstrap >/dev/null 2>&1
}

doris_sql_ready() {
    docker compose exec -T fe mysql -uroot -P9030 -h127.0.0.1 -N -e "SELECT 1" >/dev/null 2>&1
}

doris_mysql() {
    docker compose exec -T fe mysql -uroot -P9030 -h127.0.0.1 "$@"
}

doris_backends_ready() {
    local count
    count="$(doris_mysql -N -e "SHOW BACKENDS" 2>/dev/null | \
        awk -F'\t' '{for(i=1;i<=NF;i++) if($i=="true") c++} END{print c+0}')"
    [ "${count:-0}" -ge 2 ]
}

if [ "$START_SERVICES" = true ]; then
    echo "==> 启动 Doris 集群（1 FE + 2 BE）..."
    docker compose up -d
    wait_until "FE 就绪" 60 2 fe_ready || die "请查看日志: docker compose logs fe"
    wait_until "Doris SQL 接口就绪" 60 2 doris_sql_ready || die "请查看日志: docker compose logs fe"
    wait_until "2 个 BE 注册并存活" 60 2 doris_backends_ready || die "请查看日志: docker compose logs be1 be2"

    echo "==> 校验示例数据..."
    ROW_COUNT=""
    for i in $(seq 1 15); do
        ROW_COUNT="$(doris_mysql -N -e "SELECT COUNT(*) FROM demo.orders" 2>/dev/null || true)"
        [ -n "${ROW_COUNT:-}" ] && break
        sleep 2
    done
    if [ -n "${ROW_COUNT:-}" ]; then
        log_ok "示例表 demo.orders 已就绪，共 ${ROW_COUNT} 行"
    else
        log_skip "示例表尚未生成，可能仍在初始化"
    fi

    DORIS_ROOT_PASSWORD="$(grep '^DORIS_ROOT_PASSWORD=' .env | cut -d= -f2-)"
    echo "==> 设置 Doris root 密码..."
    doris_mysql -e "SET PASSWORD FOR 'root'@'%' = PASSWORD('${DORIS_ROOT_PASSWORD}')"
    if docker compose exec -T -e MYSQL_PWD="$DORIS_ROOT_PASSWORD" fe \
        mysql -uroot -P9030 -hfe -N -e "SELECT 1" >/dev/null 2>&1; then
        log_ok "Doris root 密码已生效"
    else
        die "Doris root 密码设置后验证失败"
    fi

    show_compose_status
    printf '\n==> 连接方式:\n'
    echo "  mysql -h 127.0.0.1 -P 9030 -u root -p  # 密码见 .env"
    printf '\n==> Web UI:\n'
    echo "  FE 控制台   http://localhost:8030"
    echo ""
    echo "完成！"
else
    print_no_start
fi
