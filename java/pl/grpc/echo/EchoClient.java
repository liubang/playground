// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/06/21

package pl.grpc.echo;

import io.grpc.ManagedChannel;
import io.grpc.ManagedChannelBuilder;
import io.grpc.StatusRuntimeException;
import io.grpc.stub.StreamObserver;
import pl.grpc.proto.EchoServiceGrpc;
import pl.grpc.proto.StreamServiceGrpc;
import pl.grpc.proto.*;

import java.util.Iterator;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Java gRPC echo client using generated stubs. */
public class EchoClient {
    private final ManagedChannel channel;
    private final EchoServiceGrpc.EchoServiceBlockingStub blockingStub;
    private final StreamServiceGrpc.StreamServiceBlockingStub streamBlockingStub;
    private final StreamServiceGrpc.StreamServiceStub streamAsyncStub;

    public EchoClient(String host, int port) {
        channel = ManagedChannelBuilder.forAddress(host, port).usePlaintext().build();
        blockingStub = EchoServiceGrpc.newBlockingStub(channel);
        streamBlockingStub = StreamServiceGrpc.newBlockingStub(channel);
        streamAsyncStub = StreamServiceGrpc.newStub(channel);
    }

    public void shutdown() throws InterruptedException {
        channel.shutdown().awaitTermination(5, TimeUnit.SECONDS);
    }

    public void doEcho(String message) {
        long now = System.currentTimeMillis() * 1000;
        EchoRequest req = EchoRequest.newBuilder()
                .setMessage(message).setTimestampUs(now).putHeaders("client", "java").build();
        try {
            EchoResponse resp = blockingStub.echo(req);
            System.out.println("[Echo] response: " + resp.getMessage()
                    + " | rtt_us=" + (resp.getServerTimestamp() - now)
                    + " | server=" + resp.getServerId());
        } catch (StatusRuntimeException e) {
            System.err.println("[Echo] RPC failed: " + e.getStatus());
        }
    }

    public void doServerStream(String pattern, int maxResponses) {
        ServerStreamRequest req = ServerStreamRequest.newBuilder()
                .setPattern(pattern).setMaxResponses(maxResponses).build();
        try {
            Iterator<StreamItem> items = streamBlockingStub.serverStream(req);
            System.out.println("[ServerStream] receiving items (pattern='" + pattern
                    + "', limit=" + maxResponses + "):");
            while (items.hasNext()) {
                StreamItem item = items.next();
                System.out.println("  [" + item.getIndex() + "] " + item.getContent());
            }
        } catch (StatusRuntimeException e) {
            System.err.println("[ServerStream] RPC failed: " + e.getStatus());
        }
    }

    public void doClientStream(List<String> messages) throws InterruptedException {
        CountDownLatch latch = new CountDownLatch(1);
        StreamObserver<EchoRequest> requestObserver = streamAsyncStub.clientStream(
                new StreamObserver<>() {
                    @Override public void onNext(EchoSummary summary) {
                        System.out.println("[ClientStream] summary: count=" + summary.getMessageCount()
                                + " | server=" + summary.getServerId());
                        for (int i = 0; i < summary.getMessagesCount(); i++)
                            System.out.println("  [" + i + "] " + summary.getMessages(i));
                    }
                    @Override public void onError(Throwable t) {
                        System.err.println("[ClientStream] RPC failed: " + t.getMessage());
                        latch.countDown();
                    }
                    @Override public void onCompleted() { latch.countDown(); }
                });
        for (String msg : messages)
            requestObserver.onNext(EchoRequest.newBuilder().setMessage(msg).build());
        requestObserver.onCompleted();
        latch.await(5, TimeUnit.SECONDS);
    }

    public void doChat(List<String> messagesToSend) throws InterruptedException {
        CountDownLatch latch = new CountDownLatch(1);
        StreamObserver<ChatMessage> requestObserver = streamAsyncStub.chat(
                new StreamObserver<>() {
                    @Override public void onNext(ChatMessage msg) {
                        System.out.println("  " + msg.getSender() + " → " + msg.getContent());
                    }
                    @Override public void onError(Throwable t) {
                        System.err.println("[Chat] RPC failed: " + t.getMessage());
                        latch.countDown();
                    }
                    @Override public void onCompleted() { latch.countDown(); }
                });
        System.out.println("[Chat] round-trip:");
        for (String content : messagesToSend) {
            requestObserver.onNext(ChatMessage.newBuilder()
                    .setSender("java-client").setContent(content)
                    .setTimestampUs(System.currentTimeMillis() * 1000).build());
            Thread.sleep(50);
        }
        requestObserver.onCompleted();
        latch.await(10, TimeUnit.SECONDS);
    }

    public void doHealthCheck() {
        try {
            HealthResponse resp = blockingStub.healthCheck(HealthRequest.newBuilder().build());
            System.out.println("[HealthCheck] status=" + resp.getStatus().name()
                    + " | server=" + resp.getServerId() + " | version=" + resp.getVersion()
                    + " | uptime=" + resp.getUptimeSeconds() + "s");
        } catch (StatusRuntimeException e) {
            System.err.println("[HealthCheck] RPC failed: " + e.getStatus());
        }
    }

    public static void main(String[] args) throws Exception {
        String host = args.length >= 1 ? args[0] : "localhost";
        int port = args.length >= 2 ? Integer.parseInt(args[1]) : 50051;
        EchoClient client = new EchoClient(host, port);
        try {
            System.out.println("============ HealthCheck ============");
            client.doHealthCheck();
            System.out.println("============ Unary Echo =============");
            client.doEcho("Hello from Java client!");
            System.out.println("============ Server Stream ==========");
            client.doServerStream("[aeiou].*", 5);
            System.out.println("============ Client Stream ==========");
            client.doClientStream(List.of("msg-1", "msg-2", "msg-3"));
            System.out.println("============ Bidi Chat ==============");
            client.doChat(List.of("First message", "Second message", "Third message", "Fourth message"));
        } finally { client.shutdown(); }
    }
}
