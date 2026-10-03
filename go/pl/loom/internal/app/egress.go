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

package app

import (
	"fmt"
	"log/slog"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process/egress"
)

// egressPolicy adapts the workspace's PackageSet host rules to the
// egress proxy's Policy interface. The zero Policy decision for an
// unmatched destination comes from the
// configured posture; globalOnly selects the SOCKS5 conservative
// evaluation (global-scope rules only, unmatched never allows).
type egressPolicy struct {
	packages       *permission.PackageSet
	workspace      string
	unmatchedAllow bool
	globalOnly     bool
}

// Decide implements egress.Policy.
func (p egressPolicy) Decide(host string, _ int) egress.Decision {
	allow, pkg, literals, matched := p.packages.DecideEgress(host, p.workspace, p.globalOnly)
	if matched {
		return egress.Decision{
			Allow:          allow,
			Matched:        true,
			Reason:         pkg.Justification,
			Rule:           pkg.Scope.String(),
			ExemptLiterals: literals,
		}
	}
	if p.globalOnly || !p.unmatchedAllow {
		return egress.Decision{
			Allow:  false,
			Reason: "no host rule covers this destination",
			Rule:   "unmatched-deny",
		}
	}
	return egress.Decision{
		Allow:  true,
		Reason: "no host rule covers this destination",
		Rule:   "unmatched-allow",
	}
}

// egressLogger bridges connection records to the process logger. Every
// attempt — allowed or not — is recorded: connection-level visibility
// is the egress proxy's core payoff.
type egressLogger struct {
	logger *slog.Logger
}

// LogConn implements egress.Logger.
func (l egressLogger) LogConn(rec egress.ConnRecord) {
	attrs := []any{
		"proto", rec.Proto,
		"host", rec.Host,
		"port", rec.Port,
		"allow", rec.Decision.Allow,
		"rule", rec.Decision.Rule,
	}
	if rec.Decision.Reason != "" {
		attrs = append(attrs, "reason", rec.Decision.Reason)
	}
	if rec.Dialed != "" {
		attrs = append(attrs, "dialed", rec.Dialed)
	}
	if rec.Err != "" {
		attrs = append(attrs, "error", rec.Err)
	}
	l.logger.Info("egress connection", attrs...)
}

// startEgressProxy starts the workspace's egress proxy when the
// resolved config selects the proxy network posture. It returns the
// server (nil for the off/full postures) and the ProxyEnv to wire into
// the platform sandbox. A start failure is a hard bootstrap error:
// silently falling back to the no-network sandbox would fail every
// networked command with a message pointing nowhere.
func startEgressProxy(resolved *config.ResolvedConfig, packages *permission.PackageSet, workspaceRoot string, logger *slog.Logger) (*egress.Server, *egress.ProxyEnv, error) {
	if resolved.Sandbox.Network != config.SandboxNetworkProxy {
		return nil, nil, nil
	}
	server, err := egress.NewServer(egress.Config{
		Policy: egressPolicy{
			packages:       packages,
			workspace:      workspaceRoot,
			unmatchedAllow: resolved.Sandbox.ProxyUnmatchedAllow,
		},
		// SOCKS5 arrives unauthenticated: global rules only, and the
		// unmatched posture never applies.
		SOCKSPolicy: egressPolicy{
			packages:   packages,
			workspace:  workspaceRoot,
			globalOnly: true,
		},
		Logger: egressLogger{logger: logger.With("component", "egress")},
	})
	if err != nil {
		return nil, nil, fmt.Errorf("start egress proxy: %w", err)
	}
	logger.Info("egress proxy started",
		"addr", server.Addr(), "unmatched_allow", resolved.Sandbox.ProxyUnmatchedAllow)
	return server, &egress.ProxyEnv{URL: server.ProxyURL()}, nil
}
