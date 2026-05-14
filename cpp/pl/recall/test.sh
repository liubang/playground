#!/usr/bin/env bash
# Vector Recall Service — 端到端测试脚本
# 用法: bash cpp/pl/recall/test.sh
#
# 流程: 编译 → 启动 server → curl 打全部 API → 关闭 server → 汇总结果

set -uo pipefail

PORT=18200
DIM=4
SERVER="http://127.0.0.1:${PORT}"
SNAPSHOT_DIR="/tmp/recall_test_snapshot_$$"
PASS=0
FAIL=0
SERVER_PID=""

# ── 颜色 ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

cleanup() {
    if [[ -n "${SERVER_PID}" ]]; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${SNAPSHOT_DIR}"
}
trap cleanup EXIT

check() {
    local name="$1" response="$2" expected="$3"
    if echo "${response}" | grep -q "${expected}"; then
        printf "${GREEN}✓ PASS${NC}  %s\n" "${name}"
        PASS=$((PASS + 1))
    else
        printf "${RED}✗ FAIL${NC}  %s\n" "${name}"
        printf "  expected to contain: %s\n" "${expected}"
        printf "  got: %s\n" "${response}"
        FAIL=$((FAIL + 1))
    fi
}

# ── 1. 编译 ──
echo "━━━ Building ━━━"
bazel build //cpp/pl/recall:recall_server //cpp/pl/recall:recall_client 2>&1 | tail -3

# ── 2. 启动 server ──
echo ""
echo "━━━ Starting server (port=${PORT}, dim=${DIM}) ━━━"
bazel-bin/cpp/pl/recall/recall_server \
    --port="${PORT}" --dimension="${DIM}" --index_type=Flat >/dev/null 2>&1 &
SERVER_PID=$!

# 等待 server 就绪
for i in $(seq 1 30); do
    if curl -s "${SERVER}/api/recall/stats" >/dev/null 2>&1; then
        echo "Server ready (pid=${SERVER_PID})"
        break
    fi
    if [[ $i -eq 30 ]]; then
        echo "Server failed to start"
        exit 1
    fi
    sleep 0.2
done

echo ""
echo "━━━ Running API Tests ━━━"
echo ""

# ── 3. Add ──
RESP=$(curl -s -X POST "${SERVER}/api/recall/add" \
    -H 'Content-Type: application/json' \
    -d '{
        "table_id": "default.user_info",
        "embedding": [0.1, 0.2, 0.3, 0.4],
        "meta": {
            "database": "default",
            "table": "user_info",
            "comment": "用户基本信息表"
        }
    }')
check "POST /api/recall/add" "${RESP}" '"success":true'

# ── 4. Add 第二条 ──
RESP=$(curl -s -X POST "${SERVER}/api/recall/add" \
    -H 'Content-Type: application/json' \
    -d '{
        "table_id": "default.order_detail",
        "embedding": [0.9, 0.8, 0.7, 0.6],
        "meta": {
            "database": "default",
            "table": "order_detail",
            "comment": "订单明细表"
        }
    }')
check "POST /api/recall/add (2nd)" "${RESP}" '"success":true'

# ── 5. Add 维度不匹配 → 应返回 400 ──
RESP=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${SERVER}/api/recall/add" \
    -H 'Content-Type: application/json' \
    -d '{
        "table_id": "bad.dim",
        "embedding": [0.1, 0.2]
    }')
check "POST /api/recall/add (dim mismatch → 400)" "${RESP}" "400"

# ── 6. BatchAdd ──
RESP=$(curl -s -X POST "${SERVER}/api/recall/batch_add" \
    -H 'Content-Type: application/json' \
    -d '{
        "items": [
            {
                "table_id": "analytics.daily_report",
                "embedding": [0.5, 0.5, 0.5, 0.5],
                "meta": {"database":"analytics","table":"daily_report","comment":"日报"}
            },
            {
                "table_id": "analytics.weekly_summary",
                "embedding": [0.3, 0.3, 0.7, 0.7],
                "meta": {"database":"analytics","table":"weekly_summary","comment":"周报"}
            }
        ]
    }')
check "POST /api/recall/batch_add" "${RESP}" '"success_count":2'

# ── 7. Stats ──
RESP=$(curl -s "${SERVER}/api/recall/stats")
check "GET  /api/recall/stats (total_vectors)" "${RESP}" '"total_vectors":4'
check "GET  /api/recall/stats (dimension)"    "${RESP}" '"dimension":4'
check "GET  /api/recall/stats (is_trained)"    "${RESP}" '"is_trained":true'

# ── 8. Search ──
RESP=$(curl -s -X POST "${SERVER}/api/recall/search" \
    -H 'Content-Type: application/json' \
    -d '{
        "embedding": [0.1, 0.2, 0.3, 0.4],
        "top_k": 2
    }')
check "POST /api/recall/search (has results)" "${RESP}" '"table_id"'
check "POST /api/recall/search (top result)"  "${RESP}" 'user_info'

# ── 9. Search 维度不匹配 → 400 ──
RESP=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${SERVER}/api/recall/search" \
    -H 'Content-Type: application/json' \
    -d '{"embedding": [0.1], "top_k": 2}')
check "POST /api/recall/search (dim mismatch → 400)" "${RESP}" "400"

# ── 10. SaveSnapshot ──
mkdir -p "${SNAPSHOT_DIR}"
RESP=$(curl -s -X POST "${SERVER}/api/recall/snapshot/save" \
    -H 'Content-Type: application/json' \
    -d "{\"path\": \"${SNAPSHOT_DIR}\"}")
check "POST /api/recall/snapshot/save" "${RESP}" '"success":true'

# 验证快照文件存在
if [[ -f "${SNAPSHOT_DIR}/faiss.index" && -f "${SNAPSHOT_DIR}/id_mapper.bin" ]]; then
    printf "${GREEN}✓ PASS${NC}  Snapshot files exist on disk\n"
    PASS=$((PASS + 1))
else
    printf "${RED}✗ FAIL${NC}  Snapshot files missing\n"
    FAIL=$((FAIL + 1))
fi

# ── 11. LoadSnapshot ──
RESP=$(curl -s -X POST "${SERVER}/api/recall/snapshot/load" \
    -H 'Content-Type: application/json' \
    -d "{\"path\": \"${SNAPSHOT_DIR}\"}")
check "POST /api/recall/snapshot/load" "${RESP}" '"success":true'

# ── 12. 404 ──
RESP=$(curl -s -o /dev/null -w "%{http_code}" "${SERVER}/api/recall/nonexistent")
check "GET  /api/recall/nonexistent (→ 404)" "${RESP}" "404"

# ── 13. 无效 JSON ──
RESP=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${SERVER}/api/recall/add" \
    -H 'Content-Type: application/json' \
    -d 'not json at all')
check "POST /api/recall/add (bad JSON → 400)" "${RESP}" "400"

# ── 14. *_by_text 接口（未配置 embedding → 503）──
echo ""
echo "━━━ Text-based API Tests (no embedding configured → 503) ━━━"
echo ""

RESP=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${SERVER}/api/recall/add_by_text" \
    -H 'Content-Type: application/json' \
    -d '{"table_id":"test.t1","text":"user info table"}')
check "POST /api/recall/add_by_text (no embedding → 503)" "${RESP}" "503"

RESP=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${SERVER}/api/recall/batch_add_by_text" \
    -H 'Content-Type: application/json' \
    -d '{"items":[{"table_id":"test.t1","text":"user info"}]}')
check "POST /api/recall/batch_add_by_text (no embedding → 503)" "${RESP}" "503"

RESP=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${SERVER}/api/recall/search_by_text" \
    -H 'Content-Type: application/json' \
    -d '{"text":"find user table","top_k":3}')
check "POST /api/recall/search_by_text (no embedding → 503)" "${RESP}" "503"

# 验证 503 响应体包含有意义的错误信息
RESP=$(curl -s -X POST "${SERVER}/api/recall/search_by_text" \
    -H 'Content-Type: application/json' \
    -d '{"text":"find user table","top_k":3}')
check "POST /api/recall/search_by_text (error message)" "${RESP}" 'embedding service not configured'

# ── 汇总 ──
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
TOTAL=$((PASS + FAIL))
printf "Total: %d  ${GREEN}Passed: %d${NC}  ${RED}Failed: %d${NC}\n" "${TOTAL}" "${PASS}" "${FAIL}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [[ ${FAIL} -gt 0 ]]; then
    exit 1
fi
