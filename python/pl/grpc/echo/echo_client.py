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
# Created: 2026/06/21

"""Python gRPC echo client — tests all 5 RPCs against a server."""

import sys
import time

import grpc

from proto.echo.echo_pb2 import (
    ChatMessage,
    EchoRequest,
    HealthRequest,
    HealthResponse,
    ServerStreamRequest,
)
from proto.echo.echo_pb2_grpc import EchoServiceStub


class EchoClient:
    """Client for EchoService covering all 5 RPCs."""

    def __init__(self, host: str = "localhost", port: int = 50051):
        self.channel = grpc.insecure_channel(f"{host}:{port}")
        self.stub = EchoServiceStub(self.channel)

    def close(self):
        self.channel.close()

    def do_echo(self, message: str):
        now_us = int(time.time() * 1_000_000)
        request = EchoRequest(
            message=message,
            timestamp_us=now_us,
            headers={"client": "python"},
        )
        try:
            response = self.stub.Echo(request, timeout=5)
            rtt_us = response.server_timestamp - now_us
            print(
                f"[Echo] response: {response.message}"
                f" | rtt_us={rtt_us}"
                f" | server={response.server_id}"
            )
        except grpc.RpcError as e:
            print(f"[Echo] RPC failed: {e.code()} {e.details()}")

    def do_server_stream(self, pattern: str, max_responses: int):
        request = ServerStreamRequest(pattern=pattern, max_responses=max_responses)
        try:
            print(
                f"[ServerStream] receiving items (pattern='{pattern}', limit={max_responses}):"
            )
            for item in self.stub.ServerStream(request, timeout=10):
                print(f"  [{item.index}] {item.content}")
        except grpc.RpcError as e:
            print(f"[ServerStream] RPC failed: {e.code()} {e.details()}")

    def do_client_stream(self, messages: list):
        def generate():
            for msg in messages:
                yield EchoRequest(message=msg)

        try:
            response = self.stub.ClientStream(generate(), timeout=10)
            print(
                f"[ClientStream] summary: count={response.message_count}"
                f" | server={response.server_id}"
            )
            for i, msg in enumerate(response.messages):
                print(f"  [{i}] {msg}")
        except grpc.RpcError as e:
            print(f"[ClientStream] RPC failed: {e.code()} {e.details()}")

    def do_chat(self, messages_to_send: list):
        def generate():
            for content in messages_to_send:
                msg = ChatMessage(
                    sender="python-client",
                    content=content,
                    timestamp_us=int(time.time() * 1_000_000),
                )
                yield msg
                time.sleep(0.05)

        try:
            print("[Chat] round-trip:")
            for response in self.stub.Chat(generate(), timeout=15):
                print(f"  {response.sender} → {response.content}")
        except grpc.RpcError as e:
            print(f"[Chat] RPC failed: {e.code()} {e.details()}")

    def do_health_check(self):
        request = HealthRequest()
        try:
            response = self.stub.HealthCheck(request, timeout=5)
            status_name = HealthResponse.Status.Name(response.status)
            print(
                f"[HealthCheck] status={status_name}"
                f" | server={response.server_id}"
                f" | version={response.version}"
                f" | uptime={response.uptime_seconds}s"
            )
        except grpc.RpcError as e:
            print(f"[HealthCheck] RPC failed: {e.code()} {e.details()}")


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 50051

    client = EchoClient(host, port)
    try:
        print("============ HealthCheck ============")
        client.do_health_check()

        print("============ Unary Echo =============")
        client.do_echo("Hello from Python client!")

        print("============ Server Stream ==========")
        client.do_server_stream("[aeiou].*", 5)

        print("============ Client Stream ==========")
        client.do_client_stream(["msg-1", "msg-2", "msg-3"])

        print("============ Bidi Chat ==============")
        client.do_chat(
            ["First message", "Second message", "Third message", "Fourth message"]
        )
    finally:
        client.close()


if __name__ == "__main__":
    main()
