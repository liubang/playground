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

package subagent

import (
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// signPreparedCall signs a prepared sub-agent call using the toolkit Signer.
// The fingerprint binds the call ID, tool name, canonical arguments, and the
// approved risk tier, so a prepared call cannot be re-purposed between
// Prepare and Execute (REVIEW H10).
func signPreparedCall(signer *toolkit.Signer, prepared domain.PreparedCall) string {
	return signer.Sign(prepared)
}

// verifyPreparedCall re-checks a prepared call at Execute time: tool name,
// source, and risk must match the live definition, and the HMAC must
// verify against this tool instance's key.
func verifyPreparedCall(signer *toolkit.Signer, def domain.ToolDefinition, risk domain.RiskLevel, prepared domain.PreparedCall) error {
	if prepared.Call.Name != def.Name || prepared.Definition.Name != def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if prepared.Definition.Source != def.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call source mismatch")
	}
	if prepared.Risk != risk {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	expected := signPreparedCall(signer, prepared)
	if !signer.VerifyRaw(expected, prepared.ArgsHash) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}
