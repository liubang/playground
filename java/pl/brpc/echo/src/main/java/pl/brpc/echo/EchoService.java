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

import pl.grpc.proto.EchoRequest;
import pl.grpc.proto.EchoResponse;
import pl.grpc.proto.HealthRequest;
import pl.grpc.proto.HealthResponse;

/**
 * Java interface for the EchoService, consumed by Starlight.
 * Method names MUST match the proto RPC names exactly for brpc cross-language interop.
 */
public interface EchoService {
    EchoResponse Echo(EchoRequest request);
    HealthResponse HealthCheck(HealthRequest request);
}
