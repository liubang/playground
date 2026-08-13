#!/usr/bin/env python3
"""Kerberos (GSSAPI) 回显服务端。

只接受通过 Kerberos 双向认证的连接：客户端必须先向 KDC 拿到
demo/demo-server@LAB.LOCAL 的服务票据，否则握手直接失败。
认证通过后，后续消息用会话密钥加密（GSS wrap/unwrap）传输。

服务自身不"输密码"：它通过 KRB5_KTNAME 指向的 keytab 证明自己的身份。
"""

import socket
import struct
import sys

import gssapi

HOST = "0.0.0.0"
PORT = 9999


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


def handle(conn: socket.socket, addr, server_creds: gssapi.Credentials) -> None:
    # ---- 阶段 1: GSSAPI 握手（Kerberos 认证 + 协商会话密钥）----
    ctx = gssapi.SecurityContext(creds=server_creds, usage="accept")
    while not ctx.complete:
        token = recv_msg(conn)
        out = ctx.step(token)
        if out:
            send_msg(conn, out)

    initiator = str(ctx.initiator_name)
    mutual = gssapi.RequirementFlag.mutual_authentication in ctx.actual_flags
    print(
        f"[+] {addr[0]}:{addr[1]} 认证通过: {initiator} "
        f"(mutual={mutual}, lifetime={ctx.lifetime}s)",
        flush=True,
    )

    # ---- 阶段 2: 用会话密钥加密的回显服务 ----
    while True:
        try:
            wrapped = recv_msg(conn)
        except ConnectionError:
            break
        msg = ctx.unwrap(wrapped).message
        text = msg.decode()
        print(f"[msg] {initiator}: {text}", flush=True)
        reply = ctx.wrap(f"ECHO[{initiator}]: {text}".encode(), encrypt=True)
        send_msg(conn, reply.message)

    print(f"[-] {initiator} 断开", flush=True)


def main() -> int:
    # 不带 name 获取 acceptor 凭证：接受 keytab（KRB5_KTNAME）中的任意主体
    server_creds = gssapi.Credentials(usage="accept")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((HOST, PORT))
    sock.listen(5)
    print(f"[*] demo-server 监听 {HOST}:{PORT}，等待 Kerberos 认证连接...", flush=True)

    while True:
        conn, addr = sock.accept()
        try:
            handle(conn, addr, server_creds)
        except Exception as exc:  # 实验环境：打印所有失败原因（包括认证失败）
            print(f"[!] {addr[0]}:{addr[1]} 处理失败: {exc}", flush=True)
        finally:
            conn.close()


if __name__ == "__main__":
    sys.exit(main())
