#!/usr/bin/env bash
# Bigdata 模块：准备依赖与认证配置，启动 HDFS/Hive/Spark/Trino。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/bootstrap-common.sh
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

ICEBERG_VERSION="${ICEBERG_VERSION:-1.10.0}"
MYSQL_JAR="mysql-connector-j-8.3.0.jar"
MYSQL_JAR_URL="https://repo1.maven.org/maven2/com/mysql/mysql-connector-j/8.3.0/${MYSQL_JAR}"

bootstrap_check_dependencies curl
bootstrap_handle_reset

echo "==> 准备 .env..."
if [ -f .env ]; then
    log_skip ".env 已存在，不覆盖"
else
    echo ""
    echo "首次初始化，请设置数据库密码"
    prompt_secret MYSQL_ROOT_PASSWORD_VAL "MySQL root 密码" 20
    prompt_secret MYSQL_PASSWORD_VAL "MySQL hive 用户密码" 20
    TRINO_SHARED_SECRET_VAL="$(gen_secret 32)"
    select_bind_mode \
        "仅 MySQL 绑定 0.0.0.0，其余无认证服务仅本机可访问" \
        "所有实验端口绑定 0.0.0.0（仅限可信局域网）" \
        "无认证的 HDFS/Hive/Spark/Trino 将对外开放"
    write_env_file .env \
        MYSQL_ROOT_PASSWORD "$MYSQL_ROOT_PASSWORD_VAL" \
        MYSQL_DATABASE "metastore" \
        MYSQL_USER "hive" \
        MYSQL_PASSWORD "$MYSQL_PASSWORD_VAL" \
        TRINO_SHARED_SECRET "$TRINO_SHARED_SECRET_VAL" \
        PUBLIC_BIND_ADDR "$PUBLIC_BIND_ADDR" \
        INTERNAL_BIND_ADDR "$INTERNAL_BIND_ADDR"
    log_ok ".env 已生成（模式: ${BIND_MODE}，公开入口: ${PUBLIC_BIND_ADDR}，内部服务: ${INTERNAL_BIND_ADDR}）"
fi

echo "==> 准备 MySQL 驱动..."
mkdir -p hive/lib
if [ -f "hive/lib/${MYSQL_JAR}" ]; then
    log_skip "${MYSQL_JAR} 已存在"
else
    log_run "下载 ${MYSQL_JAR} ..."
    curl -fL -o "hive/lib/${MYSQL_JAR}" "$MYSQL_JAR_URL" || die "下载失败 ${MYSQL_JAR_URL}"
    log_ok "下载完成 ${MYSQL_JAR}"
fi

echo "==> 准备 Iceberg Spark runtime..."
mkdir -p spark/jars
ICEBERG_JAR="iceberg-spark-runtime-4.0_2.13-${ICEBERG_VERSION}.jar"
ICEBERG_JAR_URL="https://repo1.maven.org/maven2/org/apache/iceberg/iceberg-spark-runtime-4.0_2.13/${ICEBERG_VERSION}/${ICEBERG_JAR}"
if [ -f "spark/jars/${ICEBERG_JAR}" ]; then
    log_skip "${ICEBERG_JAR} 已存在"
else
    log_run "下载 ${ICEBERG_JAR} ..."
    curl -fL -o "spark/jars/${ICEBERG_JAR}" "$ICEBERG_JAR_URL" || die "下载失败 ${ICEBERG_JAR_URL}"
    log_ok "下载完成 ${ICEBERG_JAR}"
fi

mysql_healthy() {
    docker compose ps mysql | grep -q "healthy"
}

if [ "$START_SERVICES" = true ]; then
    compose_start
    wait_until "MySQL 就绪" 30 2 mysql_healthy || die "MySQL 启动失败"

    echo "==> 初始化 HDFS 目录..."
    docker compose exec -T namenode hdfs dfs -mkdir -p /user/hive/warehouse /tmp 2>/dev/null \
        && docker compose exec -T namenode hdfs dfs -chmod -R 777 /user/hive/warehouse /tmp 2>/dev/null \
        && log_ok "HDFS warehouse 目录已创建" \
        || log_skip "HDFS 目录创建跳过（NameNode 可能尚未就绪）"

    show_compose_status
    printf '\n==> Web UI:\n'
    echo "  HDFS NameNode     http://localhost:9870"
    echo "  HiveServer2       http://localhost:10002"
    echo "  Spark Master      http://localhost:8080"
    echo "  Spark Worker      http://localhost:8081"
    echo "  Trino Coordinator http://localhost:8099"
    echo ""
    echo "完成！首次启动请等待 healthcheck 通过（约 1-2 分钟）。"
else
    print_no_start
fi
