#! /usr/bin/env python
# -*- coding: utf-8 -*-
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
# Created: 2026/05/14 16:21

"""
MLX-based Embedding Server — OpenAI-compatible /v1/embeddings API.

This server loads a Transformer embedding model (default: BAAI/bge-m3) using
Apple MLX for inference on Apple Silicon, and exposes an HTTP endpoint that
is wire-compatible with the OpenAI Embeddings API.

Usage:
    python server.py [--model BAAI/bge-m3] [--host 0.0.0.0] [--port 8000] [--max-length 512]
"""

from __future__ import annotations

# ---------------------------------------------------------------------------
# Preload MLX native libraries before importing mlx.
#
# Bazel's rules_python extracts the ``mlx`` and ``mlx-metal`` wheels into
# separate external-repo directories, breaking the @rpath/libmlx.dylib
# reference in core.cpython-*-darwin.so.  We fix this by scanning sys.path
# for the dylib shipped by mlx-metal and preloading it via ctypes.
# ---------------------------------------------------------------------------
import ctypes
import os
import sys

def _preload_mlx_native_libs() -> bool:
    for path_entry in sys.path:
        dylib = os.path.join(path_entry, "mlx", "lib", "libmlx.dylib")
        if os.path.isfile(dylib):
            try:
                ctypes.CDLL(dylib, mode=ctypes.RTLD_GLOBAL)
                return True
            except OSError:
                continue
    runfiles_dir = os.environ.get("RUNFILES_DIR", "")
    if runfiles_dir:
        for root, _dirs, files in os.walk(runfiles_dir):
            if "libmlx.dylib" in files:
                try:
                    ctypes.CDLL(os.path.join(root, "libmlx.dylib"), mode=ctypes.RTLD_GLOBAL)
                    return True
                except OSError:
                    continue
    return False

_preload_mlx_native_libs()
# ---------------------------------------------------------------------------

import argparse
import logging
import time
import uuid
from pathlib import Path
from typing import Union

import mlx.core as mx
import mlx.nn as nn
import numpy as np
import uvicorn
from fastapi import FastAPI, HTTPException
from huggingface_hub import snapshot_download
from pydantic import BaseModel, Field
from safetensors import safe_open
from transformers import AutoConfig, AutoTokenizer

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s - %(message)s",
)
logger = logging.getLogger("embedding_server")

# ---------------------------------------------------------------------------
# Pydantic request / response models (OpenAI-compatible)
# ---------------------------------------------------------------------------


class EmbeddingRequest(BaseModel):
    input: Union[str, list[str]] = Field(..., description="Text(s) to embed")
    model: str = Field(default="bge-m3", description="Model identifier (informational)")
    encoding_format: str = Field(default="float", description="Encoding format")


class EmbeddingData(BaseModel):
    object: str = "embedding"
    embedding: list[float]
    index: int


class UsageInfo(BaseModel):
    prompt_tokens: int
    total_tokens: int


class EmbeddingResponse(BaseModel):
    object: str = "list"
    data: list[EmbeddingData]
    model: str
    usage: UsageInfo


# ---------------------------------------------------------------------------
# MLX Transformer Embedding Model
# ---------------------------------------------------------------------------


class MLXEmbeddingModel:
    """Loads a HuggingFace transformer model into MLX and runs mean-pooling
    inference entirely on the Apple GPU / ANE via MLX."""

    def __init__(self, model_name_or_path: str, max_length: int = 512):
        self.max_length = max_length
        self.model_name = model_name_or_path

        # Download model if needed
        logger.info("Loading model: %s", model_name_or_path)
        local_path = self._resolve_model_path(model_name_or_path)

        # Load tokenizer
        self.tokenizer = AutoTokenizer.from_pretrained(local_path)

        # Load config
        config = AutoConfig.from_pretrained(local_path)
        self.hidden_size = config.hidden_size
        self.num_attention_heads = config.num_attention_heads
        self.num_hidden_layers = config.num_hidden_layers
        self.layer_norm_eps = getattr(config, "layer_norm_eps", 1e-5)

        # Load weights into MLX arrays
        self.weights = self._load_weights(local_path)
        logger.info(
            "Model loaded: hidden_size=%d, num_heads=%d, num_layers=%d, "
            "max_length=%d, weights=%d tensors",
            self.hidden_size,
            self.num_attention_heads,
            self.num_hidden_layers,
            self.max_length,
            len(self.weights),
        )

    @staticmethod
    def _resolve_model_path(model_name_or_path: str) -> str:
        """Return a local directory — download from HF Hub if necessary."""
        p = Path(model_name_or_path)
        if p.is_dir():
            return str(p)
        return snapshot_download(
            repo_id=model_name_or_path,
            allow_patterns=["*.json", "*.safetensors", "*.bin", "*.txt", "*.model"],
        )

    @staticmethod
    def _load_weights(model_dir: str) -> dict[str, mx.array]:
        """Load model weights into a flat dict of MLX arrays.

        Tries safetensors first (preferred), then falls back to PyTorch
        ``pytorch_model.bin`` using a lightweight pickle-based loader that
        does **not** require PyTorch to be installed.
        """
        weights: dict[str, mx.array] = {}
        model_path = Path(model_dir)

        # --- Strategy 1: safetensors (preferred) ---
        sf_files = sorted(model_path.glob("*.safetensors"))
        if sf_files:
            for sf_file in sf_files:
                with safe_open(str(sf_file), framework="numpy") as f:
                    for key in f.keys():
                        weights[key] = mx.array(f.get_tensor(key))
            return weights

        # --- Strategy 2: pytorch_model.bin (torch.save format) ---
        bin_files = sorted(model_path.glob("pytorch_model*.bin"))
        if bin_files:
            weights = MLXEmbeddingModel._load_pytorch_bin(bin_files)
            if weights:
                return weights

        raise FileNotFoundError(
            f"No .safetensors or pytorch_model*.bin files found in {model_dir}"
        )

    @staticmethod
    def _load_pytorch_bin(bin_files: list[Path]) -> dict[str, mx.array]:
        """Load PyTorch checkpoint files without requiring torch.

        PyTorch's ``torch.save`` produces a ZIP archive containing pickled
        metadata and raw numpy-compatible tensor storage.  We use Python's
        ``zipfile`` + a restricted ``pickle.Unpickler`` to extract the
        tensors safely, then convert them to MLX arrays.
        """
        import io
        import pickle
        import struct
        import zipfile

        weights: dict[str, mx.array] = {}

        for bin_file in bin_files:
            logger.info("Loading PyTorch weights from %s", bin_file.name)
            with zipfile.ZipFile(str(bin_file), "r") as zf:
                # Find the data.pkl entry
                pkl_entries = [n for n in zf.namelist() if n.endswith("data.pkl")]
                if not pkl_entries:
                    continue

                # Build a mapping from storage name -> raw bytes
                data_entries = {
                    n.split("/")[-1]: n
                    for n in zf.namelist()
                    if "/data/" in n and not n.endswith("data.pkl")
                }

                class _TorchUnpickler(pickle.Unpickler):
                    """Restricted unpickler that handles torch storage objects."""

                    _DTYPE_MAP = {
                        "FloatStorage": (np.float32, 4),
                        "DoubleStorage": (np.float64, 8),
                        "HalfStorage": (np.float16, 2),
                        "BFloat16Storage": ("bfloat16", 2),
                        "LongStorage": (np.int64, 8),
                        "IntStorage": (np.int32, 4),
                        "ShortStorage": (np.int16, 2),
                        "ByteStorage": (np.uint8, 1),
                        "CharStorage": (np.int8, 1),
                        "BoolStorage": (np.bool_, 1),
                    }

                    def find_class(self, module: str, name: str):
                        if module == "collections" and name == "OrderedDict":
                            from collections import OrderedDict
                            return OrderedDict
                        if module == "torch._utils" and name == "_rebuild_tensor_v2":
                            return self._rebuild_tensor_v2
                        if "torch" in module and "Storage" in name:
                            return self._make_storage_class(name)
                        if module == "torch" and name in ("FloatStorage", "HalfStorage",
                                                          "BFloat16Storage", "LongStorage",
                                                          "IntStorage", "ShortStorage",
                                                          "ByteStorage", "BoolStorage",
                                                          "DoubleStorage", "CharStorage"):
                            return self._make_storage_class(name)
                        # Allow basic builtins
                        if module == "builtins":
                            import builtins
                            return getattr(builtins, name)
                        if module == "_codecs" and name == "encode":
                            import _codecs
                            return _codecs.encode
                        raise pickle.UnpicklingError(
                            f"Forbidden: {module}.{name}"
                        )

                    def _make_storage_class(self, name: str):
                        dtype_info = self._DTYPE_MAP.get(name, (np.float32, 4))
                        class _Storage:
                            def __init__(self, size):
                                self.size = size
                                self.dtype_info = dtype_info
                            def __reduce_ex__(self, protocol):
                                return (self.__class__, (self.size,))
                        _Storage.__name__ = name
                        _Storage.__qualname__ = name
                        return _Storage

                    @staticmethod
                    def _rebuild_tensor_v2(storage, storage_offset, size, stride, *args):
                        return (storage, storage_offset, size, stride)

                    def persistent_load(self, saved_id):
                        if isinstance(saved_id, tuple) and len(saved_id) >= 5:
                            # ("storage", storage_cls, key, location, numel)
                            _, storage_cls, key, _location, numel = saved_id[:5]
                            return (storage_cls, str(key), numel)
                        return saved_id

                # Unpickle the state dict
                pkl_data = zf.read(pkl_entries[0])
                unpickler = _TorchUnpickler(io.BytesIO(pkl_data))
                state_dict = unpickler.load()

                # Resolve tensors
                for param_name, tensor_info in state_dict.items():
                    if not isinstance(tensor_info, tuple) or len(tensor_info) < 4:
                        continue
                    storage_info, storage_offset, size, stride = tensor_info[:4]
                    if not isinstance(storage_info, tuple) or len(storage_info) < 3:
                        continue
                    storage_cls, data_key, _numel = storage_info
                    dtype_np, elem_size = storage_cls.dtype_info if hasattr(storage_cls, 'dtype_info') else (np.float32, 4)

                    # Read raw data from zip
                    if data_key not in data_entries:
                        continue
                    raw = zf.read(data_entries[data_key])

                    # Handle bfloat16 specially
                    is_bf16 = (dtype_np == "bfloat16")
                    if is_bf16:
                        # Read as uint16, convert to float32
                        arr = np.frombuffer(raw, dtype=np.uint16)
                        # bfloat16 -> float32: shift left by 16 bits
                        arr_f32 = np.zeros(len(arr), dtype=np.float32)
                        arr_f32.view(np.uint32)[:] = arr.astype(np.uint32) << 16
                        total_elements = 1
                        for s in size:
                            total_elements *= s
                        offset_elements = storage_offset
                        arr_f32 = arr_f32[offset_elements:offset_elements + total_elements]
                        tensor = arr_f32.reshape(size)
                    else:
                        arr = np.frombuffer(raw, dtype=dtype_np)
                        total_elements = 1
                        for s in size:
                            total_elements *= s
                        offset_elements = storage_offset
                        arr = arr[offset_elements:offset_elements + total_elements]
                        tensor = arr.reshape(size)

                    weights[param_name] = mx.array(tensor)

        return weights

    def _get_weight(self, name: str) -> mx.array:
        """Retrieve a weight tensor by name, raising a clear error if missing."""
        if name not in self.weights:
            raise KeyError(f"Weight '{name}' not found in model")
        return self.weights[name]

    def _layer_norm(self, x: mx.array, weight_key: str, bias_key: str, eps: float | None = None) -> mx.array:
        """Apply layer normalization."""
        if eps is None:
            eps = self.layer_norm_eps
        weight = self._get_weight(weight_key)
        bias = self._get_weight(bias_key)
        mean = mx.mean(x, axis=-1, keepdims=True)
        var = mx.var(x, axis=-1, keepdims=True)
        return weight * (x - mean) / mx.sqrt(var + eps) + bias

    def _linear(self, x: mx.array, weight_key: str, bias_key: str | None = None) -> mx.array:
        """Apply a linear projection."""
        w = self._get_weight(weight_key)
        out = x @ w.T
        if bias_key and bias_key in self.weights:
            out = out + self._get_weight(bias_key)
        return out

    def _self_attention(self, hidden: mx.array, attention_mask: mx.array,
                        prefix: str, num_heads: int) -> mx.array:
        """Multi-head self-attention."""
        B, S, D = hidden.shape
        head_dim = D // num_heads

        q = self._linear(hidden, f"{prefix}.self.query.weight", f"{prefix}.self.query.bias")
        k = self._linear(hidden, f"{prefix}.self.key.weight", f"{prefix}.self.key.bias")
        v = self._linear(hidden, f"{prefix}.self.value.weight", f"{prefix}.self.value.bias")

        q = q.reshape(B, S, num_heads, head_dim).transpose(0, 2, 1, 3)
        k = k.reshape(B, S, num_heads, head_dim).transpose(0, 2, 1, 3)
        v = v.reshape(B, S, num_heads, head_dim).transpose(0, 2, 1, 3)

        scores = (q @ k.transpose(0, 1, 3, 2)) / mx.sqrt(mx.array(head_dim, dtype=q.dtype))

        if attention_mask is not None:
            mask_4d = attention_mask[:, None, None, :]  # (B, 1, 1, S)
            scores = scores + (1.0 - mask_4d) * (-1e9)

        attn_weights = mx.softmax(scores, axis=-1)
        context = (attn_weights @ v).transpose(0, 2, 1, 3).reshape(B, S, D)

        context = self._linear(context, f"{prefix}.output.dense.weight", f"{prefix}.output.dense.bias")
        return context

    def _transformer_layer(self, hidden: mx.array, attention_mask: mx.array,
                           layer_idx: int, num_heads: int) -> mx.array:
        """One transformer encoder layer: attention + FFN with residual + LN."""
        prefix = f"encoder.layer.{layer_idx}"

        # Self-attention + residual + LN
        attn_out = self._self_attention(hidden, attention_mask, f"{prefix}.attention", num_heads)
        hidden = self._layer_norm(
            hidden + attn_out,
            f"{prefix}.attention.output.LayerNorm.weight",
            f"{prefix}.attention.output.LayerNorm.bias",
        )

        # FFN: intermediate -> activation -> output
        intermediate = self._linear(hidden, f"{prefix}.intermediate.dense.weight",
                                    f"{prefix}.intermediate.dense.bias")
        intermediate = nn.gelu(intermediate)
        ffn_out = self._linear(intermediate, f"{prefix}.output.dense.weight",
                               f"{prefix}.output.dense.bias")
        hidden = self._layer_norm(
            hidden + ffn_out,
            f"{prefix}.output.LayerNorm.weight",
            f"{prefix}.output.LayerNorm.bias",
        )
        return hidden

    def _forward(self, input_ids: mx.array, attention_mask: mx.array) -> mx.array:
        """Full forward pass through the BERT-like encoder."""
        # Embeddings
        word_emb = self._get_weight("embeddings.word_embeddings.weight")[input_ids]
        pos_ids = mx.arange(input_ids.shape[1])[None, :]
        pos_emb = self._get_weight("embeddings.position_embeddings.weight")[pos_ids]
        tok_type_ids = mx.zeros_like(input_ids)
        tok_emb = self._get_weight("embeddings.token_type_embeddings.weight")[tok_type_ids]

        hidden = word_emb + pos_emb + tok_emb
        hidden = self._layer_norm(
            hidden,
            "embeddings.LayerNorm.weight",
            "embeddings.LayerNorm.bias",
        )

        for i in range(self.num_hidden_layers):
            hidden = self._transformer_layer(hidden, attention_mask, i, self.num_attention_heads)

        return hidden

    def encode(self, texts: list[str]) -> list[list[float]]:
        """Tokenize, run forward pass, mean-pool, L2-normalize, return embeddings."""
        encoded = self.tokenizer(
            texts,
            padding=True,
            truncation=True,
            max_length=self.max_length,
            return_tensors="np",
        )

        input_ids = mx.array(encoded["input_ids"])
        attention_mask = mx.array(encoded["attention_mask"]).astype(mx.float32)

        hidden = self._forward(input_ids, attention_mask)

        # Mean pooling: average over non-padding tokens
        mask_expanded = attention_mask[:, :, None]
        summed = mx.sum(hidden * mask_expanded, axis=1)
        counts = mx.sum(mask_expanded, axis=1)
        pooled = summed / mx.maximum(counts, mx.array(1e-9))

        # L2 normalize
        norms = mx.sqrt(mx.sum(pooled * pooled, axis=-1, keepdims=True))
        normalized = pooled / mx.maximum(norms, mx.array(1e-9))

        mx.eval(normalized)
        return np.array(normalized).tolist()


# ---------------------------------------------------------------------------
# FastAPI application
# ---------------------------------------------------------------------------

app = FastAPI(title="MLX Embedding Server", version="1.0.0")
model: MLXEmbeddingModel | None = None


@app.get("/health")
async def health():
    return {"status": "ok", "model": model.model_name if model else None}


@app.post("/v1/embeddings", response_model=EmbeddingResponse)
async def create_embeddings(request: EmbeddingRequest):
    if model is None:
        raise HTTPException(status_code=503, detail="Model not loaded")

    texts = request.input if isinstance(request.input, list) else [request.input]
    if not texts:
        raise HTTPException(status_code=400, detail="Input must not be empty")

    t0 = time.perf_counter()
    embeddings = model.encode(texts)
    elapsed = time.perf_counter() - t0

    total_tokens = sum(
        len(model.tokenizer.encode(t, add_special_tokens=True)) for t in texts
    )

    logger.info(
        "Encoded %d text(s) in %.3fs (%d tokens)", len(texts), elapsed, total_tokens
    )

    return EmbeddingResponse(
        data=[
            EmbeddingData(embedding=emb, index=i)
            for i, emb in enumerate(embeddings)
        ],
        model=request.model,
        usage=UsageInfo(prompt_tokens=total_tokens, total_tokens=total_tokens),
    )


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MLX Embedding Server")
    parser.add_argument("--model", type=str, default="BAAI/bge-m3",
                        help="HuggingFace model name or local path")
    parser.add_argument("--host", type=str, default="0.0.0.0",
                        help="Bind host")
    parser.add_argument("--port", type=int, default=8000,
                        help="Bind port")
    parser.add_argument("--max-length", type=int, default=512,
                        help="Max token length for truncation")
    return parser.parse_args()


def main():
    args = parse_args()
    global model
    model = MLXEmbeddingModel(args.model, max_length=args.max_length)
    logger.info("Starting server on %s:%d", args.host, args.port)
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
