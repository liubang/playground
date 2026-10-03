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

import (
	"errors"
	"fmt"
	"net/netip"
	"strconv"
	"strings"
)

// errBadAuthority rejects malformed CONNECT authorities / absolute-URI
// hosts with a 400.
var errBadAuthority = errors.New("invalid target authority")

// parseAuthority splits a CONNECT authority or absolute-URI host into a
// normalized host and port. It rejects userinfo, paths, IPv6 zones,
// empty hosts, and ports outside 1-65535 — anything a client could use
// to make the policy layer and the dialer disagree about the target.
func parseAuthority(authority string, defaultPort int) (string, int, error) {
	if authority == "" {
		return "", 0, errBadAuthority
	}
	if strings.ContainsAny(authority, "@/?# \t\r\n\x00") {
		return "", 0, errBadAuthority
	}
	host, portStr, err := splitHostPort(authority)
	if err != nil {
		return "", 0, err
	}
	port := defaultPort
	if portStr != "" {
		port, err = parsePort(portStr)
		if err != nil {
			return "", 0, err
		}
	}
	host = normalizeHost(host)
	if err := validateHost(host); err != nil {
		return "", 0, err
	}
	return host, port, nil
}

// splitHostPort separates host and port, honouring bracketed IPv6
// ([::1]:443). A bare multi-colon string is accepted only if it parses
// as an IPv6 literal (then no port is split).
func splitHostPort(authority string) (host, port string, err error) {
	if strings.HasPrefix(authority, "[") {
		close := strings.IndexByte(authority, ']')
		if close == -1 {
			return "", "", errBadAuthority
		}
		host = authority[1:close]
		if strings.Contains(host, "%") {
			return "", "", errBadAuthority // IPv6 zones refused up front
		}
		rest := authority[close+1:]
		switch {
		case rest == "":
			return host, "", nil
		case strings.HasPrefix(rest, ":"):
			return host, rest[1:], nil
		default:
			return "", "", errBadAuthority
		}
	}
	colons := strings.Count(authority, ":")
	switch {
	case colons == 0:
		return authority, "", nil
	case colons == 1:
		idx := strings.LastIndexByte(authority, ':')
		return authority[:idx], authority[idx+1:], nil
	default:
		// Multi-colon without brackets: only acceptable as a bare IPv6
		// literal (never split a trailing hextet into a "port").
		if _, parseErr := netip.ParseAddr(authority); parseErr != nil {
			return "", "", errBadAuthority
		}
		if strings.Contains(authority, "%") {
			return "", "", errBadAuthority
		}
		return authority, "", nil
	}
}

func parsePort(s string) (int, error) {
	port, err := strconv.Atoi(s)
	if err != nil || port < 1 || port > 65535 {
		return 0, errBadAuthority
	}
	return port, nil
}

// normalizeHost canonicalizes the spelling every downstream consumer
// (policy, guard, dial, log record) shares: lowercased, trailing dot
// stripped.
func normalizeHost(host string) string {
	host = strings.TrimSpace(host)
	host = strings.TrimSuffix(host, ".")
	return strings.ToLower(host)
}

// validateHost rejects hosts that could corrupt logs or resolvers:
// empty, or carrying control characters (SOCKS5 DOMAINNAME arrives as a
// length-prefixed raw byte string with zero protocol validation).
func validateHost(host string) error {
	if host == "" {
		return errBadAuthority
	}
	for _, r := range host {
		if r < 0x20 || r == 0x7f {
			return fmt.Errorf("%w: control character in host", errBadAuthority)
		}
	}
	return nil
}

// isLoopbackName reports RFC 6761 loopback names.
func isLoopbackName(host string) bool {
	return host == "localhost" || strings.HasSuffix(host, ".localhost")
}
