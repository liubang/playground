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

"""Python gRPC echo server implementing all 5 RPCs."""

import re
import sys
import time
from concurrent import futures
from datetime import datetime, timezone

import grpc

from pl.grpc.proto.echo_pb2 import (
    ChatMessage,
    EchoRequest,
    EchoResponse,
    EchoSummary,
    HealthRequest,
    HealthResponse,
    ServerStreamRequest,
    StreamItem,
)
from pl.grpc.proto.echo_pb2_grpc import (
    EchoServiceServicer,
    EchoServiceStub,
    add_EchoServiceServicer_to_server,
)


class EchoService(EchoServiceServicer):
    """Implementation of EchoService for all 5 RPCs."""

    ITEMS = [
        "Alpha", "Bravo", "Charlie", "Delta", "Echo",
        "Foxtrot", "Golf", "Hotel", "India", "Juliet",
    ]

    def __init__(self, server_id: str):
        self.server_id = server_id
        self.start_time = time.monotonic()

    def Echo(self, request: EchoRequest, context: grpc.ServicerContext) -> EchoResponse:
        now_us = int(time.time() * 1_000_000)
        return EchoResponse(
            message=request.message,
            original_timestamp=request.timestamp_us,
            server_timestamp=now_us,
            server_id=self.server_id,
        )

    def ServerStream(
        self, request: ServerStreamRequest, context: grpc.ServicerContext
    ):
        pattern = None
        if request.pattern:
            try:
                pattern = re.compile(request.pattern, re.IGNORECASE)
            except re.error:
                context.abort(grpc.StatusCode.INVALID_ARGUMENT, "Invalid regex pattern")
                return

        limit = request.max_responses if request.max_responses > 0 else len(self.ITEMS)
        count = 0
        for i, content in enumerate(self.ITEMS):
            if count >= limit:
                break
            if pattern and not pattern.search(content):
                continue
            yield StreamItem(index=i, content=content)
            count += 1

    def ClientStream(
        self, request_iterator, context: grpc.ServicerContext
    ) -> EchoSummary:
        messages = []
        for req in request_iterator:
            messages.append(req.message)
        now_us = int(time.time() * 1_000_000)
        return EchoSummary(
            message_count=len(messages),
            messages=messages,
            server_timestamp=now_us,
            server_id=self.server_id,
        )

    def Chat(self, request_iterator, context: grpc.ServicerContext):
        for msg in request_iterator:
            yield msg

    def HealthCheck(
        self, request: HealthRequest, context: grpc.ServicerContext
    ) -> HealthResponse:
        uptime = int(time.monotonic() - self.start_time)
        return HealthResponse(
            status=HealthResponse.SERVING,
            server_id=self.server_id,
            version="1.0.0",
            uptime_seconds=uptime,
        )


def serve(port: int, server_id: str):
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    add_EchoServiceServicer_to_server(EchoService(server_id), server)
    server.add_insecure_port(f"0.0.0.0:{port}")
    server.start()
    print(f"[Python EchoServer] Listening on port {port} (id: {server_id})")
    server.wait_for_termination()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 50051
    server_id = sys.argv[2] if len(sys.argv) > 2 else "python-server"
    serve(port, server_id)
