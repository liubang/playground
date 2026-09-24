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

import Foundation

// MARK: - RuntimeEvent envelope (internal/runtimeevent/event.go)

/// The SSE `data:` payload — one RuntimeEvent per frame. `payload` stays
/// schema-less at the envelope level and is materialized per kind by the
/// store layer, keeping the client forward-compatible with new kinds and
/// new optional payload fields (SERVE_DESIGN §5.5).
struct RuntimeEvent: Decodable, Sendable {
    let version: Int
    let sequence: UInt64
    let sessionId: String
    let runId: String?
    let turn: Int?
    let kind: Kind
    let durable: Bool
    let payload: JSONValue?

    enum CodingKeys: String, CodingKey {
        case version, sequence
        case sessionId = "session_id"
        case runId = "run_id"
        case turn, kind, durable, payload
    }

    enum Kind: String, Decodable, Sendable {
        case sessionOpened = "session.opened"
        case sessionClosed = "session.closed"
        case turnStarted = "turn.started"
        case turnFinished = "turn.finished"
        case runPhaseChanged = "run.phase_changed"
        case runCancelRequested = "run.cancel_requested"
        case runCancelled = "run.cancelled"
        case runCompleted = "run.completed"
        case modelRequestStarted = "model.request_started"
        case modelTextDelta = "model.text_delta"
        case modelReasoningDelta = "model.reasoning_delta"
        case modelToolCallDelta = "model.tool_call_delta"
        case modelResponseCompleted = "model.response_completed"
        case modelRequestFailed = "model.request_failed"
        case modelRequestRetrying = "model.request_retrying"
        case approvalRequested = "approval.requested"
        case approvalResolved = "approval.resolved"
        case questionAsked = "question.asked"
        case questionAnswered = "question.answered"
        case toolPrepared = "tool.prepared"
        case toolStarted = "tool.started"
        case toolCompleted = "tool.completed"
        case toolProgress = "tool.progress"
        case budgetUpdated = "budget.updated"
        case budgetNotice = "budget.notice"
        case contextCompacted = "context.compacted"
        case usageUpdated = "usage.updated"
        case contextUsage = "context.usage"
        case planUpdated = "plan.updated"
        case steerQueued = "steer.queued"
        case steerInjected = "steer.injected"
        case subagentStarted = "subagent.started"
        case subagentProgress = "subagent.progress"
        case subagentFinished = "subagent.finished"
        case runtimeWarning = "runtime.warning"
        case runtimeFatal = "runtime.fatal"
        case unknown

        init(from decoder: Decoder) throws {
            let raw = try decoder.singleValueContainer().decode(String.self)
            self = Kind(rawValue: raw) ?? .unknown
        }
    }
}

// MARK: - Per-kind payloads (json tags from event.go)

struct TurnStartedPayload: Decodable, Sendable {
    let turnIndex: Int?
    let prompt: String?

    enum CodingKeys: String, CodingKey {
        case turnIndex = "turn_index"
        case prompt
    }
}

struct RunPhaseChangedPayload: Decodable, Sendable {
    let phase: RunPhase

    enum RunPhase: String, Decodable, Sendable {
        case preparing
        case callingModel = "calling_model"
        case awaitingApproval = "awaiting_approval"
        case executingTools = "executing_tools"
        case compacting
        case unknown

        init(from decoder: Decoder) throws {
            let raw = try decoder.singleValueContainer().decode(String.self)
            self = RunPhase(rawValue: raw) ?? .unknown
        }
    }
}

struct ModelRequestStartedPayload: Decodable, Sendable {
    let requestId: String
    let modelName: String?
    let turn: Int?

    enum CodingKeys: String, CodingKey {
        case requestId = "request_id"
        case modelName = "model_name"
        case turn
    }
}

struct ModelDeltaPayload: Decodable, Sendable {
    let requestId: String
    let delta: String

    enum CodingKeys: String, CodingKey {
        case requestId = "request_id"
        case delta
    }
}

struct ModelToolCallDeltaPayload: Decodable, Sendable {
    let requestId: String
    let toolIndex: Int
    let toolName: String?
    let toolId: String?
    let arguments: String?

    enum CodingKeys: String, CodingKey {
        case requestId = "request_id"
        case toolIndex = "tool_index"
        case toolName = "tool_name"
        case toolId = "tool_id"
        case arguments
    }
}

struct ModelResponseCompletedPayload: Decodable, Sendable {
    let requestId: String
    let stopReason: String?
    let inputTokens: Int64?
    let outputTokens: Int64?
    let hasToolCalls: Bool
    let text: String?

    enum CodingKeys: String, CodingKey {
        case requestId = "request_id"
        case stopReason = "stop_reason"
        case inputTokens = "input_tokens"
        case outputTokens = "output_tokens"
        case hasToolCalls = "has_tool_calls"
        case text
    }
}

struct ApprovalRequestedPayload: Decodable, Sendable {
    let approvalId: String
    let callId: String
    let toolName: String
    let source: String?
    let risk: Int
    let description: String
    let argsHash: String
    let readPaths: [String]?
    let writePaths: [String]?
    let arguments: JSONValue?
    /// The operation's derived target (command/path); always shown in
    /// the mono cmd block when present — if it repeats the description,
    /// the prose copy is suppressed instead (ApprovalCard).
    let target: String?
    /// Human-readable consequence of the operation ("will do" row).
    let consequence: String?
    /// Rule preview for "Always allow"; empty means the call cannot be
    /// remembered, so the button must be hidden (WebUI cards.tsx).
    let rulePreview: String?
    /// Trust preview for "Trust (no sandbox)".
    let trustPreview: String?

    enum CodingKeys: String, CodingKey {
        case approvalId = "approval_id"
        case callId = "call_id"
        case toolName = "tool_name"
        case source, risk, description
        case argsHash = "args_hash"
        case readPaths = "read_paths"
        case writePaths = "write_paths"
        case arguments
        case target, consequence
        case rulePreview = "rule_preview"
        case trustPreview = "trust_preview"
    }
}

struct ApprovalResolvedPayload: Decodable, Sendable {
    let approvalId: String
    let decision: String

    enum CodingKeys: String, CodingKey {
        case approvalId = "approval_id"
        case decision
    }
}

struct QuestionAskedPayload: Decodable, Sendable {
    let questionId: String
    let text: String
    let options: [Option]
    let allowMultiple: Bool?

    struct Option: Decodable, Sendable, Hashable {
        let label: String
        let description: String?
    }

    enum CodingKeys: String, CodingKey {
        case questionId = "question_id"
        case text, options
        case allowMultiple = "allow_multiple"
    }
}

struct QuestionAnsweredPayload: Decodable, Sendable {
    let questionId: String

    enum CodingKeys: String, CodingKey {
        case questionId = "question_id"
    }
}

struct ToolPreparedPayload: Decodable, Sendable {
    let callId: String
    let toolName: String
    let risk: Int?
    let target: String?
    let diff: String?

    enum CodingKeys: String, CodingKey {
        case callId = "call_id"
        case toolName = "tool_name"
        case risk, target, diff
    }
}

struct ToolStartedPayload: Decodable, Sendable {
    let callId: String
    let toolName: String

    enum CodingKeys: String, CodingKey {
        case callId = "call_id"
        case toolName = "tool_name"
    }
}

struct ToolCompletedPayload: Decodable, Sendable {
    let callId: String
    let toolName: String
    let status: Status
    let durationMs: Int64?
    let errorMessage: String?
    let preview: String?
    /// Artifact references found in the tool result (runtimeevent
    /// ToolCompletedPayload.Artifacts), for live rendering without
    /// waiting for a snapshot rebuild. The server already excludes
    /// model_only (view_image) artifacts (controller.toolResultPreview).
    let artifacts: [ContentPart.Artifact]?

    enum Status: String, Decodable, Sendable {
        case success, error, timeout, cancelled, unknown

        init(from decoder: Decoder) throws {
            let raw = try decoder.singleValueContainer().decode(String.self)
            self = Status(rawValue: raw) ?? .unknown
        }
    }

    enum CodingKeys: String, CodingKey {
        case callId = "call_id"
        case toolName = "tool_name"
        case status
        case durationMs = "duration_ms"
        case errorMessage = "error_message"
        case preview
        case artifacts
    }
}

struct BudgetUpdatedPayload: Decodable, Sendable {
    let turns: Int?
    let inputTokens: Int64?
    let outputTokens: Int64?
    let toolCalls: Int?
    let cachedInputTokens: Int64?
    let contextTokens: Int64?

    enum CodingKeys: String, CodingKey {
        case turns
        case inputTokens = "input_tokens"
        case outputTokens = "output_tokens"
        case toolCalls = "tool_calls"
        case cachedInputTokens = "cached_input_tokens"
        case contextTokens = "context_tokens"
    }
}

struct BudgetNoticePayload: Decodable, Sendable {
    let text: String
    let level: String?
}

struct ContextUsagePayload: Decodable, Sendable {
    let occupancyTokens: Int64?

    enum CodingKeys: String, CodingKey {
        case occupancyTokens = "occupancy_tokens"
    }
}

struct ContextCompactedPayload: Decodable, Sendable {
    let estTokensBefore: Int64?
    let estTokensAfter: Int64?

    enum CodingKeys: String, CodingKey {
        case estTokensBefore = "est_tokens_before"
        case estTokensAfter = "est_tokens_after"
    }
}

struct SteerQueuedPayload: Decodable, Sendable {
    let text: String
    let queueLen: Int?
    let queue: String?

    enum CodingKeys: String, CodingKey {
        case text
        case queueLen = "queue_len"
        case queue
    }
}

struct SteerInjectedPayload: Decodable, Sendable {
    let text: String
}

struct PlanPayload: Decodable, Sendable {
    let title: String?
    let items: [Item]

    struct Item: Decodable, Sendable, Identifiable {
        let index: Int
        let goal: String
        let status: String
        /// Completion evidence lines (domain todo item) — an ARRAY of
        /// strings; decoding it as a single String used to fail the
        /// whole snapshot with a typeMismatch.
        let evidence: [String]?

        var id: Int {
            index
        }
    }
}

struct SubagentStartedPayload: Decodable, Sendable {
    let callId: String
    let childSessionId: String
    let task: String

    enum CodingKeys: String, CodingKey {
        case callId = "call_id"
        case childSessionId = "child_session_id"
        case task
    }
}

struct SubagentFinishedPayload: Decodable, Sendable {
    let callId: String
    let childSessionId: String
    let outcome: String

    enum CodingKeys: String, CodingKey {
        case callId = "call_id"
        case childSessionId = "child_session_id"
        case outcome
    }
}

struct RuntimeMessagePayload: Decodable, Sendable {
    let message: String
}
