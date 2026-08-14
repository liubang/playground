#!/usr/bin/env python3
"""FoundationDB status json -> Prometheus exporter（仅使用 Python 标准库）。

读取 status-collector 写入共享卷的 status.json，将其中的数值/布尔字段
按路径展平为 Prometheus 指标，通过 /metrics 暴露：

- cluster.*          -> fdb_cluster_<path>          （gauge）
- cluster.processes  -> fdb_process_<path>{address, class_type, machine_id, zone_id}
- cluster.machines   -> fdb_machine_<path>{machine, address}
- client.*           -> fdb_client_<path>
- 元信息             -> fdb_exporter_status_json_ok / fdb_status_json_mtime_seconds

采用防御式解析：字段缺失仅导致对应指标不输出，不会导致 exporter 崩溃，
因此对 FDB 各版本 status json 的字段差异具有兼容性。
"""

import json
import math
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATUS_FILE = os.environ.get("STATUS_FILE", "/status/status.json")
LISTEN_ADDR = os.environ.get("LISTEN_ADDR", "0.0.0.0")
LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "9189"))


def sanitize(name):
    """将任意字段路径转换为合法的 Prometheus 指标名片段。"""
    return "".join(c if c.isalnum() or c == "_" else "_" for c in name)


def escape_label(value):
    return str(value).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


class Collector:
    def __init__(self):
        self.lines = []

    def emit(self, name, value, labels=None):
        if isinstance(value, bool):
            value = 1 if value else 0
        if not isinstance(value, (int, float)):
            return
        if isinstance(value, float) and not math.isfinite(value):
            return
        lbl = ""
        if labels:
            lbl = (
                "{"
                + ",".join(f'{k}="{escape_label(v)}"' for k, v in labels.items())
                + "}"
            )
        self.lines.append(f"{name}{lbl} {value}")

    def flatten(self, prefix, node, labels=None):
        """递归展平 dict：数值/布尔叶子输出为指标，字符串与 list 跳过。"""
        if isinstance(node, dict):
            for key, val in node.items():
                self.flatten(f"{prefix}_{sanitize(key)}", val, labels)
        elif isinstance(node, list):
            return  # list 由调用方按需定制处理
        else:
            self.emit(prefix, node, labels)


def collect_client(out, client):
    if not isinstance(client, dict):
        return
    out.flatten("fdb_client", client)
    coordinators = client.get("coordinators", {}).get("coordinators", [])
    for coord in coordinators:
        if isinstance(coord, dict):
            out.emit(
                "fdb_client_coordinator_reachable",
                coord.get("reachable", False),
                {"address": coord.get("address", "unknown")},
            )


def collect_processes(out, processes):
    for key, proc in processes.items():
        if not isinstance(proc, dict):
            continue
        # dict 的 key 是进程 ID（如 12fceab...），真实地址在 address 字段
        addr = proc.get("address") or key
        locality = proc.get("locality") or {}
        # label 名避免用 instance：Prometheus 会将其改写为 exported_instance
        labels = {
            "address": addr,
            "class_type": proc.get("class_type", "unknown"),
            "machine_id": locality.get("machineid", "unknown"),
            "zone_id": locality.get("zoneid", "unknown"),
        }
        for field, val in proc.items():
            if field in ("locality", "roles", "messages"):
                continue
            out.flatten(f"fdb_process_{sanitize(field)}", val, labels)
        version = proc.get("version")
        if version:
            out.emit(
                "fdb_process_version_info", 1, {"address": addr, "version": version}
            )
        for role in proc.get("roles") or []:
            if isinstance(role, dict):
                out.emit(
                    "fdb_process_role_info",
                    1,
                    {"address": addr, "role": role.get("role", "unknown")},
                )
        out.emit("fdb_process_roles", len(proc.get("roles") or []), {"address": addr})
        out.emit(
            "fdb_process_messages", len(proc.get("messages") or []), {"address": addr}
        )


def collect_machines(out, machines):
    for machine_id, machine in machines.items():
        if not isinstance(machine, dict):
            continue
        labels = {"machine": machine_id}
        if machine.get("address"):
            labels["address"] = machine["address"]
        locality = machine.get("locality") or {}
        if locality.get("dcid"):
            labels["dc_id"] = locality["dcid"]
        for key, val in machine.items():
            if key in ("locality", "address"):
                continue
            out.flatten(f"fdb_machine_{sanitize(key)}", val, labels)


def collect_cluster(out, cluster):
    if not isinstance(cluster, dict):
        return
    for key, val in cluster.items():
        if key in (
            "processes",
            "machines",
            "incompatible_connections",
            "recovery_state",
        ):
            continue
        out.flatten(f"fdb_cluster_{sanitize(key)}", val)
    recovery = cluster.get("recovery_state") or {}
    if isinstance(recovery, dict):
        if recovery.get("name"):
            out.emit("fdb_cluster_recovery_state_info", 1, {"name": recovery["name"]})
        for key, val in recovery.items():
            if key not in ("name", "description"):
                out.flatten(f"fdb_cluster_recovery_state_{sanitize(key)}", val)
    out.emit(
        "fdb_cluster_incompatible_connections",
        len(cluster.get("incompatible_connections") or []),
    )
    collect_processes(out, cluster.get("processes") or {})
    collect_machines(out, cluster.get("machines") or {})


def render_metrics():
    out = Collector()
    status = None
    try:
        with open(STATUS_FILE, encoding="utf-8") as f:
            status = json.load(f)
        out.emit("fdb_exporter_status_json_ok", 1)
    except (OSError, ValueError):
        out.emit("fdb_exporter_status_json_ok", 0)
    try:
        out.emit("fdb_status_json_mtime_seconds", os.path.getmtime(STATUS_FILE))
    except OSError:
        pass
    if isinstance(status, dict):
        collect_client(out, status.get("client") or {})
        collect_cluster(out, status.get("cluster") or {})
    return ("\n".join(out.lines) + "\n").encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/healthz":
            self._respond(200, b"ok\n", "text/plain")
        elif self.path in ("/", "/metrics"):
            self._respond(
                200, render_metrics(), "text/plain; version=0.0.4; charset=utf-8"
            )
        else:
            self._respond(404, b"not found\n", "text/plain")

    def _respond(self, code, body, content_type):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):  # 静默访问日志
        pass


def main():
    server = ThreadingHTTPServer((LISTEN_ADDR, LISTEN_PORT), Handler)
    print(
        f"fdb-exporter listening on {LISTEN_ADDR}:{LISTEN_PORT}, status file: {STATUS_FILE}"
    )
    server.serve_forever()


if __name__ == "__main__":
    main()
