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
# Created: 2026/08/17

# MiniSearch 评测脚本（DESIGN.md §11）
# 用法: bash eval/eval.sh [server_address]
# 流程: 灌入语料 → 逐条 golden query 检索 → 计算 Recall@5 / MRR@10 → 输出报告
#
# 语料的向量字段为 mode=server + 1024 维，与本地开源模型 BAAI/bge-m3 对齐。
# 推荐启动方式（全本地、零外部依赖，embedding 与 rerank 同进程）：
#   bazel run //cpp/pl/minisearch/embedding_server:embedding_server -- \
#       --model BAAI/bge-m3 --rerank-model BAAI/bge-reranker-v2-m3 --port 18000 &
#   minisearch_server --data_dir=<dir> \
#       --embedding_endpoint=http://127.0.0.1:18000 \
#       --embedding_model=bge-m3 \
#       --rerank_endpoint=http://127.0.0.1:18000 \
#       --rerank_model=bge-reranker-v2-m3

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EVAL_ROOT="${SCRIPT_DIR}"
SERVER="${1:-http://127.0.0.1:8200}"
TOP_K="${EVAL_TOP_K:-5}"

# --- 参数 ---
CORPUS="${EVAL_ROOT}/corpus/loom-kb.json"
GOLDEN="${EVAL_ROOT}/golden/loom-kb.jsonl"

if [ ! -f "${CORPUS}" ] || [ ! -f "${GOLDEN}" ]; then
    echo "ERROR: corpus or golden set not found under ${EVAL_ROOT}" >&2
    exit 1
fi

# --- 提取 collection 名 ---
COLLECTION=$(python3 -c "import json,sys; print(json.load(open('${CORPUS}'))['name'])")

# --- 灌入语料（drop + create + bulk upsert）---
echo "==> loading corpus '${COLLECTION}' into ${SERVER}"
curl -s -X DELETE "${SERVER}/api/v2/collections/${COLLECTION}?confirm=${COLLECTION}" \
    -o /dev/null 2>/dev/null || true

SPEC=$(python3 -c "
import json
d = json.load(open('${CORPUS}'))
spec = {'name': d['name'], 'default_analyzer': d.get('default_analyzer',''), 'fields': d['fields']}
print(json.dumps(spec, ensure_ascii=False))
")
RESP=$(curl -s -X POST "${SERVER}/api/v2/collections" \
    -H 'Content-Type: application/json' -d "${SPEC}" -w '\n%{http_code}')
CODE=$(echo "${RESP}" | tail -1)
BODY=$(echo "${RESP}" | sed '$d')
if [ "${CODE}" != "200" ]; then
    echo "ERROR: create collection failed (HTTP ${CODE}): ${BODY}" >&2
    exit 1
fi

python3 - "${SERVER}" "${COLLECTION}" "${CORPUS}" <<'PYEOF'
import json, sys, urllib.request

server, collection, corpus_path = sys.argv[1], sys.argv[2], sys.argv[3]
data = json.load(open(corpus_path))
for doc in data["documents"]:
    body = json.dumps({"version": 1, "fields": doc["fields"]}, ensure_ascii=False).encode()
    req = urllib.request.Request(
        f"{server}/api/v2/{collection}/documents/{doc['id']}",
        data=body, method="PUT", headers={"Content-Type": "application/json"})
    resp = urllib.request.urlopen(req)
    result = json.loads(resp.read())
    if not result.get("ok"):
        print(f"ERROR: upsert {doc['id']} failed: {result}", file=sys.stderr)
        sys.exit(1)
print(f"    {len(data['documents'])} documents loaded")
PYEOF

# --- 逐条评测 ---
echo "==> evaluating $(wc -l <"${GOLDEN}" | tr -d ' ') golden queries (top_k=${TOP_K})"
echo

python3 - "${SERVER}" "${COLLECTION}" "${GOLDEN}" "${TOP_K}" <<'PYEOF'
import json, sys, urllib.request

server, collection, golden_path, top_k = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])

hits_per_query = []
with open(golden_path) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        case = json.loads(line)
        query = case["query"]
        expect = case["expect"]

        body = json.dumps({"text": query, "top_k": top_k}, ensure_ascii=False).encode()
        req = urllib.request.Request(
            f"{server}/api/v2/{collection}/search",
            data=body, headers={"Content-Type": "application/json"})
        resp = json.loads(urllib.request.urlopen(req).read())
        got = [h["id"] for h in resp.get("hits", [])]

        hits_per_query.append({"query": query, "expect": expect, "got": got})

# Recall@K
recall_hits = 0
recall_total = 0
for h in hits_per_query:
    found = sum(1 for e in h["expect"] if e in h["got"])
    recall_hits += found
    recall_total += len(h["expect"])
recall = recall_hits / recall_total if recall_total else 0.0

# MRR@10
mrr_sum = 0.0
for h in hits_per_query:
    rank = None
    for i, doc_id in enumerate(h["got"][:10]):
        if doc_id in h["expect"]:
            rank = i + 1
            break
    if rank:
        mrr_sum += 1.0 / rank
mrr = mrr_sum / len(hits_per_query) if hits_per_query else 0.0

# per-query report
print(f"{'query':<30} {'expect':<30} {'got':<30} status")
print("-" * 95)
for h in hits_per_query:
    ok = any(e in h["got"] for e in h["expect"])
    status = "HIT " if ok else "MISS"
    print(f"{h['query'][:28]:<30} {','.join(h['expect'])[:28]:<30} {','.join(h['got'])[:28]:<30} {status}")

print()
print(f"Recall@{top_k}: {recall:.4f}  ({recall_hits}/{recall_total})")
print(f"MRR@10:      {mrr:.4f}")
PYEOF
