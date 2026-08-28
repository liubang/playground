#!/usr/bin/env bash
# Doris 模块：启动 FE/BE、初始化示例数据、配置 MySQL 外表 Catalog 并保护对外 SQL 接口。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/bootstrap-common.sh
source "$SCRIPT_DIR/../scripts/bootstrap-common.sh"
bootstrap_init "$SCRIPT_DIR" "$@"

bootstrap_check_dependencies curl
bootstrap_handle_reset

DRIVER_JAR_NAME="mysql-connector-j-8.3.0.jar"
DRIVER_JAR_URL="https://repo1.maven.org/maven2/com/mysql/mysql-connector-j/8.3.0/${DRIVER_JAR_NAME}"
FEDERATION_NETWORK="doris-bigdata-federation"

echo "==> 准备 .env..."
if [ -f .env ]; then
    log_skip ".env 已存在，不覆盖"
    # 旧版 .env 可能缺少外表数据源密码，幂等补齐
    if ! grep -q '^MYSQL_ROOT_PASSWORD=' .env; then
        printf 'MYSQL_ROOT_PASSWORD=%s\n' "$(gen_secret 20)" >>.env
        log_ok ".env 已补充 MYSQL_ROOT_PASSWORD（MySQL 外部数据源密码）"
    fi
else
    prompt_secret DORIS_ROOT_PASSWORD_VAL "Doris root 密码" 20
    MYSQL_ROOT_PASSWORD_VAL="$(gen_secret 20)"
    select_bind_mode \
        "仅 MySQL 9030 绑定 0.0.0.0，其余端口仅本机可访问" \
        "数据接口和集群内部端口均绑定 0.0.0.0" \
        "FE 选举、HTTP、BE Web/心跳端口也将公开"
    write_env_file .env \
        DORIS_VERSION "4.0.6" \
        DORIS_ROOT_PASSWORD "$DORIS_ROOT_PASSWORD_VAL" \
        MYSQL_ROOT_PASSWORD "$MYSQL_ROOT_PASSWORD_VAL" \
        PUBLIC_BIND_ADDR "$PUBLIC_BIND_ADDR" \
        INTERNAL_BIND_ADDR "$INTERNAL_BIND_ADDR"
    log_ok ".env 已生成（模式: ${BIND_MODE}，SQL 接口: ${PUBLIC_BIND_ADDR}，内部端口: ${INTERNAL_BIND_ADDR}）"
    echo "  Doris root 密码已写入 .env，请妥善保管"
fi

DORIS_ROOT_PASSWORD="$(grep '^DORIS_ROOT_PASSWORD=' .env | cut -d= -f2-)"
MYSQL_ROOT_PASSWORD="$(grep '^MYSQL_ROOT_PASSWORD=' .env | cut -d= -f2-)"

echo "==> 准备 JDBC 驱动（外表 Catalog 依赖）..."
mkdir -p jdbc-drivers
if [ -f "jdbc-drivers/${DRIVER_JAR_NAME}" ]; then
    log_skip "jdbc-drivers/${DRIVER_JAR_NAME} 已存在，不重复下载"
# 优先复用 bigdata 模块已下载的驱动（版本一致，免去重复拉取）
elif [ -f "../bigdata/hive/lib/${DRIVER_JAR_NAME}" ]; then
    cp "../bigdata/hive/lib/${DRIVER_JAR_NAME}" "jdbc-drivers/${DRIVER_JAR_NAME}"
    log_ok "已从 bigdata 模块复制 JDBC 驱动"
else
    curl -fSL --retry 3 -o "jdbc-drivers/${DRIVER_JAR_NAME}" "$DRIVER_JAR_URL" ||
        die "JDBC 驱动下载失败，可将 ${DRIVER_JAR_NAME} 手动放入 jdbc-drivers/ 后重试"
    log_ok "JDBC 驱动下载完成"
fi

fe_ready() {
    curl -sf http://localhost:8030/api/bootstrap >/dev/null 2>&1
}

doris_mysql() {
    docker compose exec -T fe mysql -uroot -P9030 -h127.0.0.1 "$@"
}

# 兼容 root 密码已生效的重复执行场景：首次启动（密码未设置）无密码连接，否则带密码重试
doris_mysql_auth() {
    if doris_mysql "$@" 2>/dev/null; then
        return 0
    fi
    docker compose exec -T -e "MYSQL_PWD=${DORIS_ROOT_PASSWORD}" fe \
        mysql -uroot -P9030 -h127.0.0.1 "$@" 2>/dev/null
}

doris_sql_ready() {
    doris_mysql_auth -N -e "SELECT 1" >/dev/null 2>&1
}

doris_backends_ready() {
    local count
    count="$(doris_mysql_auth -N -e "SHOW BACKENDS" 2>/dev/null |
        awk -F'\t' '{for(i=1;i<=NF;i++) if($i=="true") c++} END{print c+0}')"
    [ "${count:-0}" -ge 2 ]
}

create_fed_catalog() {
    doris_mysql_auth -e "
        CREATE CATALOG IF NOT EXISTS mysql_fed PROPERTIES (
            \"type\" = \"jdbc\",
            \"user\" = \"root\",
            \"password\" = \"${MYSQL_ROOT_PASSWORD}\",
            \"jdbc_url\" = \"jdbc:mysql://mysql:3306/federation_demo?useSSL=false&serverTimezone=UTC&allowPublicKeyRetrieval=true\",
            \"driver_url\" = \"file:///opt/jdbc_drivers/${DRIVER_JAR_NAME}\",
            \"driver_class\" = \"com.mysql.cj.jdbc.Driver\"
        )" >/dev/null 2>&1
}

federation_query_ready() {
    doris_mysql_auth -N -e "SELECT user_id FROM mysql_fed.federation_demo.users WHERE user_id = 1001" \
        >/dev/null 2>&1
}

# bigdata 模块（Hive Metastore / HDFS）是否可达
bigdata_hms_reachable() {
    docker compose exec -T fe bash -c 'echo > /dev/tcp/hivemetastore/9083' >/dev/null 2>&1
}

create_lake_catalogs() {
    doris_mysql_auth -e "
        CREATE CATALOG IF NOT EXISTS hive_fed PROPERTIES (
            \"type\" = \"hms\",
            \"hive.metastore.uris\" = \"thrift://hivemetastore:9083\",
            \"hadoop.username\" = \"root\",
            \"fs.defaultFS\" = \"hdfs://namenode:9000\"
        );
        CREATE CATALOG IF NOT EXISTS paimon_fed PROPERTIES (
            \"type\" = \"paimon\",
            \"paimon.catalog.type\" = \"hms\",
            \"hive.metastore.uris\" = \"thrift://hivemetastore:9083\",
            \"warehouse\" = \"hdfs://namenode:9000/user/hive/warehouse/paimon\",
            \"hadoop.username\" = \"root\",
            \"fs.defaultFS\" = \"hdfs://namenode:9000\"
        )" >/dev/null 2>&1
}

lake_federation_ready() {
    doris_mysql_auth -N -e "SELECT COUNT(*) FROM hive_fed.demo.users" >/dev/null 2>&1 &&
        doris_mysql_auth -N -e "SELECT COUNT(*) FROM paimon_fed.demo.events" >/dev/null 2>&1
}

if [ "$START_SERVICES" = true ]; then
    echo "==> 准备跨模块联邦网络（供本集群访问 bigdata 的 Hive/HDFS）..."
    if docker network inspect "$FEDERATION_NETWORK" >/dev/null 2>&1; then
        log_skip "${FEDERATION_NETWORK} 已存在"
    else
        docker network create "$FEDERATION_NETWORK" >/dev/null
        log_ok "联邦网络 ${FEDERATION_NETWORK} 已创建"
    fi

    echo "==> 启动 Doris 集群（1 FE + 2 BE + MySQL 外部数据源）..."
    docker compose up -d
    wait_until "FE 就绪" 60 2 fe_ready || die "请查看日志: docker compose logs fe"
    wait_until "Doris SQL 接口就绪" 60 2 doris_sql_ready || die "请查看日志: docker compose logs fe"
    wait_until "2 个 BE 注册并存活" 60 2 doris_backends_ready || die "请查看日志: docker compose logs be1 be2"

    echo "==> 校验示例数据..."
    ROW_COUNT=""
    for i in $(seq 1 15); do
        ROW_COUNT="$(doris_mysql_auth -N -e "SELECT COUNT(*) FROM demo.orders" 2>/dev/null || true)"
        [ -n "${ROW_COUNT:-}" ] && break
        sleep 2
    done
    if [ -n "${ROW_COUNT:-}" ]; then
        log_ok "示例表 demo.orders 已就绪，共 ${ROW_COUNT} 行"
    else
        log_skip "示例表尚未生成，可能仍在初始化"
    fi

    echo "==> 配置外表 Catalog（内表 ⨝ 外表联邦查询）..."
    create_fed_catalog || die "创建 MySQL Catalog 失败，请检查 jdbc-drivers/ 驱动与 MySQL 容器状态"
    wait_until "外表 Catalog 就绪" 30 2 federation_query_ready ||
        die "外表 Catalog 查询不可用，请查看日志: docker compose logs be1 be2"

    USER_COUNT="$(doris_mysql_auth -N -e "SELECT COUNT(*) FROM mysql_fed.federation_demo.users")"
    log_ok "外表 mysql_fed.federation_demo.users 已就绪，共 ${USER_COUNT} 行"

    echo "==> 验证内外表联邦 JOIN 语义..."
    echo "  [--] 内表 demo.orders 与外表 users 数据"
    doris_mysql_auth -e "SELECT order_id, user_id, amount, status FROM demo.orders;
        SELECT user_id, user_name, city, vip_level FROM mysql_fed.federation_demo.users"

    JOIN_COUNT="$(doris_mysql_auth -N -e "SELECT COUNT(*) FROM demo.orders o
        JOIN mysql_fed.federation_demo.users u ON o.user_id = u.user_id")"
    [ "${JOIN_COUNT:-0}" -ge 1 ] || die "联邦 JOIN 结果为空，内外表数据关联异常"

    echo "  [--] 验证 LEFT JOIN 语义（内表数据为空时，外表记录仍可读取）"
    LEFT_ROWS="$(doris_mysql_auth -N -e "SELECT user_name FROM mysql_fed.federation_demo.users u
        LEFT JOIN demo.orders o ON u.user_id = o.user_id
        WHERE o.order_id IS NULL ORDER BY u.user_id")"
    if [ -n "${LEFT_ROWS:-}" ]; then
        log_ok "LEFT JOIN 语义验证通过（$JOIN_COUNT 条 INNER JOIN 记录）"
        echo "  [--] 仅外表侧存在的用户：$LEFT_ROWS"
    else
        log_ok "联邦 JOIN 验证通过（$JOIN_COUNT 条 INNER JOIN 记录）"
    fi

    echo "==> 配置湖仓外表 Catalog（Hive / Paimon 联邦查询，依赖 bigdata 模块）..."
    if bigdata_hms_reachable; then
        if create_lake_catalogs && wait_until "Hive/Paimon 外表就绪" 30 2 lake_federation_ready; then
            HIVE_USER_COUNT="$(doris_mysql_auth -N -e "SELECT COUNT(*) FROM hive_fed.demo.users")"
            PAIMON_EVENT_COUNT="$(doris_mysql_auth -N -e "SELECT COUNT(*) FROM paimon_fed.demo.events")"
            log_ok "外表 hive_fed.demo.users 已就绪，共 ${HIVE_USER_COUNT} 行"
            log_ok "外表 paimon_fed.demo.events 已就绪，共 ${PAIMON_EVENT_COUNT} 行"

            echo "  [--] 内表 ⨝ Hive 维度表联邦 JOIN"
            doris_mysql_auth -e "SELECT o.order_id, o.amount, u.user_name, u.vip_level
                FROM demo.orders o
                JOIN hive_fed.demo.users u ON o.user_id = u.user_id
                ORDER BY o.order_id"
            echo "  [--] Paimon 明细表直查"
            doris_mysql_auth -e "SELECT event_id, user_id, event_type, event_time
                FROM paimon_fed.demo.events ORDER BY event_id"
        else
            log_warn "湖仓外表暂不可用（bigdata 示例数据可能未初始化），可在两侧重跑 bootstrap.sh 补齐"
        fi
    else
        log_skip "bigdata 模块未启动（hivemetastore 不可达），跳过 Hive/Paimon 联邦配置"
        echo "  提示: 先执行 docker/bigdata/bootstrap.sh，再重跑本脚本即可打通湖仓联邦查询"
    fi

    echo "==> 设置 Doris root 密码..."
    doris_mysql_auth -e "SET PASSWORD FOR 'root'@'%' = PASSWORD('${DORIS_ROOT_PASSWORD}')"
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
    printf '\n==> 联邦查询示例:\n'
    cat <<'EOF'
  -- MySQL 外表（JDBC Catalog）
  SELECT * FROM mysql_fed.federation_demo.users;
  -- Hive 外表（HMS Catalog，数据在 HDFS 上）
  SELECT * FROM hive_fed.demo.users;
  -- Paimon 外表（湖仓格式，HMS 注册）
  SELECT * FROM paimon_fed.demo.events;
  -- 内表 ⨝ 湖仓外表联邦 JOIN
  SELECT o.order_id, o.amount, u.user_name, u.vip_level
  FROM demo.orders o
  JOIN hive_fed.demo.users u ON o.user_id = u.user_id;
EOF
    echo ""
    echo "完成！"
else
    print_no_start
fi
