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

import io.grpc.Server;
import io.grpc.ServerBuilder;
import io.grpc.Status;
import io.grpc.stub.StreamObserver;
import pl.grpc.proto.*;

import java.io.IOException;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.logging.Logger;
import java.util.regex.Pattern;
import java.util.regex.PatternSyntaxException;

/** Java gRPC echo server using generated stubs. */
public class EchoServer {
    private static final Logger logger = Logger.getLogger(EchoServer.class.getName());
    private static final String VERSION = "1.0.0";
    private static final List<String> ITEMS = List.of(
            "Alpha", "Bravo", "Charlie", "Delta", "Echo",
            "Foxtrot", "Golf", "Hotel", "India", "Juliet");

    static class EchoServiceImpl extends EchoServiceGrpc.EchoServiceImplBase {
        private final String serverId;
        private final Instant startTime;

        EchoServiceImpl(String serverId) {
            this.serverId = serverId;
            this.startTime = Instant.now();
        }

        // NOTE: Java timestamps use System.currentTimeMillis()*1000 which has
        // millisecond (not microsecond) granularity. The field type (int64) is
        // compatible but precision is lower than the C++/Go implementations.
        @Override
        public void echo(EchoRequest req, StreamObserver<EchoResponse> observer) {
            long now = System.currentTimeMillis() * 1000;
            observer.onNext(EchoResponse.newBuilder().setMessage(req.getMessage())
                    .setOriginalTimestamp(req.getTimestampUs()).setServerTimestamp(now)
                    .setServerId(serverId).build());
            observer.onCompleted();
        }

        @Override
        public void serverStream(ServerStreamRequest req, StreamObserver<StreamItem> observer) {
            Pattern pattern = null;
            if (!req.getPattern().isEmpty()) {
                try { pattern = Pattern.compile(req.getPattern(), Pattern.CASE_INSENSITIVE); }
                catch (PatternSyntaxException e) {
                    observer.onError(Status.INVALID_ARGUMENT
                            .withDescription("Invalid regex").asRuntimeException());
                    return;
                }
            }
            int limit = req.getMaxResponses() > 0 ? req.getMaxResponses() : ITEMS.size();
            for (int i = 0, count = 0; i < ITEMS.size() && count < limit; i++) {
                if (pattern != null && !pattern.matcher(ITEMS.get(i)).find()) continue;
                observer.onNext(StreamItem.newBuilder().setIndex(i).setContent(ITEMS.get(i)).build());
                count++;
            }
            observer.onCompleted();
        }

        @Override
        public StreamObserver<EchoRequest> clientStream(StreamObserver<EchoSummary> observer) {
            return new StreamObserver<>() {
                final EchoSummary.Builder summary = EchoSummary.newBuilder();
                int count;
                @Override public void onNext(EchoRequest req) { summary.addMessages(req.getMessage()); count++; }
                @Override public void onError(Throwable t) {
                    logger.warning("ClientStream error: " + t);
                    observer.onError(t); // propagate to client
                }
                @Override public void onCompleted() {
                    long now = System.currentTimeMillis() * 1000;
                    summary.setMessageCount(count).setServerTimestamp(now).setServerId(serverId);
                    observer.onNext(summary.build());
                    observer.onCompleted();
                }
            };
        }

        @Override
        public StreamObserver<ChatMessage> chat(StreamObserver<ChatMessage> observer) {
            return new StreamObserver<>() {
                @Override public void onNext(ChatMessage msg) { observer.onNext(msg); }
                @Override public void onError(Throwable t) {
                    logger.warning("Chat error: " + t);
                    observer.onError(t); // propagate to client
                }
                @Override public void onCompleted() { observer.onCompleted(); }
            };
        }

        @Override
        public void healthCheck(HealthRequest req, StreamObserver<HealthResponse> observer) {
            long uptime = Duration.between(startTime, Instant.now()).getSeconds();
            observer.onNext(HealthResponse.newBuilder().setStatus(HealthResponse.Status.SERVING)
                    .setServerId(serverId).setVersion(VERSION).setUptimeSeconds(uptime).build());
            observer.onCompleted();
        }
    }

    private final Server server;

    public EchoServer(int port, String serverId) throws IOException {
        server = ServerBuilder.forPort(port)
                .addService(new EchoServiceImpl(serverId)).build().start();
        logger.info("[Java EchoServer] Listening on port " + port + " (id: " + serverId + ")");
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try { server.shutdown().awaitTermination(5, TimeUnit.SECONDS); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }));
    }

    public void blockUntilShutdown() throws InterruptedException { server.awaitTermination(); }

    public static void main(String[] args) throws IOException, InterruptedException {
        int port = args.length >= 1 ? Integer.parseInt(args[0]) : 50051;
        String serverId = args.length >= 2 ? args[1] : "java-server";
        new EchoServer(port, serverId).blockUntilShutdown();
    }
}
