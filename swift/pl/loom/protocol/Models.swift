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

// MARK: - Shared JSON plumbing

/// Go's time.Time marshals as RFC3339, sometimes with nanoseconds — the
/// system ISO8601 formatter rejects fractional digits, so parse by hand.
enum LoomJSON {
    static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .custom { decoder in
            let container = try decoder.singleValueContainer()
            let raw = try container.decode(String.self)
            if let date = LoomJSON.parseRFC3339(raw) {
                return date
            }
            throw DecodingError.dataCorruptedError(
                in: container, debugDescription: "invalid RFC3339 date: \(raw)",
            )
        }
        return d
    }()

    static let encoder = JSONEncoder()

    private static let withFraction: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()

    private static let plain: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f
    }()

    static func parseRFC3339(_ raw: String) -> Date? {
        withFraction.date(from: raw) ?? plain.date(from: raw)
    }
}

// MARK: - Error model (internal/server/errors.go)

struct APIErrorBody: Decodable, Sendable {
    struct Detail: Decodable, Sendable {
        let code: String
        let message: String
        let state: String?
    }

    let error: Detail
}

enum LoomAPIError: Error, LocalizedError, Sendable {
    case http(status: Int, code: String?, message: String?)
    case transport(String)
    case decoding(String)

    var errorDescription: String? {
        switch self {
        case let .http(status, code, message):
            if let code, let message {
                return "\(code) (\(status)): \(message)"
            }
            return "HTTP \(status)"
        case let .transport(m): return m
        case let .decoding(m): return m
        }
    }
}

// MARK: - Meta

struct MetaVersion: Decodable, Sendable {
    let protocolField: Int
    let version: String
    let instance: String?

    enum CodingKeys: String, CodingKey {
        case protocolField = "protocol"
        case version, instance
    }
}

struct MetaModels: Decodable, Sendable {
    let models: [ModelInfo]
    let `default`: String?

    struct ModelInfo: Decodable, Sendable, Hashable {
        let provider: String
        let name: String
        let contextWindow: Int?

        enum CodingKeys: String, CodingKey {
            case provider, name
            case contextWindow = "context_window"
        }
    }
}

// MARK: - Sessions

struct SessionSummary: Decodable, Sendable, Identifiable, Hashable {
    let id: String
    let createdAt: Date?
    let updatedAt: Date?
    let workspaceId: String?
    let state: String?
    let modelName: String?
    let turnCount: Int?
    let title: String?
    let parentSessionId: String?

    enum CodingKeys: String, CodingKey {
        case id
        case createdAt = "created_at"
        case updatedAt = "updated_at"
        case workspaceId = "workspace_id"
        case state
        case modelName = "model_name"
        case turnCount = "turn_count"
        case title
        case parentSessionId = "parent_session_id"
    }
}

struct SessionListResponse: Decodable, Sendable {
    let sessions: [SessionSummary]
    let nextCursor: String?

    enum CodingKeys: String, CodingKey {
        case sessions
        case nextCursor = "next_cursor"
    }
}

struct CreateSessionResponse: Decodable, Sendable {
    let sessionId: String
    let state: String
    let workspaceId: String?

    enum CodingKeys: String, CodingKey {
        case sessionId = "session_id"
        case state
        case workspaceId = "workspace_id"
    }
}

// MARK: - Message projection (internal/domain/message.go)

enum MessageRole: String, Decodable, Sendable {
    case system, user, assistant, unknown

    init(from decoder: Decoder) throws {
        let raw = try decoder.singleValueContainer().decode(String.self)
        self = MessageRole(rawValue: raw) ?? .unknown
    }
}

enum MessageStatus: String, Decodable, Sendable {
    case draft, final, interrupted, unknown

    init(from decoder: Decoder) throws {
        let raw = try decoder.singleValueContainer().decode(String.self)
        self = MessageStatus(rawValue: raw) ?? .unknown
    }
}

/// ContentPart tagged union (message.go:132-151): discriminator `kind`,
/// the remaining fields are per-kind optionals.
enum ContentPart: Decodable, Sendable, Equatable {
    case text(String)
    case reasoning(Reasoning)
    case toolCall(ToolCall)
    case toolResult(ToolResult)
    case artifact(Artifact)
    case image(ImageContent)
    case unknown

    struct Reasoning: Decodable, Sendable, Equatable {
        let text: String?
        let redacted: Bool?
        let durationMs: Int64?

        enum CodingKeys: String, CodingKey {
            case text, redacted
            case durationMs = "duration_ms"
        }
    }

    struct ToolCall: Decodable, Sendable, Equatable {
        let id: String
        let name: String
        let arguments: JSONValue?
    }

    struct ToolResult: Decodable, Sendable, Equatable {
        let callId: String
        let status: String
        let content: [ContentPart]?
        let error: ResultError?
        /// Wall-clock span (domain.ToolResult started_at/finished_at) —
        /// the WebUI's histCompletion derives duration_ms from these.
        let startedAt: Date?
        let finishedAt: Date?

        struct ResultError: Decodable, Sendable, Equatable {
            let code: String
            let message: String
        }

        /// Execution wall time in milliseconds (WebUI histCompletion).
        var durationMs: Int64? {
            guard let startedAt, let finishedAt else { return nil }
            let ms = Int64(finishedAt.timeIntervalSince(startedAt) * 1000)
            return ms >= 0 ? ms : nil
        }

        enum CodingKeys: String, CodingKey {
            case callId = "call_id"
            case status, content, error
            case startedAt = "started_at"
            case finishedAt = "finished_at"
        }
    }

    struct Artifact: Decodable, Sendable, Equatable {
        let id: String
        let size: Int64?
        let mediaType: String?
        /// Part-level display flags (domain/message.go: ModelOnly /
        /// PresentOnly). They live on the part object — siblings of the
        /// nested `artifact` payload — so ContentPart's decoder injects
        /// them after decoding the ref. view_image marks its artifact
        /// model_only (display channels must NOT render it);
        /// present_image marks it present_only (display-only).
        var modelOnly = false
        var presentOnly = false

        enum CodingKeys: String, CodingKey {
            case id, size
            case mediaType = "media_type"
        }
    }

    struct ImageContent: Decodable, Sendable, Equatable {
        let mediaType: String
        let data: String

        enum CodingKeys: String, CodingKey {
            case mediaType = "media_type"
            case data
        }
    }

    private enum CodingKeys: String, CodingKey {
        case kind, text, reasoning
        case toolCall = "tool_call"
        case toolResult = "tool_result"
        case artifact, image
        case modelOnly = "model_only"
        case presentOnly = "present_only"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        switch try c.decodeIfPresent(String.self, forKey: .kind) {
        case "text":
            self = try .text(c.decodeIfPresent(String.self, forKey: .text) ?? "")
        case "reasoning":
            self = try .reasoning(c.decode(Reasoning.self, forKey: .reasoning))
        case "tool_call":
            self = try .toolCall(c.decode(ToolCall.self, forKey: .toolCall))
        case "tool_result":
            self = try .toolResult(c.decode(ToolResult.self, forKey: .toolResult))
        case "artifact_ref":
            var artifact = try c.decode(Artifact.self, forKey: .artifact)
            artifact.modelOnly = try c.decodeIfPresent(Bool.self, forKey: .modelOnly) ?? false
            artifact.presentOnly = try c.decodeIfPresent(Bool.self, forKey: .presentOnly) ?? false
            self = .artifact(artifact)
        case "image":
            self = try .image(c.decode(ImageContent.self, forKey: .image))
        default:
            self = .unknown
        }
    }
}

struct Message: Decodable, Sendable, Identifiable, Equatable {
    let id: String
    let role: MessageRole
    let status: MessageStatus?
    let parts: [ContentPart]
    let createdAt: Date?
    /// Agent-loop stamps (domain.Message.Metadata); `run_id` keys the
    /// message to its turn's file-change summary (WebUI transcript.ts).
    /// `var` (not `let`) so the memberwise init defaults it to nil —
    /// hand-built messages in tests stay source-compatible.
    var metadata: [String: String]?

    enum CodingKeys: String, CodingKey {
        case id, role, status, parts, metadata
        case createdAt = "created_at"
    }
}

// MARK: - Turn summary (internal/app/turn_summary.go)

/// One write-tool file mutation within a finished turn
/// (runtimeevent.TurnFileChange): the first mutation of a path
/// establishes it (`created` = the file did not exist before the
/// turn), later ones only bump the edit count.
struct TurnFileChange: Decodable, Sendable, Equatable {
    let path: String
    let created: Bool?
    let edits: Int
    /// Post-mutation size of the LAST change (0 for old events).
    let size: Int64?
}

/// The review-oriented projection of one finished turn (snapshot
/// .turn_summaries; server-derived from the file.changed ledger, cap
/// 400): which files the turn's write tools touched. run_cmd writes
/// (sed/tee) bypass the ledger by design.
struct TurnSummary: Decodable, Sendable, Equatable {
    let runId: String?
    let turn: Int?
    let cancelled: Bool?
    let failed: Bool?
    let changes: [TurnFileChange]?

    enum CodingKeys: String, CodingKey {
        case runId = "run_id"
        case turn, cancelled, failed, changes
    }
}

/// Per-path review data of one run (GET /v1/sessions/{id}/runs/{runID}
/// /changes — app.RunFileStat): the ledger's before-content compared
/// against the file's CURRENT workspace content, no git involved.
struct RunFileStat: Decodable, Sendable, Equatable {
    let path: String
    let created: Bool?
    let edits: Int?
    /// -1 when the ledger never captured the content.
    let beforeSize: Int
    /// -1 when the file no longer exists or is unreadable.
    let afterSize: Int64
    let added: Int
    let removed: Int
    /// Real unified hunks (@@ headers, +/-/space lines); empty when
    /// identical or when notComparable explains why.
    let diff: String?
    let diffTruncated: Bool?
    /// Why the diff is unavailable (never captured / oversized /
    /// binary / unreadable); empty means the diff is trustworthy.
    let notComparable: String?

    enum CodingKeys: String, CodingKey {
        case path, created, edits
        case beforeSize = "before_size"
        case afterSize = "after_size"
        case added, removed, diff
        case diffTruncated = "diff_truncated"
        case notComparable = "not_comparable"
    }
}

struct RunChangeStatsResponse: Decodable, Sendable {
    let runId: String?
    let entries: [RunFileStat]?

    enum CodingKeys: String, CodingKey {
        case runId = "run_id"
        case entries
    }
}

/// POST /v1/sessions/{id}/runs/{runID}/revert result: paths restored
/// to their pre-turn content, turn-created files removed, conflicts
/// (external modifications overwritten — reported, never silent),
/// and unrestorable paths (content never captured).
struct RevertOutcome: Decodable, Sendable {
    let restored: [String]?
    let deleted: [String]?
    let conflicts: [String]?
    let skipped: [String]?
}

struct TranscriptPage: Decodable, Sendable {
    let sessionId: String
    let nextAfter: UInt64?
    let hasMore: Bool
    let messages: [Message]

    enum CodingKeys: String, CodingKey {
        case sessionId = "session_id"
        case nextAfter = "next_after"
        case hasMore = "has_more"
        case messages
    }
}

// MARK: - Snapshot (internal/app/controller.go)

struct Usage: Decodable, Sendable {
    let turns: Int?
    let toolCalls: Int?
    let inputTokens: Int64?
    let outputTokens: Int64?
    let cachedInputTokens: Int64?
    let contextTokens: Int64?
    let reasoningTokens: Int64?
    let costUsd: Double?

    enum CodingKeys: String, CodingKey {
        case turns
        case toolCalls = "tool_calls"
        case inputTokens = "input_tokens"
        case outputTokens = "output_tokens"
        case cachedInputTokens = "cached_input_tokens"
        case contextTokens = "context_tokens"
        case reasoningTokens = "reasoning_tokens"
        case costUsd = "cost_usd"
    }
}

enum SessionState: String, Decodable, Sendable {
    case booting, idle, running
    case awaitingApproval = "awaiting_approval"
    case cancelling, closed, fatal, unknown

    init(from decoder: Decoder) throws {
        let raw = try decoder.singleValueContainer().decode(String.self)
        self = SessionState(rawValue: raw) ?? .unknown
    }
}

struct PendingRequest: Decodable, Sendable, Identifiable {
    let kind: Kind
    let id: String
    let approval: ApprovalRequestedPayload?
    let question: Question?

    enum Kind: String, Decodable, Sendable {
        case approval, question, unknown

        init(from decoder: Decoder) throws {
            let raw = try decoder.singleValueContainer().decode(String.self)
            self = Kind(rawValue: raw) ?? .unknown
        }
    }

    struct Question: Decodable, Sendable {
        let id: String
        let text: String
        let options: [QuestionAskedPayload.Option]
        let allowMultiple: Bool?

        enum CodingKeys: String, CodingKey {
            case id, text, options
            case allowMultiple = "allow_multiple"
        }
    }
}

struct Snapshot: Decodable, Sendable {
    let state: SessionState
    let sessionId: String
    let modelName: String
    let providerName: String?
    let contextWindow: Int?
    let window: ContextWindow?
    let occupancy: Int64?
    let workspaceRoot: String?
    let turnCount: Int
    let usage: Usage?
    let messages: [Message]?
    /// Per-turn file-change projection: each becomes the turn's
    /// closing review card (WebUI .block-turn-summary), keyed to the
    /// turn via run_id (assistant messages carry it in metadata).
    let turnSummaries: [TurnSummary]?
    let pendingRequests: [PendingRequest]?
    let pendingSteers: [String]?
    let lastError: SnapshotError?
    let plan: PlanPayload?
    /// Sub-agent child sessions are read-only for frontends (WebUI
    /// readOnly badge); the parent id renders as the badge's tooltip.
    let delegated: Bool?
    let parentSessionId: String?
    let eventSeq: UInt64

    struct SnapshotError: Decodable, Sendable {
        let stage: String?
        let code: String?
        let message: String
    }

    enum CodingKeys: String, CodingKey {
        case state
        case sessionId = "session_id"
        case modelName = "model_name"
        case providerName = "provider_name"
        case contextWindow = "context_window"
        case window, occupancy
        case workspaceRoot = "workspace_root"
        case turnCount = "turn_count"
        case usage, messages
        case turnSummaries = "turn_summaries"
        case pendingRequests = "pending_requests"
        case pendingSteers = "pending_steers"
        case lastError = "last_error"
        case plan
        case delegated
        case parentSessionId = "parent_session_id"
        case eventSeq = "event_seq"
    }
}

/// Context window projection (snapshot.window) — the same accounting
/// as the backend compaction trigger; `effective` is the gauge's
/// denominator (WebUI CtxGauge: occupancy / window.effective).
struct ContextWindow: Decodable, Sendable {
    let nominal: Int?
    let effective: Int?
    let compactTrigger: Int?
    let compactTarget: Int?

    enum CodingKeys: String, CodingKey {
        case nominal, effective
        case compactTrigger = "compact_trigger"
        case compactTarget = "compact_target"
    }
}

// MARK: - Turn control responses

struct PromptResponse: Decodable, Sendable {
    let turn: Int?
    let steered: Bool?
    let queueLen: Int?
    let deduplicated: Bool?

    enum CodingKeys: String, CodingKey {
        case turn, steered
        case queueLen = "queue_len"
        case deduplicated
    }
}

// MARK: - Workspaces

struct Workspace: Decodable, Sendable, Identifiable, Hashable {
    let id: String
    let name: String
    let rootPath: String
    let sessionCount: Int?
    let isDefault: Bool?

    enum CodingKeys: String, CodingKey {
        case id, name
        case rootPath = "root_path"
        case sessionCount = "session_count"
        case isDefault = "is_default"
    }
}

struct WorkspaceListResponse: Decodable, Sendable {
    let workspaces: [Workspace]
}
