#!/usr/bin/env bash
# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.

# Real-model golden regression (correctness + perf) for mllm with the
# PaddleOCR-VL-1.6 GGUF checkpoint (LM + ViT mmproj), complementing
# parity_vs_llamacpp.sh with multimodal coverage.
#
# Correctness is asserted in two layers per case:
#   1. golden freeze — stdout SHA-256 must equal the hash recorded below
#      (greedy decoding, Metal backend, Apple M4 Pro, --config=release).
#      Operator-level parity tests cannot catch end-to-end regressions
#      (e.g. KV indexing, RoPE deltas), so the frozen output is the anchor.
#   2. semantics — the OCR cases must transcribe the known text that
#      gen_doc_images.py renders into the test images. This is an absolute
#      answer check, not just backend-vs-backend agreement.
#
# Perf: timings are parsed from the CLI perf footer and printed as a
# markdown table (append-ready for bench/PERF_BASELINE.md; also written to
# $PERF_MD when that variable points at a file).
#
# Requirements (the test SKIPS with exit 0 when absent):
#   - mllm_cli (arg $1, or ../cli/mllm_cli relative to this script)
#   - python3 + Pillow, and the image generator (arg $2, auto-detected in
#     the repo layout / bazel runfiles otherwise)
#   - model files under $MLLM_MODELS_DIR (default /tmp/mllm_models):
#       paddleocr-vl-1.6/PaddleOCR-VL-1.6-GGUF.gguf
#       paddleocr-vl-1.6/PaddleOCR-VL-1.6-GGUF-mmproj.gguf
#
# Usage:
#   bazel test //cpp/pl/mllm/e2e:e2e_golden_ocr --config=release --test_output=all
#   BACKEND=cpu ./golden_ocr_regression.sh <path-to-mllm_cli>

set -uo pipefail

BACKEND="${BACKEND:-metal}"

MLLM_CLI="${1:-}"
if [[ -z "$MLLM_CLI" ]]; then
    MLLM_CLI="$(dirname "$0")/../cli/mllm_cli"
fi
if [[ ! -x "$MLLM_CLI" ]]; then
    echo "SKIP: mllm_cli not found at $MLLM_CLI"
    exit 0
fi

GEN_SCRIPT="${2:-}"
if [[ -z "$GEN_SCRIPT" ]]; then
    for cand in cpp/pl/mllm/bench/testdata/gen_doc_images.py \
        "$(dirname "$0")/../bench/testdata/gen_doc_images.py"; do
        if [[ -f "$cand" ]]; then
            GEN_SCRIPT="$cand"
            break
        fi
    done
fi
if [[ -z "$GEN_SCRIPT" || ! -f "$GEN_SCRIPT" ]]; then
    echo "SKIP: gen_doc_images.py not found"
    exit 0
fi
if ! python3 -c "import PIL" 2>/dev/null; then
    echo "SKIP: python3 + Pillow not available"
    exit 0
fi

SHA256=""
if command -v shasum >/dev/null; then
    SHA256="shasum -a 256"
elif command -v sha256sum >/dev/null; then
    SHA256="sha256sum"
else
    echo "SKIP: no sha256 tool found"
    exit 0
fi

MODELS_DIR="${MLLM_MODELS_DIR:-/tmp/mllm_models}"
M="$MODELS_DIR/paddleocr-vl-1.6/PaddleOCR-VL-1.6-GGUF.gguf"
MM="$MODELS_DIR/paddleocr-vl-1.6/PaddleOCR-VL-1.6-GGUF-mmproj.gguf"
if [[ ! -f "$M" || ! -f "$MM" ]]; then
    echo "SKIP: PaddleOCR-VL-1.6 GGUF weights not found under $MODELS_DIR"
    exit 0
fi

# --- Frozen goldens (greedy, Metal, recorded 2026-09-12 on commit 979aa3d32)
#
# text_short is the exception called out in bench/PERF_BASELINE.md: the
# Metal f16 GEMM path drifts from the CPU fp32 path and the outputs fork
# after ~24 tokens, so its hash is only enforced on Metal. The other cases
# are byte-identical across CPU and Metal and their hashes hold on both.
GOLDEN_TEXT_SHORT="236fbe40911f8449d294cd1d25e949da4fe99c900eae124822ff9245cd555cbe"
GOLDEN_TEXT_LONG="2692320628d830b856fca2d525d1aad05a60031fc8ddb1b57cc4548c2b02f6ed"
GOLDEN_OCR_SMALL="c79fca6c6e1c30ee4a288caa1bdc7150e8755bf69b841717da525ef39c66bb0d"
GOLDEN_OCR_LARGE="fb630ef46b64048f82401299b8f6d64f18f7d7c96010f5c3869d6433379b22c8"

LONGP="OCR: The quick brown fox jumps over the lazy dog. Invoices total one thousand dollars. Page one of three report two thousand twenty six. Packing list shipper consignee notify party vessel voyage bill of lading number container seal weight measurement. Description of goods harmonized code quantity unit price amount. Terms and conditions apply to all shipments. Payment due within thirty days of invoice date. Late payments accrue interest at one percent per month."

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

python3 "$GEN_SCRIPT" "$WORK" >/dev/null

failures=0
cases=0

# run_case <name> <golden_hash> <n_tokens> <extra cli args...>
run_case() {
    local name="$1" golden="$2" n="$3"
    shift 3
    cases=$((cases + 1))
    local out="$WORK/$name.out" perf="$WORK/$name.perf"

    if ! "$MLLM_CLI" -m "$M" "$@" -n "$n" --backend "$BACKEND" --ctx 8192 \
        >"$out" 2>"$perf"; then
        echo "FAIL: $name (cli exited non-zero)"
        failures=$((failures + 1))
        return
    fi

    local actual
    actual="$($SHA256 "$out" | awk '{print $1}')"
    local enforce=1
    if [[ "$name" == "text_short" && "$BACKEND" != "metal" ]]; then
        enforce=0
    fi
    if [[ "$actual" != "$golden" ]]; then
        if [[ "$enforce" == "1" ]]; then
            echo "FAIL: $name output diverged from golden"
            echo "  expected sha256: $golden"
            echo "  actual   sha256: $actual"
            echo "  actual output (head): $(head -c 200 "$out")"
            failures=$((failures + 1))
        else
            echo "WARN: $name diverged from the Metal golden (expected on $BACKEND:"
            echo "      f16-vs-fp32 drift after ~24 tokens, see PERF_BASELINE.md)"
        fi
    else
        echo "PASS: $name (golden hash match)"
    fi

    # Perf table row (fields from the CLI perf footer on stderr).
    local ptok gen prefill decode toks
    ptok=$(awk -F': *' '/^prompt tokens/{print $2}' "$perf")
    gen=$(awk -F': *' '/^generated/{print $2}' "$perf")
    prefill=$(awk -F': *' '/^prefill ms/{print $2}' "$perf")
    decode=$(awk -F': *' '/^decode ms/{print $2}' "$perf")
    toks=$(awk -F': *' '/^tok\/s/{print $2}' "$perf")
    local row="| $name | $BACKEND | $ptok | $gen | $prefill | $decode | $toks |"
    PERF_ROWS+=("$row")
}

PERF_ROWS=()

# --- Case 1: plain-text short prompt (LM only)
run_case text_short "$GOLDEN_TEXT_SHORT" 32 -p "OCR Recognition:"

# --- Case 2: plain-text long prompt, ~175 tokens (LM only)
run_case text_long "$GOLDEN_TEXT_LONG" 32 -p "$LONGP"

# --- Case 3: OCR on doc_small (768 patches). The image text is rendered by
# gen_doc_images.py and known exactly, so assert the transcription itself.
run_case ocr_small "$GOLDEN_OCR_SMALL" 48 \
    --mmproj "$MM" -p "OCR:" --image "$WORK/doc_small.png"
if ! grep -q "The▁quick▁brown▁fox" "$WORK/ocr_small.out" ||
    ! grep -q "1,024.50" "$WORK/ocr_small.out"; then
    echo "FAIL: ocr_small did not transcribe the known image text"
    echo "  actual output: $(cat "$WORK/ocr_small.out")"
    failures=$((failures + 1))
else
    echo "PASS: ocr_small semantic check (known image text transcribed)"
fi

# --- Case 4: OCR on doc_large (3456 patches, 869 prompt tokens).
run_case ocr_large "$GOLDEN_OCR_LARGE" 48 \
    --mmproj "$MM" -p "OCR:" --image "$WORK/doc_large.png"
if [[ ! -s "$WORK/ocr_large.out" ]] || grep -q "<unk>" "$WORK/ocr_large.out"; then
    echo "FAIL: ocr_large produced empty or <unk> output"
    failures=$((failures + 1))
else
    echo "PASS: ocr_large sanity check (non-empty, no <unk>)"
fi

echo ""
echo "=== perf ($BACKEND, single run, cold process start) ==="
echo "| case | backend | prompt tok | gen tok | prefill ms | decode ms | tok/s |"
echo "|---|---|---|---|---|---|---|"
for row in "${PERF_ROWS[@]}"; do
    echo "$row"
done
if [[ -n "${PERF_MD:-}" ]]; then
    {
        echo "| case | backend | prompt tok | gen tok | prefill ms | decode ms | tok/s |"
        echo "|---|---|---|---|---|---|---|"
        printf '%s\n' "${PERF_ROWS[@]}"
    } >>"$PERF_MD"
fi

echo ""
if [[ "$failures" -gt 0 ]]; then
    echo "FAILED: $failures failure(s) across $cases cases"
    exit 1
fi
echo "OK: all $cases cases passed"
