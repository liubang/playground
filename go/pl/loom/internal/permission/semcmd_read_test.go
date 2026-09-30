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
// Created: 2026/09/30

package permission

import (
	"strings"
	"testing"
)

func hasIndicatorLike(e Effect, want string) bool {
	for _, ind := range e.Indicators {
		if strings.Contains(ind, want) {
			return true
		}
	}
	return false
}

// Ordinary viewer invocations are proven-confined and carry no indicator:
// the deriver upgrades them from "unrecognized program" without changing
// the verdict.
func TestSemDeriveFileReadConfined(t *testing.T) {
	for _, argv := range [][]string{
		{"cat", "README.md"},
		{"cat", "-n", "main.go"},
		{"head", "-n", "5", "main.go"},
		{"tail", "-f", "app.log"},
		{"nl", "-ba", "main.go"},
		{"od", "-c", "bin.dat"},
		{"cat", ".git/HEAD"}, // repository metadata is not a read-side concern
		{"cat", "/ws/.git/config"},
		{"cat", "src/.env.example"},
	} {
		d := deriveExec(argv)
		if !d.Effect.Proven {
			t.Fatalf("%v: not proven (%s)", argv, d.Effect.Reason)
		}
		if d.Effect.Consequence != ConsequenceConfined {
			t.Fatalf("%v: consequence = %v, want confined", argv, d.Effect.Consequence)
		}
		if len(d.Effect.Indicators) != 0 {
			t.Fatalf("%v: unexpected indicators %v", argv, d.Effect.Indicators)
		}
	}
}

// A viewer targeting credential material must carry the read-side
// indicator so the attempt is surfaced and can never be categorically
// remembered.
func TestSemDeriveFileReadCredentialIndicator(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	for _, argv := range [][]string{
		{"cat", ".env"},
		{"cat", "config/credentials.json"},
		{"cat", ".ssh/known_hosts"},
		{"head", "-n", "5", home + "/.ssh/id_rsa"},
		{"tail", "~/.aws/credentials"},
		{"od", "-c", "service-account.json"},
	} {
		d := deriveExec(argv)
		if !hasIndicatorLike(d.Effect, "credential path") {
			t.Fatalf("%v: no credential indicator (effect %+v)", argv, d.Effect)
		}
	}
}

// Unknown option forms fail closed to unprovable — the deriver never
// guesses a grammar it does not fully understand.
func TestSemDeriveFileReadUnknownFlagUnprovable(t *testing.T) {
	d := deriveExec([]string{"cat", "--definitely-not-a-flag", "x"})
	if d.Effect.Proven {
		t.Fatal("unknown cat flag must degrade to unprovable")
	}
}
