#!/usr/bin/env bash
# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Authors: liubang (it.liubang@gmail.com)
# Created: 2026/05/14 10:46

# MiniSearch Server — v2 API 端到端测试
# 阶段 1: 内存模式功能回归（collections/documents/search/drop）
# 阶段 2: 认证与多租户（401/403/白名单/吊销/bootstrap/fail-closed）
# 阶段 3: 持久化重启（checkpoint 恢复 + 倒排重建）

set -u

BIN=bazel-bin/cpp/pl/minisearch/minisearch_server
PASS=0
FAIL=0
SERVER_PIDS=()
DATA_DIRS=()

cleanup() {
    for pid in "${SERVER_PIDS[@]:-}"; do
        kill "${pid}" 2>/dev/null
        wait "${pid}" 2>/dev/null
    done
    for dir in "${DATA_DIRS[@]:-}"; do
        rm -rf "${dir}"
    done
}
trap cleanup EXIT

check() { # check <name> <expected_status> <actual_status> <body> [body_contains]
    local name="$1" expected="$2" actual="$3" body="$4" contains="${5:-}"
    if [ "$expected" = "$actual" ] && {
        [ -z "$contains" ] || echo "$body" | grep -qF "$contains"
    }; then
        echo "PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "FAIL: ${name} (expected=${expected} actual=${actual} body=${body})"
        FAIL=$((FAIL + 1))
    fi
}

check_absent() { # check_absent <name> <expected_status> <actual_status> <body> <body_must_not_contain>
    local name="$1" expected="$2" actual="$3" body="$4" absent="$5"
    if [ "$expected" = "$actual" ] && ! echo "$body" | grep -qF "$absent"; then
        echo "PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "FAIL: ${name} (expected=${expected} actual=${actual} body=${body})"
        FAIL=$((FAIL + 1))
    fi
}

status_of() { # status_of <curl args...> ; prints "<status> <body>"
    local out
    out=$(curl -s -w '\n%{http_code}' "$@")
    local code="${out##*$'\n'}"
    local body="${out%$'\n'*}"
    echo "${code} ${body}"
}

json_field() { # json_field <field> — stdin 中提取 "field":"value"
    sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p"
}

start_server() { # start_server <port> [extra args...] ; waits for healthz
    local port="$1"
    shift
    "${BIN}" --port="${port}" "$@" &
    SERVER_PIDS+=($!)
    for _ in $(seq 1 50); do
        if curl -sf "http://127.0.0.1:${port}/healthz" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    echo "server failed to start on :${port}"
    exit 1
}

stop_last_server() {
    local last=$((${#SERVER_PIDS[@]} - 1))
    local pid="${SERVER_PIDS[${last}]}"
    kill "${pid}" 2>/dev/null
    wait "${pid}" 2>/dev/null
    unset "SERVER_PIDS[${last}]"
}

cd "$(dirname "$0")/../../.." || exit 1

echo "==> building minisearch_server"
bazel build //cpp/pl/minisearch:minisearch_server || exit 1

# ======================================================================
# 阶段 1: 内存模式功能回归
# ======================================================================
PORT=18200
BASE="http://127.0.0.1:${PORT}"

echo "==> phase 1: in-memory functional regression on :${PORT}"
start_server "${PORT}"

SCHEMA='{"name":"kb","default_analyzer":"cjk_jieba","fields":[{"name":"title","type":"text","indexed":true,"stored":true},{"name":"tags","type":"keyword","indexed":true,"stored":true},{"name":"vec","type":"vector","indexed":false,"stored":true,"dims":4,"metric":"cosine","mode":"client"}]}'
DOC1='{"version":1,"fields":{"title":{"s":"presto 调优"},"tags":{"s":"wiki"},"vec":{"v":{"data":[1.0,0.0,0.0,0.0]}}}}'
DOC2='{"version":1,"fields":{"title":{"s":"loom 架构"},"tags":{"s":"wiki"},"vec":{"v":{"data":[0.0,1.0,0.0,0.0]}}}}'

# --- collections ---
R=$(status_of -X POST "${BASE}/api/v2/collections" -H 'Content-Type: application/json' -d "${SCHEMA}")
check "create collection" 200 "${R%% *}" "${R#* }" '"ok":true'

R=$(status_of -X POST "${BASE}/api/v2/collections" -H 'Content-Type: application/json' -d "${SCHEMA}")
check "duplicate collection -> 409" 409 "${R%% *}" "${R#* }"

R=$(status_of "${BASE}/api/v2/collections")
check "list collections" 200 "${R%% *}" "${R#* }" '"kb"'

R=$(status_of -X POST "${BASE}/api/v2/collections" -H 'Content-Type: application/json' \
    -d '{"name":"bad","fields":[{"name":"v","type":"vector","dims":0,"mode":"client"}]}')
check "invalid schema -> 400" 400 "${R%% *}" "${R#* }"

R=$(status_of -X POST "${BASE}/api/v2/collections" -H 'Content-Type: application/json' \
    -d '{"name":"bad/name","fields":[{"name":"t","type":"text"}]}')
check "invalid collection name -> 400" 400 "${R%% *}" "${R#* }"

# --- documents ---
R=$(status_of -X PUT "${BASE}/api/v2/kb/documents/doc1" -H 'Content-Type: application/json' -d "${DOC1}")
check "upsert doc1" 200 "${R%% *}" "${R#* }" '"ok":true'

R=$(status_of -X PUT "${BASE}/api/v2/kb/documents/doc2" -H 'Content-Type: application/json' -d "${DOC2}")
check "upsert doc2" 200 "${R%% *}" "${R#* }" '"ok":true'

R=$(status_of -X PUT "${BASE}/api/v2/kb/documents/doc1" -H 'Content-Type: application/json' -d "${DOC1}")
check "stale version -> 409" 409 "${R%% *}" "${R#* }" 'stale'

R=$(status_of "${BASE}/api/v2/kb/documents/doc1")
check "get doc1" 200 "${R%% *}" "${R#* }" 'presto'

R=$(status_of "${BASE}/api/v2/kb/documents/missing")
check "get missing -> 404" 404 "${R%% *}" "${R#* }"

R=$(status_of "${BASE}/api/v2/missing/documents/x")
check "unknown collection -> 404" 404 "${R%% *}" "${R#* }"

# 文档 id 允许包含 '/'
R=$(status_of -X PUT "${BASE}/api/v2/kb/documents/docs/a.md" -H 'Content-Type: application/json' -d "${DOC1}")
check "upsert doc with slash id" 200 "${R%% *}" "${R#* }" '"ok":true'
R=$(status_of "${BASE}/api/v2/kb/documents/docs/a.md")
check "get doc with slash id" 200 "${R%% *}" "${R#* }" 'docs/a.md'
R=$(status_of -X DELETE "${BASE}/api/v2/kb/documents/docs/a.md")
check "delete doc with slash id" 200 "${R%% *}" "${R#* }" '"ok":true'

# --- search ---
R=$(status_of -X POST "${BASE}/api/v2/kb/search" -H 'Content-Type: application/json' \
    -d '{"embedding":[0.9,0.1,0.0,0.0],"top_k":1}')
check "search nearest" 200 "${R%% *}" "${R#* }" 'doc1'

R=$(status_of -X POST "${BASE}/api/v2/kb/search" -H 'Content-Type: application/json' -d '{"text":"调优"}')
check "text query degrades to bm25 (no embedding service)" 200 "${R%% *}" "${R#* }" 'doc1'
check "degraded marker present" 200 "${R%% *}" "${R#* }" '"degraded":["vector"]'

R=$(status_of -X POST "${BASE}/api/v2/kb/search" -H 'Content-Type: application/json' \
    -d '{"text":"调优","filter":{"and":[{"field":"tags","op":"=","values":[{"s":"other"}]}]}}')
check_absent "filtered search excludes doc1" 200 "${R%% *}" "${R#* }" 'doc1'

R=$(status_of -X POST "${BASE}/api/v2/kb/search" -H 'Content-Type: application/json' \
    -d '{"embedding":[1.0,0.0],"top_k":1}')
check "query dims mismatch -> 400" 400 "${R%% *}" "${R#* }"

# --- delete ---
R=$(status_of -X DELETE "${BASE}/api/v2/kb/documents/doc2")
check "delete doc2" 200 "${R%% *}" "${R#* }" '"ok":true'

R=$(status_of -X POST "${BASE}/api/v2/kb/search" -H 'Content-Type: application/json' \
    -d '{"embedding":[0.0,1.0,0.0,0.0],"top_k":5}')
check_absent "deleted doc drops from search" 200 "${R%% *}" "${R#* }" 'doc2'

# --- drop ---
R=$(status_of -X DELETE "${BASE}/api/v2/collections/kb")
check "drop without confirm -> 400" 400 "${R%% *}" "${R#* }"

R=$(status_of -X DELETE "${BASE}/api/v2/collections/kb?confirm=kb")
check "drop collection" 200 "${R%% *}" "${R#* }" '"ok":true'

R=$(status_of "${BASE}/api/v2/collections")
check_absent "list after drop" 200 "${R%% *}" "${R#* }" 'kb'

stop_last_server

# ======================================================================
# 阶段 2: 认证与多租户
# ======================================================================
AUTH_PORT=18201
AUTH_BASE="http://127.0.0.1:${AUTH_PORT}"
AUTH_DIR=$(mktemp -d /tmp/minisearch_auth_test_XXXXXX)
DATA_DIRS+=("${AUTH_DIR}")

echo "==> phase 2: auth & tenancy on :${AUTH_PORT}"
start_server "${AUTH_PORT}" --data_dir="${AUTH_DIR}" --auth=true

# fail-closed：非回环监听 + auth=off 必须拒绝启动（进程应立即退出）
"${BIN}" --port=18299 --listen=0.0.0.0 >/dev/null 2>&1 &
REFUSE_PID=$!
sleep 2
if kill -0 ${REFUSE_PID} 2>/dev/null; then
    echo "FAIL: non-loopback without auth refused (fail-closed)"
    FAIL=$((FAIL + 1))
    kill ${REFUSE_PID} 2>/dev/null
    wait ${REFUSE_PID} 2>/dev/null
else
    echo "PASS: non-loopback without auth refused (fail-closed)"
    PASS=$((PASS + 1))
    wait ${REFUSE_PID} 2>/dev/null
fi

# bootstrap admin key
if [ -f "${AUTH_DIR}/bootstrap.key" ]; then
    echo "PASS: bootstrap key file created"
    PASS=$((PASS + 1))
else
    echo "FAIL: bootstrap key file created"
    FAIL=$((FAIL + 1))
fi
ADMIN_KEY=$(tr -d '\n' <"${AUTH_DIR}/bootstrap.key")

R=$(status_of "${AUTH_BASE}/api/v2/collections")
check "no token -> 401" 401 "${R%% *}" "${R#* }"

R=$(status_of "${AUTH_BASE}/api/v2/collections" -H "Authorization: Bearer msk_bogus")
check "bogus token -> 401" 401 "${R%% *}" "${R#* }"

R=$(status_of "${AUTH_BASE}/healthz")
check "healthz unauthenticated" 200 "${R%% *}" "${R#* }" 'ok'

R=$(status_of "${AUTH_BASE}/api/v2/admin/tenants" -H "Authorization: Bearer ${ADMIN_KEY}")
check "admin list tenants" 200 "${R%% *}" "${R#* }"

# admin 在 team-a 建 kb / other
R=$(status_of -X POST "${AUTH_BASE}/api/v2/collections?tenant=team-a" \
    -H "Authorization: Bearer ${ADMIN_KEY}" -H 'Content-Type: application/json' -d "${SCHEMA}")
check "admin create kb in team-a" 200 "${R%% *}" "${R#* }" '"ok":true'
R=$(status_of -X POST "${AUTH_BASE}/api/v2/collections?tenant=team-a" \
    -H "Authorization: Bearer ${ADMIN_KEY}" -H 'Content-Type: application/json' \
    -d '{"name":"other","fields":[{"name":"t","type":"text"}]}')
check "admin create other in team-a" 200 "${R%% *}" "${R#* }" '"ok":true'

# 签发 writer（白名单 kb）
R=$(status_of -X POST "${AUTH_BASE}/api/v2/admin/tenants/team-a/keys" \
    -H "Authorization: Bearer ${ADMIN_KEY}" -H 'Content-Type: application/json' \
    -d '{"role":"writer","collections":["kb"]}')
check "issue writer key" 200 "${R%% *}" "${R#* }" '"key":"msk_'
WRITER_KEY=$(echo "${R#* }" | json_field key)
WRITER_KEY_ID=$(echo "${R#* }" | json_field key_id)

R=$(status_of -X PUT "${AUTH_BASE}/api/v2/kb/documents/doc1" \
    -H "Authorization: Bearer ${WRITER_KEY}" -H 'Content-Type: application/json' -d "${DOC1}")
check "writer upsert in whitelist" 200 "${R%% *}" "${R#* }" '"ok":true'

R=$(status_of -X PUT "${AUTH_BASE}/api/v2/other/documents/doc1" \
    -H "Authorization: Bearer ${WRITER_KEY}" -H 'Content-Type: application/json' -d "${DOC1}")
check "writer upsert outside whitelist -> 403" 403 "${R%% *}" "${R#* }"

R=$(status_of "${AUTH_BASE}/api/v2/other/documents/doc1" -H "Authorization: Bearer ${WRITER_KEY}")
check "writer read outside whitelist -> 403" 403 "${R%% *}" "${R#* }"

R=$(status_of -X POST "${AUTH_BASE}/api/v2/collections" \
    -H "Authorization: Bearer ${WRITER_KEY}" -H 'Content-Type: application/json' -d "${SCHEMA}")
check "writer create collection -> 403" 403 "${R%% *}" "${R#* }"

# 租户隔离：default 命名空间看不到 team-a 的 collection
R=$(status_of "${AUTH_BASE}/api/v2/collections" -H "Authorization: Bearer ${ADMIN_KEY}")
check_absent "tenant isolation" 200 "${R%% *}" "${R#* }" 'kb'

# tenant_admin 不能签发 tenant_admin
R=$(status_of -X POST "${AUTH_BASE}/api/v2/admin/tenants/team-a/keys" \
    -H "Authorization: Bearer ${ADMIN_KEY}" -H 'Content-Type: application/json' \
    -d '{"role":"tenant_admin"}')
TA_KEY=$(echo "${R#* }" | json_field key)
R=$(status_of -X POST "${AUTH_BASE}/api/v2/admin/tenants/team-a/keys" \
    -H "Authorization: Bearer ${TA_KEY}" -H 'Content-Type: application/json' \
    -d '{"role":"tenant_admin"}')
check "tenant_admin issue tenant_admin -> 400" 400 "${R%% *}" "${R#* }"
R=$(status_of -X POST "${AUTH_BASE}/api/v2/admin/tenants/team-a/keys" \
    -H "Authorization: Bearer ${TA_KEY}" -H 'Content-Type: application/json' \
    -d '{"role":"reader"}')
check "tenant_admin issue reader" 200 "${R%% *}" "${R#* }" '"key":"msk_'

# 非法租户名
R=$(status_of -X POST "${AUTH_BASE}/api/v2/admin/tenants/bad..name/keys" \
    -H "Authorization: Bearer ${ADMIN_KEY}" -H 'Content-Type: application/json' \
    -d '{"role":"reader"}')
check "invalid tenant name -> 400" 400 "${R%% *}" "${R#* }"

# stats
R=$(status_of "${AUTH_BASE}/api/v2/admin/stats" -H "Authorization: Bearer ${ADMIN_KEY}")
check "admin stats" 200 "${R%% *}" "${R#* }" '"total_collections":2'
R=$(status_of "${AUTH_BASE}/api/v2/admin/stats" -H "Authorization: Bearer ${WRITER_KEY}")
check "stats non-admin -> 403" 403 "${R%% *}" "${R#* }"

# 吊销立即生效
R=$(status_of -X DELETE "${AUTH_BASE}/api/v2/admin/tenants/team-a/keys/${WRITER_KEY_ID}" \
    -H "Authorization: Bearer ${ADMIN_KEY}")
check "revoke writer key" 200 "${R%% *}" "${R#* }" '"ok":true'
R=$(status_of "${AUTH_BASE}/api/v2/collections" -H "Authorization: Bearer ${WRITER_KEY}")
check "revoked key -> 401" 401 "${R%% *}" "${R#* }"

stop_last_server

# ======================================================================
# 阶段 3: 持久化重启（checkpoint 恢复 + 倒排重建）
# ======================================================================
PERSIST_PORT=18202
PERSIST_BASE="http://127.0.0.1:${PERSIST_PORT}"
PERSIST_DIR=$(mktemp -d /tmp/minisearch_persist_test_XXXXXX)
DATA_DIRS+=("${PERSIST_DIR}")

echo "==> phase 3: persistence & restart on :${PERSIST_PORT}"
start_server "${PERSIST_PORT}" --data_dir="${PERSIST_DIR}"

R=$(status_of -X POST "${PERSIST_BASE}/api/v2/collections" -H 'Content-Type: application/json' -d "${SCHEMA}")
check "persist: create collection" 200 "${R%% *}" "${R#* }" '"ok":true'
R=$(status_of -X PUT "${PERSIST_BASE}/api/v2/kb/documents/doc1" -H 'Content-Type: application/json' -d "${DOC1}")
check "persist: upsert doc1" 200 "${R%% *}" "${R#* }" '"ok":true'
R=$(status_of -X PUT "${PERSIST_BASE}/api/v2/kb/documents/doc2" -H 'Content-Type: application/json' -d "${DOC2}")
check "persist: upsert doc2" 200 "${R%% *}" "${R#* }" '"ok":true'
R=$(status_of -X DELETE "${PERSIST_BASE}/api/v2/kb/documents/doc2")
check "persist: delete doc2 (tombstone)" 200 "${R%% *}" "${R#* }" '"ok":true'

# 停服 -> 触发退出前 final flush checkpoint -> 重启
stop_last_server
start_server "${PERSIST_PORT}" --data_dir="${PERSIST_DIR}"

R=$(status_of "${PERSIST_BASE}/api/v2/collections")
check "restart: collection restored" 200 "${R%% *}" "${R#* }" '"active_documents":1'

R=$(status_of "${PERSIST_BASE}/api/v2/kb/documents/doc1")
check "restart: doc1 survives" 200 "${R%% *}" "${R#* }" 'presto'

R=$(status_of "${PERSIST_BASE}/api/v2/kb/documents/doc2")
check "restart: deleted doc2 stays gone" 404 "${R%% *}" "${R#* }"

# 倒排索引重启后重建：BM25 文本检索立即可用
R=$(status_of -X POST "${PERSIST_BASE}/api/v2/kb/search" -H 'Content-Type: application/json' \
    -d '{"text":"调优","top_k":5}')
check "restart: bm25 works (inverted rebuilt)" 200 "${R%% *}" "${R#* }" 'doc1'

# 向量索引从 checkpoint 恢复
R=$(status_of -X POST "${PERSIST_BASE}/api/v2/kb/search" -H 'Content-Type: application/json' \
    -d '{"embedding":[0.9,0.1,0.0,0.0],"top_k":1}')
check "restart: vector search works" 200 "${R%% *}" "${R#* }" 'doc1'

# 恢复后 docid 不复用
R=$(status_of -X PUT "${PERSIST_BASE}/api/v2/kb/documents/doc3" -H 'Content-Type: application/json' \
    -d '{"version":1,"fields":{"title":{"s":"新文档"},"tags":{"s":"wiki"},"vec":{"v":{"data":[0.0,0.0,1.0,0.0]}}}}')
check "restart: new upsert ok" 200 "${R%% *}" "${R#* }" '"ok":true'

stop_last_server

echo
echo "==================================="
echo "PASS: ${PASS}  FAIL: ${FAIL}"
echo "==================================="

[ "${FAIL}" = "0" ]
