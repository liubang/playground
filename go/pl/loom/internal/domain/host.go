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
// Created: 2026/08/17

package domain

import (
	"fmt"
	"net/url"
	"strings"
)

// CanonicalHost validates and canonicalizes a bare hostname: lowercase, no
// scheme/userinfo/path/port, no leading or trailing dot. It is the single
// host-normalization implementation shared by the tool side (building the
// typed URLRequest during Prepare) and the policy side (domain rules,
// session domain memory), so the two can never drift apart.
func CanonicalHost(host string) (string, error) {
	h := strings.ToLower(strings.TrimSpace(host))
	h = strings.TrimSuffix(h, ".")
	if h == "" {
		return "", fmt.Errorf("host is empty")
	}
	if strings.ContainsAny(h, "/:@*") {
		return "", fmt.Errorf("host %q must be a bare hostname (no scheme, path, port, userinfo, or wildcards)", host)
	}
	if strings.HasPrefix(h, ".") || strings.HasSuffix(h, ".") || strings.Contains(h, "..") {
		return "", fmt.Errorf("host %q is not a valid hostname", host)
	}
	return h, nil
}

// HostFromURL extracts the canonical host from a URL string. Only http and
// https URLs with a valid host are eligible; anything else reports ok=false.
func HostFromURL(rawURL string) (string, bool) {
	u, err := url.Parse(strings.TrimSpace(rawURL))
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") {
		return "", false
	}
	host, err := CanonicalHost(u.Hostname())
	if err != nil {
		return "", false
	}
	return host, true
}
