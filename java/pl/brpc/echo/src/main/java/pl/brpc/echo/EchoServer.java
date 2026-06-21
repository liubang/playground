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

package pl.brpc.echo;

import com.baidu.cloud.starlight.api.rpc.StarlightServer;
import com.baidu.cloud.starlight.api.rpc.config.ServiceConfig;
import com.baidu.cloud.starlight.api.rpc.config.TransportConfig;
import com.baidu.cloud.starlight.core.rpc.DefaultStarlightServer;

import pl.grpc.proto.EchoRequest;
import pl.grpc.proto.EchoResponse;
import pl.grpc.proto.HealthRequest;
import pl.grpc.proto.HealthResponse;

/**
 * Starlight echo server — implements the 2 unary RPCs (Echo + HealthCheck)
 * using the same proto message types as the C++ brpc server.
 * <p>
 * Starlight exposes both brpc binary protocol and Spring MVC REST on the
 * same port. This server is configured programmatically (non-Spring).
 */
public class EchoServer {

    /** The service implementation. */
    public static class EchoServiceImpl implements EchoService {
        private final long startTimeMs = System.currentTimeMillis();

        @Override
        public EchoResponse Echo(EchoRequest request) {
            return EchoResponse.newBuilder()
                    .setMessage(request.getMessage())
                    .setOriginalTimestamp(request.getTimestampUs())
                    .setServerTimestamp(System.currentTimeMillis() * 1000)
                    .setServerId("java-starlight-server")
                    .build();
        }

        @Override
        public HealthResponse HealthCheck(HealthRequest request) {
            long uptimeSec = (System.currentTimeMillis() - startTimeMs) / 1000;
            return HealthResponse.newBuilder()
                    .setStatus(HealthResponse.Status.SERVING)
                    .setServerId("java-starlight-server")
                    .setVersion("1.0.0")
                    .setUptimeSeconds(uptimeSec)
                    .build();
        }
    }

    public static void main(String[] args) {
        String host = args.length > 0 ? args[0] : "localhost";
        int port = args.length > 1 ? Integer.parseInt(args[1]) : 8005;

        TransportConfig transportConfig = new TransportConfig();
        StarlightServer server = new DefaultStarlightServer(host, port, transportConfig);
        server.init();

        ServiceConfig serviceConfig = new ServiceConfig();
        server.export(EchoService.class, new EchoServiceImpl(), serviceConfig);
        server.serve();

        System.out.println("[Java Starlight EchoServer] Listening on " + host + ":"
                + port + " (server_id: java-starlight-server)");

        synchronized (EchoServer.class) {
            try {
                EchoServer.class.wait();
            } catch (InterruptedException ignored) {
            }
        }
    }
}
