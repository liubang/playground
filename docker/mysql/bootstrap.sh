#!/usr/bin/env bash
# MySQL 模块：准备认证与绑定配置，并按需启动服务。
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
    echo ""
    echo "首次初始化，请设置 MySQL root 密码（直接回车使用随机密码）"
    prompt_secret MYSQL_ROOT_PASSWORD "MySQL root 密码" 20
    select_public_bind "MySQL 绑定 0.0.0.0，并使用强密码认证"
    write_env_file .env \
        MYSQL_ROOT_PASSWORD "$MYSQL_ROOT_PASSWORD" \
        PUBLIC_BIND_ADDR "$PUBLIC_BIND_ADDR"
    log_ok ".env 已生成（模式: ${BIND_MODE}，MySQL: ${PUBLIC_BIND_ADDR}）"
    echo "  root 密码已写入 .env，请妥善保管"
fi

if [ "$START_SERVICES" = true ]; then
    compose_start
    show_compose_status
    printf '\n==> 连接方式:\n'
    echo "  mysql -h 127.0.0.1 -P 3307 -u root -p"
    echo ""
    echo "完成！首次启动请等待 healthcheck 通过（约 30 秒）。"
else
    print_no_start
fi
