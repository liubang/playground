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

import com.baidu.cloud.starlight.api.rpc.StarlightClient;
import com.baidu.cloud.starlight.api.rpc.config.ServiceConfig;
import com.baidu.cloud.starlight.api.rpc.config.TransportConfig;
import com.baidu.cloud.starlight.core.rpc.SingleStarlightClient;
import com.baidu.cloud.starlight.core.rpc.proxy.JDKProxyFactory;

import pl.grpc.proto.EchoRequest;
import pl.grpc.proto.EchoResponse;
import pl.grpc.proto.HealthRequest;
import pl.grpc.proto.HealthResponse;

/**
 * Starlight echo client — calls the 2 unary RPCs (Echo + HealthCheck)
 * over the brpc binary protocol.
 * <p>
 * Can talk to either a Java Starlight server or a C++ brpc server.
 */
public class EchoClient {

    public static void main(String[] args) {
        String host = args.length > 0 ? args[0] : "localhost";
        int port = args.length > 1 ? Integer.parseInt(args[1]) : 8005;

        TransportConfig transportConfig = new TransportConfig();
        StarlightClient client = new SingleStarlightClient(host, port, transportConfig);
        client.init();

        ServiceConfig serviceConfig = new ServiceConfig();
        serviceConfig.setProtocol("brpc");
        // For cross-language interop with C++ brpc, set the proto service name.
        // When talking to a Java Starlight server, javaInterface.getName() is used by default.
        // brpc service FQN: pl.brpc.echo.EchoService
        serviceConfig.setServiceId("pl.brpc.echo.EchoService");
        serviceConfig.setSerializeMode("pb2-std"); // standard protobuf for C++ compat

        JDKProxyFactory proxyFactory = new JDKProxyFactory();
        EchoService echoService = proxyFactory.getProxy(
                EchoService.class, serviceConfig, client);

        try {
            // --- HealthCheck ---
            System.out.println("============ HealthCheck ============");
            HealthRequest healthRequest = HealthRequest.newBuilder().build();
            HealthResponse healthResponse = echoService.HealthCheck(healthRequest);
            System.out.println("[HealthCheck] status="
                    + healthResponse.getStatus()
                    + " | server=" + healthResponse.getServerId()
                    + " | version=" + healthResponse.getVersion()
                    + " | uptime=" + healthResponse.getUptimeSeconds() + "s");

            // --- Unary Echo ---
            System.out.println("============ Unary Echo =============");
            long sentUs = System.currentTimeMillis() * 1000;
            EchoRequest echoRequest = EchoRequest.newBuilder()
                    .setMessage("Hello from Java Starlight client!")
                    .setTimestampUs(sentUs)
                    .putHeaders("client", "java-starlight")
                    .build();
            EchoResponse echoResponse = echoService.Echo(echoRequest);
            long rttUs = System.currentTimeMillis() * 1000 - sentUs;
            System.out.println("[Echo] response: " + echoResponse.getMessage()
                    + " | rtt_us=" + rttUs
                    + " | server=" + echoResponse.getServerId());

        } catch (Exception e) {
            System.err.println("[EchoClient] RPC failed: " + e.getMessage());
            e.printStackTrace();
        } finally {
            client.destroy();
        }
    }
}
