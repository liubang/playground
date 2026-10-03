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
// Created: 2026/10/03

package egress

// ProxyEnv describes the proxy endpoint the seatbelt sandbox injects
// into sandboxed commands' environments. It is deliberately just a
// URL: the process package consumes it without importing the proxy
// machinery.
type ProxyEnv struct {
	// URL is the CONNECT-capable proxy URL with the auth token embedded
	// in the userinfo: http://loom:<token>@127.0.0.1:<port>.
	URL string
}

// noProxy lists targets clients should dial directly rather than via
// the proxy. Private ranges and link-local are included: the sandbox
// denies those direct dials, so they fail closed instead of being
// reflected through the proxy. "*.local" is deliberately absent — the
// sandboxed process has no working resolver, so .local names must ride
// the proxy and be resolved on the host.
const noProxy = "localhost,127.0.0.1,::1,169.254.0.0/16,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"

// EnvVars returns the full proxy environment for a sandboxed command.
// ALL_PROXY/GRPC_PROXY reuse the HTTP URL: the mux serves CONNECT on
// the same port, and a socks5h URL would make Python httpx eagerly
// import socksio at client construction (crashing where it is absent)
// and is not understood by gRPC C-core at all.
func (p *ProxyEnv) EnvVars() []string {
	return []string{
		"HTTP_PROXY=" + p.URL,
		"HTTPS_PROXY=" + p.URL,
		"http_proxy=" + p.URL,
		"https_proxy=" + p.URL,
		"ALL_PROXY=" + p.URL,
		"all_proxy=" + p.URL,
		"GRPC_PROXY=" + p.URL,
		"grpc_proxy=" + p.URL,
		"NO_PROXY=" + noProxy,
		"no_proxy=" + noProxy,
	}
}
