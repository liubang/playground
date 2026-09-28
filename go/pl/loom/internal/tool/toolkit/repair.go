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
// Created: 2026/09/28

package toolkit

import (
	"encoding/json"
	"strings"

	"github.com/kaptinlin/jsonrepair"
)

// RepairJSON salvages a malformed JSON payload with a tolerant repair
// pass (unquoted string values, literal control characters inside
// strings, trailing commas, an unterminated tail — the deviations
// providers stream in tool-call arguments; see agent.stream_hooks.go).
// ok=false means the payload was already valid (callers route those
// through the normal decode path and must not "repair" them) or the
// repair failed to converge. Repaired output is revalidated with
// encoding/json, so an ok result is ordinary strict JSON.
//
// Repair is a guess about intent: callers must gate it on the blast
// radius of a wrong guess (read-only/bookkeeping tools only, never
// writes or process execution — see agent.repairMalformedCallArgs).
func RepairJSON(raw string) (json.RawMessage, bool) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" || json.Valid([]byte(trimmed)) {
		return nil, false
	}
	repaired, err := jsonrepair.Repair(trimmed)
	if err != nil {
		return nil, false
	}
	out := json.RawMessage(repaired)
	if !json.Valid(out) {
		return nil, false
	}
	return out, true
}
