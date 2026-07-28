#!/usr/bin/env python3
"""Kerberos (GSSAPI) 客户端。

运行前必须先 kinit（或 kinit -kt keytab），让本机凭证缓存里存有 TGT。
握手时 GSSAPI 会自动用 TGT 向 KDC 换取 demo/demo-server 的服务票据，
与服务端完成双向认证，然后用会话密钥加密发送消息。

用法: python3 client.py [消息]
"""
import socket
import struct
import sys

import gssapi

SERVER_HOST = "demo-server"
SERVER_PORT = 9999
# hostbased_service 会被解析为 demo/demo-server@LAB.LOCAL
SERVICE_NAME = f"demo@{SERVER_HOST}"


def recv_exact(conn: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def recv_msg(conn: socket.socket) -> bytes:
    (length,) = struct.unpack("!I", recv_exact(conn, 4))
    return recv_exact(conn, length)


def send_msg(conn: socket.socket, data: bytes) -> None:
    conn.sendall(struct.pack("!I", len(data)) + data)


def main() -> int:
    message = sys.argv[1] if len(sys.argv) > 1 else "hello kerberos"

    target = gssapi.Name(SERVICE_NAME, name_type=gssapi.NameType.hostbased_service)
    # usage="initiate" 时会从默认凭证缓存（kinit 产生的 ccache）取 TGT；
    # 没有 TGT 会在 step() 时报 "No credentials cache found"。
    ctx = gssapi.SecurityContext(
        name=target,
        usage="initiate",
        flags=[gssapi.RequirementFlag.mutual_authentication],
    )

    conn = socket.create_connection((SERVER_HOST, SERVER_PORT), timeout=10)
    try:
        # ---- GSSAPI 握手：与服务端往返交换 token ----
        # 幕后发生：TGS_REQ -> KDC 签发服务票据 -> AP_REQ 发给服务端验证
        token = ctx.step()
        while True:
            if token:
                send_msg(conn, token)
            if ctx.complete:
                break
            token = ctx.step(recv_msg(conn))

        mutual = gssapi.RequirementFlag.mutual_authentication in ctx.actual_flags
        print(f"[+] 认证成功，对端身份: {ctx.target_name} (mutual={mutual})")

        # ---- 加密发送 / 接收 ----
        send_msg(conn, ctx.wrap(message.encode(), encrypt=True).message)
        reply = ctx.unwrap(recv_msg(conn)).message.decode()
        print(f"[reply] {reply}")
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
