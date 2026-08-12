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
// Created: 2026/07/22 21:10

package domain

import (
	"encoding/json"
	"fmt"
	"time"
)

// Capability represents a permission capability required by a tool.
type Capability string

const (
	CapFSRead          Capability = "fs.read"
	CapFSWrite         Capability = "fs.write"
	CapFSDelete        Capability = "fs.delete"
	CapProcessExec     Capability = "process.exec"
	CapProcessBg       Capability = "process.background"
	CapProcessInteract Capability = "process.interactive"
	CapNetworkConnect  Capability = "network.connect"
	CapGitRead         Capability = "git.read"
	CapGitWrite        Capability = "git.write"
	CapGitRemoteWrite  Capability = "git.remote_write"
	CapSecretUse       Capability = "secret.use"
	CapWorkspaceOut    Capability = "workspace.outside"
	CapAgentDelegate   Capability = "agent.delegate"
	// CapUserInteract marks tools whose purpose is asking the user (never
	// a side effect on the workspace); without it such tools would fall
	// into the R2 default and absurdly require approval to ask a question.
	CapUserInteract Capability = "user.interact"
)

// ToolSource identifies where a tool originates.
type ToolSource string

const (
	ToolSourceBuiltin  ToolSource = "builtin"
	ToolSourceMCP      ToolSource = "mcp"
	ToolSourceSubAgent ToolSource = "subagent"
)

// RiskLevel classifies the risk of a tool operation.
type RiskLevel int

const (
	R0 RiskLevel = iota // pure computation
	R1                  // workspace read + git read
	R2                  // workspace write, normal build/test
	R3                  // delete, outside workspace, network write, interactive
	R4                  // remote git write, deploy, credential, irreversible
)

// ToolStatus represents the result status of a tool execution.
type ToolStatus string

const (
	ToolStatusSuccess   ToolStatus = "success"
	ToolStatusError     ToolStatus = "error"
	ToolStatusTimeout   ToolStatus = "timeout"
	ToolStatusCancelled ToolStatus = "cancelled"
)

// ToolDefinition describes a tool's schema and metadata.
type ToolDefinition struct {
	Name         string          `json:"name"`
	Description  string          `json:"description"`
	InputSchema  json.RawMessage `json:"input_schema"`
	OutputSchema json.RawMessage `json:"output_schema,omitempty"`
	Capabilities []Capability    `json:"capabilities"`
	Source       ToolSource      `json:"source"`
}

// Validate checks the tool definition.
func (d ToolDefinition) Validate() error {
	if d.Name == "" {
		return fmt.Errorf("tool name required")
	}
	if !json.Valid(d.InputSchema) {
		return fmt.Errorf("invalid input_schema JSON")
	}
	if len(d.OutputSchema) > 0 && !json.Valid(d.OutputSchema) {
		return fmt.Errorf("invalid output_schema JSON")
	}
	return nil
}

// Risk returns the maximum risk level across capabilities.
func (d ToolDefinition) Risk() RiskLevel {
	max := R0
	for _, cap := range d.Capabilities {
		if r := capabilityRisk(cap); r > max {
			max = r
		}
	}
	return max
}

func capabilityRisk(c Capability) RiskLevel {
	switch c {
	case CapUserInteract:
		return R0
	case CapFSRead, CapGitRead:
		return R1
	case CapFSWrite, CapProcessExec:
		return R2
	case CapFSDelete, CapWorkspaceOut, CapNetworkConnect, CapProcessBg, CapProcessInteract:
		return R3
	case CapGitRemoteWrite, CapSecretUse, CapAgentDelegate:
		return R4
	default:
		return R2
	}
}

// ToolCall represents a model's invocation of a tool.
type ToolCall struct {
	ID        ToolCallID      `json:"id"`
	Name      string          `json:"name"`
	Arguments json.RawMessage `json:"arguments"`
}

// Validate checks the tool call.
func (c ToolCall) Validate() error {
	if c.ID.IsZero() {
		return fmt.Errorf("tool call ID required")
	}
	if c.Name == "" {
		return fmt.Errorf("tool name required")
	}
	if !json.Valid(c.Arguments) {
		return fmt.Errorf("invalid arguments JSON")
	}
	return nil
}

// ToolError represents an error from tool execution.
type ToolError struct {
	Code      string `json:"code"`
	Message   string `json:"message"`
	Retryable bool   `json:"retryable"`
	LogRef    string `json:"log_ref,omitempty"`
}

// ToolResult metadata keys for externally-metered token usage: a tool
// whose execution consumed model tokens outside the run's own model
// calls (delegate_task's sub-agent run) reports them here, and the
// agent loop folds them into the run's budget counters so delegated
// work stays budget-transparent (docs/SUBAGENT_DESIGN.md §5.2).
const (
	ToolMetaExternalInputTokens  = "external_input_tokens"
	ToolMetaExternalOutputTokens = "external_output_tokens"
)

// ToolResult represents the outcome of a tool execution.
type ToolResult struct {
	CallID     ToolCallID        `json:"call_id"`
	Status     ToolStatus        `json:"status"`
	Content    []ContentPart     `json:"content,omitempty"`
	Error      *ToolError        `json:"error,omitempty"`
	StartedAt  time.Time         `json:"started_at"`
	FinishedAt time.Time         `json:"finished_at"`
	Metadata   map[string]string `json:"metadata,omitempty"`
}

// Validate checks the tool result.
func (r ToolResult) Validate() error {
	if r.CallID.IsZero() {
		return fmt.Errorf("call_id required")
	}
	switch r.Status {
	case ToolStatusSuccess, ToolStatusError, ToolStatusTimeout, ToolStatusCancelled:
	default:
		return fmt.Errorf("invalid tool status %q", r.Status)
	}
	if r.Status == ToolStatusError && r.Error == nil {
		return fmt.Errorf("error status requires ToolError")
	}
	return nil
}

// IsRetryable reports whether a failed tool call can be retried.
func (r ToolResult) IsRetryable() bool {
	return r.Error != nil && r.Error.Retryable
}

// PreparedCall is a validated, risk-assessed tool call ready for approval and execution.
// It is produced by Tool.Prepare and consumed by Tool.Execute.
type PreparedCall struct {
	Call         ToolCall
	Definition   ToolDefinition
	Risk         RiskLevel
	ApprovalDesc string   // human-readable description for approval
	ReadPaths    []string // paths this call will read
	WritePaths   []string // paths this call will write
	ArgsHash     string   // hash of arguments for approval binding
	Recovery     *RecoverySpec
	// Grant carries the execution capabilities the policy layer granted
	// this call (docs/PERMISSION_DESIGN.md). It is assigned by the agent
	// loop AFTER Prepare and HMAC signing (policy evaluation happens
	// between the two), so it is deliberately outside the signed
	// fingerprint: grants are decided by the policy, never by the model.
	// Zero value = the default sandbox.
	Grant ExecGrant `json:"grant,omitempty"`
	// ExecRequest is the typed execution contract of process-spawning
	// tools (run_cmd, exec_session). The producing tool fills it during
	// Prepare and covers it by its signature, so the policy layer
	// classifies a typed field instead of re-parsing tool-specific
	// argument JSON (REVIEW M17/A2): any tool that sets it is eligible
	// for argv-prefix rules, the danger screen, and session memory
	// without the policy layer knowing its name.
	ExecRequest *ExecRequest `json:"exec_request,omitempty"`
	// URLRequest is the typed URL contract of URL-fetching tools
	// (web_fetch, browser navigate). The producing tool fills it during
	// Prepare and covers it by its signature, so the policy layer
	// classifies a typed field instead of re-parsing tool-specific
	// argument JSON (docs/BROWSER_DESIGN.md §5.3): any tool that sets
	// it is eligible for domain rules, session domain memory, and the
	// URL baseline without the policy layer knowing its name.
	URLRequest *URLRequest `json:"url_request,omitempty"`
	// WriteRequest is the typed write contract of file-writing tools
	// (write, edit). The producing tool fills it during Prepare and
	// covers it by its signature, so the policy layer classifies a typed
	// field instead of re-parsing tool-specific argument JSON: any tool
	// that sets it is eligible for writable-path rules, session path
	// memory, and the boundary-crossing write baseline without the
	// policy layer knowing its name.
	WriteRequest *WriteRequest `json:"write_request,omitempty"`
}

// URLRequest describes a URL the policy layer can classify uniformly,
// independent of which tool produced it. It is the URL counterpart of
// ExecRequest: the producing tool extracts the canonical host from its
// arguments during Prepare and signs it, so domain rule evaluation and
// session domain memory match the typed field rather than tool-name +
// ad-hoc argument parsing (docs/BROWSER_DESIGN.md §5.3).
type URLRequest struct {
	// Host is the canonical hostname (lowercase, no port) of the URL.
	Host string `json:"host"`
}

// WriteRequest describes a file write the policy layer can classify
// uniformly, independent of which tool produced it. It is the write
// counterpart of ExecRequest: the producing tool resolves its target
// during Prepare and signs the outcome, so writable-path rule evaluation
// and session path memory match the typed field rather than tool-name +
// ad-hoc argument parsing.
type WriteRequest struct {
	// Path is the canonical absolute write target.
	Path string `json:"path"`
	// OutsideRoots marks a target outside the workspace + scratch roots:
	// the write crosses the confinement boundary, so policy (writable-path
	// rules, session path memory, per-mode baseline) — not the path
	// validator — is the gate. The flag is explicit (rather than nil
	// meaning confined) so the policy layer can distinguish "confined
	// write" from "produced before this contract existed".
	OutsideRoots bool `json:"outside_roots,omitempty"`
}

// ExecRequest describes a process execution the policy layer can
// classify uniformly, independent of which tool produced it.
type ExecRequest struct {
	// Argv is [program, ...args] as executed (before shell unwrapping).
	Argv []string `json:"argv"`
	// Escalated marks a request to run outside the sandbox.
	Escalated bool `json:"escalated,omitempty"`
	// NeedsNetwork marks a declared need for outbound network inside the
	// sandbox.
	NeedsNetwork bool `json:"needs_network,omitempty"`
	// NeedsGUIOpen marks a declared need to drive macOS GUI applications
	// (open / Apple Events) from inside the sandbox
	// (docs/BROWSER_DESIGN.md §4).
	NeedsGUIOpen bool `json:"needs_gui_open,omitempty"`
}

// RecoverySpec describes durable evidence that can reconcile an interrupted
// operation without replaying it. It must not contain secrets or raw arguments.
type RecoverySpec struct {
	Kind         string `json:"kind"`
	Path         string `json:"path,omitempty"`
	ExpectedHash string `json:"expected_hash,omitempty"`
	ResultHash   string `json:"result_hash,omitempty"`
	// BeforeContent holds the file content before mutation, for checkpoint
	// rewind support. It is not JSON-serialized (too large for the event
	// stream) and is populated only by file-writing tools (edit, write)
	// during Prepare, then consumed by the agent loop's recordToolOutcome.
	BeforeContent []byte `json:"-"`
}
