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

// MARK: - Draft turn (live streaming projection)

/// Live view of the in-flight turn, assembled from model/tool events.
/// Authoritative history always comes from the snapshot — the draft is an
/// overlay that is discarded on every reconcile (turn end, resync).
///
/// The draft is an ORDERED segment list (WebUI transcript blocks):
/// reasoning, text and tool cards interleave in arrival order, so the
/// live turn lays out exactly like the rebuilt history — thinking stays
/// where it happened instead of piling up at the top of the turn.
struct DraftTurn: Sendable {
    var turnIndex: Int?
    var requestId: String?

    private(set) var segments: [DraftSegment] = []

    /// Raw argument deltas not yet matched to a tool.prepared (keyed by
    /// the model's tool_index, which is only request-local).
    var pendingArgs: [Int: PendingToolArgs] = [:]

    struct PendingToolArgs: Sendable {
        var name: String?
        var toolId: String?
        var arguments = ""
        var materialized = false
    }

    /// The open (unsealed) reasoning/text segments — at most one each,
    /// sealed by response_completed, the next request, the tool phase,
    /// or a terminal event (WebUI finalizeReasoning/finalizeStream).
    private var liveReasoningId: UUID?
    private var liveTextId: UUID?

    var isEmpty: Bool {
        segments.allSatisfy { segment in
            switch segment {
            case let .reasoning(reasoning): reasoning.text.isEmpty
            case let .text(text): text.text.isEmpty
            case .tool: false
            }
        }
    }

    // MARK: Model stream

    /// model.request_started: a new request means the previous one
    /// completed without us seeing response_completed — seal whatever is
    /// still open (the cursor/pulse must not bleed across requests).
    mutating func beginRequest(_ requestId: String?) {
        sealText()
        sealReasoning()
        self.requestId = requestId
    }

    /// model.reasoning_delta (WebUI reasoningAppend): append to the open
    /// segment, opening a new one once the previous was sealed. Under
    /// interleaved protocols reasoning can arrive after body text has
    /// started — the new segment goes BEFORE the live text draft, never
    /// below it.
    mutating func appendReasoning(_ delta: String) {
        if liveReasoningId == nil {
            let reasoning = DraftReasoning()
            if let textId = liveTextId, let index = segmentIndex(of: textId) {
                segments.insert(.reasoning(reasoning), at: index)
            } else {
                segments.append(.reasoning(reasoning))
            }
            liveReasoningId = reasoning.id
        }
        guard let id = liveReasoningId, let index = segmentIndex(of: id),
              case var .reasoning(reasoning) = segments[index]
        else { return }
        reasoning.text += delta
        segments[index] = .reasoning(reasoning)
    }

    /// model.text_delta: body text starting seals thinking (tool-call
    /// responses may never carry response_completed — the WebUI seals on
    /// text_delta for the same reason).
    mutating func appendText(_ delta: String) {
        sealReasoning()
        if liveTextId == nil {
            let text = DraftText()
            segments.append(.text(text))
            liveTextId = text.id
        }
        guard let id = liveTextId, let index = segmentIndex(of: id),
              case var .text(text) = segments[index]
        else { return }
        text.text += delta
        segments[index] = .text(text)
    }

    /// model.response_completed (§5.4): the canonical text replaces the
    /// lossy delta draft wholesale; without one, the delta draft stands.
    /// Either way the request's open segments are sealed.
    mutating func completeResponse(canonicalText: String?) {
        if let canonical = canonicalText, !canonical.isEmpty {
            var text = DraftText()
            text.text = canonical
            text.live = false
            if let id = liveTextId, let index = segmentIndex(of: id) {
                segments[index] = .text(text)
            } else {
                segments.append(.text(text))
            }
            liveTextId = nil
        } else {
            sealText()
        }
        sealReasoning()
    }

    /// Terminal failure/cancel paths: seal both — no live flicker may
    /// survive (WebUI sweepLiveReasoning).
    mutating func sealAll() {
        sealText()
        sealReasoning()
    }

    /// Seals the open reasoning segment: clears the live flag and stamps
    /// the thinking span (first delta → now; the event envelope's `time`
    /// is not decoded, and local receipt time is close enough).
    mutating func sealReasoning() {
        guard let id = liveReasoningId else { return }
        liveReasoningId = nil
        guard let index = segmentIndex(of: id),
              case var .reasoning(reasoning) = segments[index]
        else { return }
        reasoning.live = false
        reasoning.durationMs = Int64(Date().timeIntervalSince(reasoning.startedAt) * 1000)
        segments[index] = .reasoning(reasoning)
    }

    /// Seals the open text segment (WebUI finalizeStream): an empty
    /// draft is dropped entirely.
    mutating func sealText() {
        guard let id = liveTextId else { return }
        liveTextId = nil
        guard let index = segmentIndex(of: id),
              case var .text(text) = segments[index]
        else { return }
        text.live = false
        if text.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            segments.remove(at: index)
        } else {
            segments[index] = .text(text)
        }
    }

    // MARK: Tools

    /// tool.prepared: materialize (or update) the call's card in arrival
    /// order.
    mutating func upsertTool(_ tool: ToolCallState) {
        if let index = toolIndex(of: tool.id) {
            segments[index] = .tool(tool)
        } else {
            segments.append(.tool(tool))
        }
    }

    func tool(callId: String) -> ToolCallState? {
        guard let index = toolIndex(of: callId),
              case let .tool(tool) = segments[index]
        else { return nil }
        return tool
    }

    mutating func updateTool(callId: String, name: String, mutate: (inout ToolCallState) -> Void) {
        var tool = tool(callId: callId) ?? ToolCallState(id: callId, name: name)
        mutate(&tool)
        upsertTool(tool)
    }

    private func segmentIndex(of id: UUID) -> Int? {
        segments.firstIndex { segment in
            switch segment {
            case let .reasoning(reasoning): reasoning.id == id
            case let .text(text): text.id == id
            case .tool: false
            }
        }
    }

    private func toolIndex(of callId: String) -> Int? {
        segments.firstIndex { segment in
            if case let .tool(tool) = segment {
                return tool.id == callId
            }
            return false
        }
    }
}

/// One render unit of the live draft (WebUI BlockModel).
enum DraftSegment: Identifiable, Sendable {
    case reasoning(DraftReasoning)
    case text(DraftText)
    case tool(ToolCallState)

    var id: String {
        switch self {
        case let .reasoning(reasoning): "reasoning-\(reasoning.id.uuidString)"
        case let .text(text): "text-\(text.id.uuidString)"
        case let .tool(tool): "tool-\(tool.id)"
        }
    }
}

/// Live reasoning block: `live` drives the thinking pulse/tail preview;
/// `durationMs` is stamped at seal time (the "thought for Ns" header).
struct DraftReasoning: Identifiable, Sendable {
    let id = UUID()
    var text = ""
    var live = true
    var startedAt = Date()
    var durationMs: Int64?
}

/// Live text draft: `live` adds the streaming cursor.
struct DraftText: Identifiable, Sendable {
    let id = UUID()
    var text = ""
    var live = true
}

struct ToolCallState: Identifiable, Sendable, Equatable {
    /// call_id.
    let id: String
    var name: String
    var arguments = ""
    var risk: Int?
    var target: String?
    var diff: String?
    var status: Status = .prepared
    var durationMs: Int64?
    var preview: String?
    var errorMessage: String?
    var subagentTask: String?
    var subagentOutcome: String?
    /// Display-bound artifact refs carried by tool.completed (already
    /// filtered server-side: model_only / view_image never appears).
    var artifacts: [ContentPart.Artifact] = []

    enum Status: Sendable {
        case prepared, running, success, error, timeout, cancelled, unknown
    }
}

// MARK: - Connection status

enum SessionConnection: Equatable, Sendable {
    case connecting
    case live
    /// Server is shutting down (server.draining) — do not reconnect.
    case drained
    case offline(attempt: Int)
}

// MARK: - Session store

/// Per-session state machine: snapshot → SSE attach (after=event_seq) →
/// event projection, with the protocol's resync/reconnect semantics
/// (SERVE_DESIGN §5.4, mirroring webui/src/protocol/sse.ts).
@MainActor
@Observable
final class SessionStore {
    let sessionId: String
    /// Owning workspace (from the session summary) — the approval-mode
    /// quick toggle is a workspace-level override.
    let workspaceId: String?
    private let api: APIClient
    private let sse: SSEClient

    private(set) var state: SessionState = .booting {
        didSet {
            if !applyingSnapshot {
                rebuildTranscript()
            }
        }
    }

    private(set) var modelName = ""
    private(set) var providerName = ""
    /// Composer pickers (the server applies them from the next turn on).
    private(set) var reasoningEffort = "default"
    private(set) var approvalMode = "on-request"
    /// Catalog default model ref ("provider/model", the WebUI's
    /// defaultModelRef) — pushed in by SessionListStore; lets the
    /// composer mark the active row when the snapshot carries only a
    /// bare model name.
    var defaultModelRef: String?
    private(set) var messages: [Message] = [] {
        didSet {
            if !applyingSnapshot {
                rebuildTranscript()
            }
        }
    }

    /// Per-turn file-change projection (snapshot.turn_summaries): each
    /// becomes the turn's closing review card (WebUI
    /// .block-turn-summary) mounted at its turn boundary.
    private(set) var turnSummaries: [TurnSummary] = [] {
        didSet {
            if !applyingSnapshot {
                rebuildTranscript()
            }
        }
    }

    /// Precomputed transcript render data (rows, merged tool blocks,
    /// diffs, action-row flags). Rebuilt ONLY when messages/state
    /// change — never on draft frames — so view-body evaluation is a
    /// pure lookup instead of three O(history) walks plus an LCS diff
    /// per edit call. (Previously these were ChatView computed
    /// properties re-derived on every streaming frame.)
    private(set) var transcript: TranscriptModel = .empty
    private var applyingSnapshot = false
    /// Composer input, kept in the store so it survives session
    /// switches (SessionListStore keeps stores warm; the chat view
    /// itself is destroyed on every switch via .id(sessionId)).
    var composerDraft = ""
    /// Signals the composer to focus after a recovery action prepares a draft.
    private(set) var composerFocusRequest = UUID()
    private(set) var sendingPrompt = false
    /// Retain the key only when the outcome is unknown and the exact request is retried.
    private var uncertainPrompt: (text: String, key: String)?
    private(set) var draft: DraftTurn?
    private(set) var pendingApprovals: [ApprovalRequestedPayload] = []
    private(set) var pendingQuestions: [PendingRequest.Question] = []
    private(set) var plan: PlanPayload?
    /// Distinguishes a newly created plan from a previous one with identical goals.
    private(set) var planIdentity = UUID()
    private(set) var usage: Usage?
    private(set) var occupancy: Int64?
    /// Gauge denominator: snapshot.window.effective (the compaction
    /// accounting), falling back to the legacy context_window field.
    private(set) var contextWindow: Int?
    private(set) var window: ContextWindow?
    private(set) var turnCount = 0
    private(set) var pendingSteers: [String] = []
    private(set) var notices: [String] = []
    private static let maxNotices = 10
    private(set) var lastError: String?
    enum TurnFeedback: Equatable {
        case cancelled
        case failed(String)
    }

    private(set) var turnFeedback: TurnFeedback?
    private var dismissedTurnFailure: String?
    private(set) var connection: SessionConnection = .connecting
    private(set) var hasLoaded = false

    /// Turn-activity hook (WebUI controller: submitPrompt, turn.finished
    /// and approval events all refresh the session list — the sidebar's
    /// derived title and status dots are fed by it). SessionListStore
    /// wires this when vending the store.
    var onTurnActivity: (@MainActor () -> Void)?
    /// Sub-agent sessions are read-only (WebUI hdr-readonly badge,
    /// sourced from snapshot.delegated); the title shows the parent id.
    private(set) var readOnly = false
    private(set) var readOnlyTitle = ""

    /// Global sequence cursor — the snapshot's event_seq stitched with
    /// every observed frame id (query `after=` wins over Last-Event-ID).
    private var cursor: UInt64 = 0
    /// A snapshot may commit only if no later request or SSE projection has overtaken it.
    private var snapshotRequest = 0
    private var snapshotCommit = 0
    private var projectionRevision = 0
    /// An invalid replay cursor (including a prior server instance) must be
    /// replaced by the next snapshot watermark, never merged with it.
    private var resetCursorOnSnapshot = false
    private var instance: String?
    private var lastFrameAt = Date()
    private var loopTask: Task<Void, Never>?
    private var stopped = false
    private var reconnectAttempt = 0

    init(sessionId: String, workspaceId: String? = nil, api: APIClient) {
        self.sessionId = sessionId
        self.workspaceId = workspaceId
        self.api = api
        sse = SSEClient(baseURL: api.baseURL, token: api.token)
    }

    var isBusy: Bool {
        state == .running || state == .awaitingApproval || state == .cancelling
    }

    /// SSE-driven activity signal: bumps on substantive execution events
    /// (turn start/end, model requests/responses, tool lifecycle, gate
    /// changes, budget updates). The maze/trace views schedule their
    /// debounced /maze refetch off this — fresher than usage signals and
    /// cheaper than polling, since the event stream already knows exactly
    /// when the execution shape changed.
    private(set) var activityGeneration = 0

    /// Bumps every time buffered stream deltas are applied to the draft
    /// (~25fps while streaming): the trace view's continuous-follow
    /// trigger — the viewport tracks every flush, not 256-char buckets.
    private(set) var streamRevision = 0

    func maze() async throws -> MazeData {
        try await api.maze(sessionId)
    }

    /// Raw event log (NDJSON) for the trace tab's "Session log" export.
    func exportSessionLog() async throws -> Data {
        try await api.exportSessionLog(sessionId)
    }

    private func rebuildTranscript() {
        let midTurn = state == .running || state == .cancelling || state == .awaitingApproval
        transcript = TranscriptModel.build(
            messages: messages, midTurn: midTurn, turnSummaries: turnSummaries,
        )
    }

    var occupancyFraction: Double? {
        guard let occupancy, let contextWindow, contextWindow > 0 else { return nil }
        return min(1, Double(occupancy) / Double(contextWindow))
    }

    /// The gauge's red threshold (WebUI CtxGauge): compact_trigger /
    /// effective, defaulting to 0.8 when the server omits the trigger.
    var compactTriggerRatio: Double {
        guard let trigger = window?.compactTrigger, let effective = window?.effective,
              trigger > 0, effective > 0
        else { return 0.8 }
        return Double(trigger) / Double(effective)
    }

    // MARK: Artifacts (WebUI fetchArtifactURL + LRU cache)

    /// Loaded artifact bytes keyed by "id:size" — the transcript
    /// re-renders on every stream frame, so without this cache a
    /// scrolled-past image would re-download on every rebuild.
    /// Eviction is true LRU: artifactLRU tracks access order.
    private var artifactCache: [String: (data: Data, mediaType: String?)] = [:]
    private var artifactLRU: [String] = []

    func artifactData(_ artifact: ContentPart.Artifact) async -> (data: Data, mediaType: String?)? {
        let key = "\(artifact.id):\(artifact.size ?? -1)"
        if let cached = artifactCache[key] {
            artifactLRU.removeAll { $0 == key }
            artifactLRU.append(key)
            return cached
        }
        guard let entry = try? await api.fetchArtifact(artifact.id, size: artifact.size)
        else { return nil }
        // LRU evict at 32 entries (WebUI ARTIFACT_CACHE_MAX).
        if artifactLRU.count >= 32 {
            artifactCache.removeValue(forKey: artifactLRU.removeFirst())
        }
        artifactCache[key] = entry
        artifactLRU.append(key)
        return entry
    }

    private func addNotice(_ text: String) {
        notices.append(text)
        if notices.count > Self.maxNotices {
            notices.removeFirst(notices.count - Self.maxNotices)
        }
    }

    // MARK: Lifecycle

    func start() {
        guard loopTask == nil, !stopped else { return }
        loopTask = Task { [weak self] in await self?.eventLoop() }
        Task { [weak self] in await self?.loadApprovalMode() }
    }

    func stop() {
        stopped = true
        loopTask?.cancel()
        loopTask = nil
        deltaFlushTask?.cancel()
        deltaFlushTask = nil
        deltaBuffer.removeAll()
        onTurnActivity = nil
    }

    // MARK: Commands

    /// Claim the draft synchronously before starting a request. The store owns
    /// both the in-flight gate and draft restoration, even across view switches.
    @discardableResult
    func sendComposerDraft() -> Task<Void, Never>? {
        let text = composerDraft
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !sendingPrompt, !trimmed.isEmpty, state != .closed, state != .booting else { return nil }
        sendingPrompt = true
        lastError = nil
        composerDraft = ""
        let key: String = if let uncertainPrompt, uncertainPrompt.text == trimmed {
            uncertainPrompt.key
        } else {
            UUID().uuidString
        }
        // A different message is a new intent, not a retry of the previous one.
        uncertainPrompt = nil
        return Task { [self] in
            do {
                try await api.sendPrompt(sessionId, prompt: trimmed, idempotencyKey: key)
                // No optimistic append: idle → turn.started carries the prompt;
                // busy → steer.queued carries it. Both arrive within ms.
                onTurnActivity?()
            } catch {
                // A transport error, malformed success response or server error
                // may follow a committed submission. Only explicit rejections
                // allow the next attempt to use a fresh key.
                if !Self.isDefinitePromptRejection(error) {
                    uncertainPrompt = (trimmed, key)
                }
                lastError = error.localizedDescription
                let draft = composerDraft
                composerDraft = draft.isEmpty ? text : text + "\n" + draft
            }
            sendingPrompt = false
        }
    }

    private static func isDefinitePromptRejection(_ error: Error) -> Bool {
        guard case let LoomAPIError.http(_, code, _) = error else { return false }
        // These are rejected before a prompt can be accepted by the service.
        return ["invalid_input", "unauthenticated", "session_archived", "session_closed",
                "rate_limited", "too_large", "not_found"].contains(code)
    }

    func cancel() async {
        do {
            try await api.cancel(sessionId)
        } catch {
            lastError = error.localizedDescription
        }
    }

    func requestCompaction() async {
        do {
            try await api.requestCompaction(sessionId)
            addNotice("Context compaction scheduled — it runs at the start of the next turn.")
        } catch {
            lastError = error.localizedDescription
        }
    }

    // MARK: Turn change review / revert (turn-summary card)

    /// Stats compare each run's ledger against the CURRENT workspace, so
    /// reverting any run invalidates every card, not just the reverted one.
    private enum CachedRunStats {
        case available([RunFileStat])
        case unavailable

        var value: [RunFileStat]? {
            if case let .available(stats) = self {
                return stats
            }
            return nil
        }
    }

    private var runStatsCache: [String: CachedRunStats] = [:]
    private var runStatsEpoch = 0
    private var runStatsRequests: [String: Int] = [:]

    func runStats(runId: String, forceRefresh: Bool = false) async -> [RunFileStat]? {
        if !forceRefresh, let cached = runStatsCache[runId] {
            return cached.value
        }
        let epoch = runStatsEpoch
        let request = (runStatsRequests[runId] ?? 0) + 1
        runStatsRequests[runId] = request
        let result: CachedRunStats
        do {
            let response = try await api.runChanges(sessionId, runId: runId)
            if let entries = response.entries, !entries.isEmpty {
                result = .available(entries)
            } else {
                result = .unavailable
            }
        } catch {
            // An unavailable result is cached too; otherwise every card
            // mount retries a failed or empty request.
            result = .unavailable
        }
        // MainActor is reentrant across the await: an older request must
        // neither overwrite a forced refresh nor resurrect pre-revert stats.
        guard epoch == runStatsEpoch, runStatsRequests[runId] == request else { return nil }
        runStatsCache[runId] = result
        return result.value
    }

    /// Revert one turn's file mutations (WebUI revertRun): restores
    /// the recorded before-content, conflicts are overwritten and
    /// reported. Invalidates all run stats so cards refetch against
    /// the current workspace. Returns the
    /// note text + warn flag for the card's .tsm-note line.
    func revertRun(runId: String) async -> (note: String, warn: Bool) {
        do {
            let outcome = try await api.revertRun(sessionId, runId: runId)
            runStatsEpoch += 1
            runStatsCache.removeAll()
            runStatsRequests.removeAll()
            let restored = (outcome.restored ?? []).count + (outcome.deleted ?? []).count
            let conflicts = (outcome.conflicts ?? []).count
            let skipped = (outcome.skipped ?? []).count
            if conflicts > 0 {
                return (
                    "Reverted \(restored) file(s); \(conflicts) external change(s) made after the turn were overwritten",
                    true,
                )
            }
            if skipped > 0 {
                return (
                    "Reverted \(restored) file(s); \(skipped) had no recorded content and were left as-is",
                    true,
                )
            }
            return ("Reverted \(restored) file(s) to their pre-turn state", false)
        } catch {
            return ("Revert failed: \(error.localizedDescription)", true)
        }
    }

    // MARK: Share link (WebUI shareSession)

    /// Mints the public read-only link and returns the absolute URL —
    /// the LAN listener's URL when sharing is up, else this client's
    /// base URL + path (WebUI: absoluteUrl || location.origin + path).
    func shareLink() async -> String? {
        do {
            let link = try await api.shareSession(sessionId)
            if let url = link.url, !url.isEmpty {
                return url
            }
            return api.baseURL.absoluteString + link.path
        } catch {
            lastError = error.localizedDescription
            return nil
        }
    }

    /// Shift-click on the header's share button: the link stops
    /// resolving immediately.
    func revokeShare() async {
        do {
            try await api.revokeShare(sessionId)
        } catch {
            lastError = error.localizedDescription
        }
    }

    // MARK: Composer pickers

    /// ref: "provider/model" (same shape as the WebUI's curModelRef).
    func pickModel(_ ref: String) async {
        let parts = ref.split(separator: "/", maxSplits: 1).map(String.init)
        guard parts.count == 2 else { return }
        do {
            try await api.setModel(sessionId, provider: parts[0], model: parts[1])
            providerName = parts[0]
            modelName = parts[1]
        } catch {
            lastError = error.localizedDescription
        }
    }

    func pickReasoning(_ effort: String) async {
        do {
            try await api.setReasoning(sessionId, effort: effort)
            reasoningEffort = effort
        } catch {
            lastError = error.localizedDescription
        }
    }

    func pickApprovalMode(_ mode: String) async {
        guard let workspaceId else {
            lastError = "Cannot change approval mode: this session has no workspace ID."
            return
        }
        do {
            try await api.setWorkspaceApprovalMode(workspaceId, mode: mode)
            approvalMode = mode
        } catch {
            lastError = error.localizedDescription
        }
    }

    private func loadApprovalMode() async {
        guard let workspaceId else { return }
        if let mode = try? await api.workspaceApprovalMode(workspaceId), !mode.isEmpty {
            approvalMode = mode
        }
    }

    /// WebUI parity: `always` sends an (optionally trusted) rule_hint so
    /// the server remembers the call — plain "Always allow" for this
    /// workspace, or "Trust (no sandbox)" when `trust` is set.
    func resolveApproval(
        _ approval: ApprovalRequestedPayload, decision: ApprovalDecision,
        always: Bool = false, trust: String? = nil,
    ) async {
        // Snappy UI: remove now; a stale binding (409) triggers a reconcile
        // which restores the authoritative pending set.
        pendingApprovals.removeAll { $0.approvalId == approval.approvalId }
        do {
            try await api.resolveApproval(
                sessionId, approvalId: approval.approvalId,
                callId: approval.callId, argsHash: approval.argsHash, decision: decision,
                ruleHint: always ? ApprovalRuleHint(trust: trust) : nil,
            )
        } catch {
            lastError = error.localizedDescription
            if state == .awaitingApproval,
               !pendingApprovals.contains(where: { $0.approvalId == approval.approvalId })
            {
                pendingApprovals.append(approval)
            }
            await refresh()
        }
    }

    func answerQuestion(
        _ question: PendingRequest.Question,
        selected: [String], customText: String?, skipped: Bool,
    ) async {
        pendingQuestions.removeAll { $0.id == question.id }
        do {
            try await api.answerQuestion(
                sessionId, questionId: question.id,
                selected: selected, customText: customText, skipped: skipped,
            )
        } catch {
            lastError = error.localizedDescription
            if isBusy, !pendingQuestions.contains(where: { $0.id == question.id }) {
                pendingQuestions.append(question)
            }
            await refresh()
        }
    }

    func dismissNotices() {
        notices.removeAll()
    }

    func dismissError() {
        lastError = nil
    }

    func dismissTurnFailure() {
        if case let .failed(message) = turnFeedback {
            dismissedTurnFailure = message
        }
    }

    var visibleTurnFailure: String? {
        guard !isBusy, case let .failed(message) = turnFeedback,
              dismissedTurnFailure != message else { return nil }
        return message
    }

    var canContinueTurn: Bool {
        visibleTurnFailure != nil && state == .idle && !readOnly
            && !sendingPrompt && composerDraft.isEmpty
    }

    /// Prepares a new instruction against the existing conversation, without
    /// resubmitting the last prompt or automatically running any tools.
    @discardableResult
    func prepareContinuation() -> Bool {
        guard canContinueTurn else { return false }
        composerDraft = "Continue the unfinished task from where you left off. Review what has already been done and do not repeat completed actions."
        composerFocusRequest = UUID()
        return true
    }

    /// Retry the last user prompt as a new turn. Never overwrite an unsent draft.
    /// A retry can repeat tool side effects; it is only invoked by an explicit click.
    var canRetryLastTurn: Bool {
        guard canContinueTurn,
              let prompt = messages.last(where: { $0.role == .user })?.copyText
        else { return false }
        return !prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    @discardableResult
    func retryLastTurn() -> Task<Void, Never>? {
        guard canRetryLastTurn,
              let prompt = messages.last(where: { $0.role == .user })?.copyText
        else { return nil }
        composerDraft = prompt
        turnFeedback = nil
        return sendComposerDraft()
    }

    private func setTurnFailure(_ message: String) {
        guard turnFeedback != .cancelled else { return }
        turnFeedback = .failed(message)
        dismissedTurnFailure = nil
    }

    // MARK: Snapshot (authoritative reconcile)

    /// Full resync: re-read the snapshot and re-stitch the cursor. Called
    /// on start, on server.resync, on instance change, and after every
    /// turn end to swap the lossy draft for canonical messages.
    func refresh() async {
        guard !stopped else { return }
        snapshotRequest += 1
        let request = snapshotRequest
        let revision = projectionRevision
        do {
            let snap = try await api.snapshot(sessionId)
            commitSnapshot(snap, request: request, revision: revision)
        } catch let LoomAPIError.http(status, _, _) where status == 404 {
            // Session not alive in this process — resume, then snapshot.
            guard request == snapshotRequest, revision == projectionRevision, !Task.isCancelled else { return }
            do {
                try await api.createSession(resume: sessionId)
                guard request == snapshotRequest, revision == projectionRevision, !Task.isCancelled else { return }
                let snap = try await api.snapshot(sessionId)
                commitSnapshot(snap, request: request, revision: revision)
            } catch {
                if request == snapshotRequest, revision == projectionRevision, !Task.isCancelled {
                    lastError = error.localizedDescription
                }
            }
        } catch {
            if request == snapshotRequest, revision == projectionRevision, !Task.isCancelled {
                lastError = error.localizedDescription
            }
        }
    }

    private func commitSnapshot(_ snap: Snapshot, request: Int, revision: Int) {
        guard !stopped, request == snapshotRequest, revision == projectionRevision, !Task.isCancelled else { return }
        // Within one server instance the broker sequence is global and monotonic.
        // A snapshot may trail the terminal frame (the server projects idle before
        // publishing turn.finished), so preserve the larger observed cursor.
        let previousCursor = cursor
        applySnapshot(snap)
        cursor = Self.reconciledCursor(
            previous: previousCursor, snapshot: snap.eventSeq, reset: resetCursorOnSnapshot,
        )
        resetCursorOnSnapshot = false
        snapshotCommit += 1
    }

    /// A terminal event may arrive after the server captured its snapshot
    /// watermark. Only a rejected cursor or new broker instance may lower it.
    nonisolated static func reconciledCursor(previous: UInt64, snapshot: UInt64, reset: Bool) -> UInt64 {
        reset ? snapshot : max(previous, snapshot)
    }

    private func applySnapshot(_ snap: Snapshot) {
        // The snapshot replaces the draft wholesale — any buffered
        // deltas belong to the pre-snapshot projection.
        deltaFlushTask?.cancel()
        deltaFlushTask = nil
        deltaBuffer.removeAll()
        // Assigning these separately triggers three full transcript builds.
        // Commit the snapshot's render inputs together, then build once.
        applyingSnapshot = true
        state = snap.state
        modelName = snap.modelName
        providerName = snap.providerName ?? ""
        turnSummaries = snap.turnSummaries ?? []
        messages = snap.messages ?? []
        applyingSnapshot = false
        rebuildTranscript()
        draft = nil
        setPlan(snap.plan)
        usage = snap.usage
        occupancy = snap.occupancy
        window = snap.window
        contextWindow = snap.window?.effective ?? snap.contextWindow
        turnCount = snap.turnCount
        pendingSteers = snap.pendingSteers ?? []
        if let failure = snap.lastError?.message, !failure.isEmpty {
            // The server may retain a request's cancellation text in last_error.
            // A run.cancelled event is authoritative: never turn that user action into a failure.
            if turnFeedback != .cancelled {
                turnFeedback = .failed(failure)
            }
        } else if turnFeedback != .cancelled {
            turnFeedback = nil
        }
        readOnly = snap.delegated ?? false
        readOnlyTitle = snap.parentSessionId.map { "parent: \($0)" } ?? ""

        var approvals: [ApprovalRequestedPayload] = []
        var questions: [PendingRequest.Question] = []
        for request in snap.pendingRequests ?? [] {
            switch request.kind {
            case .approval:
                if let approval = request.approval {
                    approvals.append(approval)
                }
            case .question:
                if let question = request.question {
                    questions.append(question)
                }
            case .unknown:
                break
            }
        }
        pendingApprovals = approvals
        pendingQuestions = questions

        hasLoaded = true
    }

    // MARK: SSE loop

    private enum ConsumeOutcome {
        /// Stream ended or errored — reconnect with backoff.
        case interrupted
        /// server.resync / instance change — refresh then reconnect now.
        case resync
        /// server.draining — stop permanently.
        case drained
    }

    private func eventLoop() async {
        await refresh()
        var backoffNs: UInt64 = 1_000_000_000 // 1s

        while !Task.isCancelled, !stopped {
            if connection != .live {
                connection = reconnectAttempt == 0 ? .connecting : .offline(attempt: reconnectAttempt)
            }

            let outcome = await consumeStream()
            if Task.isCancelled || stopped {
                return
            }

            switch outcome {
            case .drained:
                connection = .drained
                return
            case .resync, .interrupted:
                if case .resync = outcome {
                    let previousCommit = snapshotCommit
                    await refresh()
                    if snapshotCommit != previousCommit {
                        reconnectAttempt = 0
                        backoffNs = 1_000_000_000
                        continue
                    }
                }
                // A deleted session cannot be resumed. Back off instead of
                // repeatedly requesting its 404 snapshot and SSE endpoint.
                reconnectAttempt += 1
                connection = .offline(attempt: reconnectAttempt)
                // 1s → 15s exponential with ±25% jitter (sse.ts:10-11).
                let jitter = Double.random(in: 0.75 ... 1.25)
                let sleepNs = UInt64(Double(backoffNs) * jitter)
                try? await Task.sleep(nanoseconds: sleepNs)
                backoffNs = min(backoffNs * 2, 15_000_000_000)
            }
        }
    }

    /// One connection attempt: consume frames until end/error/watchdog.
    /// The 45s watchdog mirrors the web client — server heartbeats every
    /// 15s, so 45s of silence means the connection is silently dead.
    private func consumeStream() async -> ConsumeOutcome {
        lastFrameAt = Date()
        return await withTaskGroup(of: ConsumeOutcome.self, returning: ConsumeOutcome.self) { group in
            group.addTask { [sse, sessionId, cursor] in
                do {
                    for try await frame in sse.frames(sessionId: sessionId, after: cursor) {
                        await self.handleFrame(frame)
                        if await self.takeTurnRefreshRequest() {
                            // Keep the stream's event projection paused until the
                            // canonical snapshot has replaced the finished draft.
                            await self.refresh()
                        }
                        // Control frames end this consumption immediately —
                        // server.resync/server.draining are followed by the
                        // server closing the stream, but we don't wait for it.
                        if await self.takeDrainRequest() {
                            return .drained
                        }
                        if await self.takeResyncRequest() {
                            return .resync
                        }
                    }
                    return .interrupted
                } catch is CancellationError {
                    return .interrupted
                } catch SSEClient.StreamError.sessionNotAlive {
                    return .resync // refresh() resumes dead sessions
                } catch SSEClient.StreamError.rateLimited {
                    try? await Task.sleep(nanoseconds: 30_000_000_000)
                    return .interrupted
                } catch {
                    return .interrupted
                }
            }
            group.addTask {
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 5_000_000_000)
                    if Task.isCancelled {
                        break
                    }
                    let silence = await self.secondsSinceLastFrame()
                    if silence > 45 {
                        return .interrupted
                    }
                }
                return .interrupted
            }
            let outcome = await group.next() ?? .interrupted
            group.cancelAll()
            return outcome
        }
    }

    private func secondsSinceLastFrame() -> TimeInterval {
        Date().timeIntervalSince(lastFrameAt)
    }

    // MARK: Frame handling

    private func handleFrame(_ frame: SSEFrame) {
        guard !stopped else { return }
        lastFrameAt = Date()

        switch frame.content {
        case let .comment(text):
            guard let newInstance = Self.parseConnectedInstance(text) else { return }
            if let existing = instance, existing != newInstance {
                // Server restarted: sequence space reset — full resync.
                instance = newInstance
                projectionRevision += 1
                resetCursorOnSnapshot = true
                resyncRequested = true
            } else {
                instance = newInstance
                reconnectAttempt = 0
                connection = .live
            }

        case let .event(_, event, data):
            guard let event else { return }
            switch event {
            case "server.resync":
                // The broker rejected `after=` (future cursor or replay gap).
                // Keeping the old cursor after the snapshot would immediately
                // request the same invalid replay again. Invalidate snapshots
                // already in flight from before the rejection, too.
                projectionRevision += 1
                resetCursorOnSnapshot = true
                resyncRequested = true
            case "server.draining":
                drainRequested = true
            default:
                guard let raw = data.data(using: .utf8),
                      let runtime = try? LoomJSON.decoder.decode(RuntimeEvent.self, from: raw)
                else {
                    // The cursor cannot safely advance past an undecodable frame.
                    // Reconcile from a snapshot rather than silently dropping it.
                    resyncRequested = true
                    return
                }
                // A snapshot can include events still queued on this SSE stream.
                // Replaying them would roll the canonical projection backwards.
                guard runtime.sequence > cursor else { return }
                cursor = max(cursor, runtime.sequence)
                projectionRevision += 1
                apply(runtime)
            }
        }
    }

    /// Set by handleFrame, read by consumeStream via a side channel —
    /// control frames must terminate the current consumption, which the
    /// frame handler cannot do directly (it runs inside the iteration).
    private var resyncRequested = false
    private var drainRequested = false
    private var turnRefreshRequested = false

    private func takeTurnRefreshRequest() -> Bool {
        defer { turnRefreshRequested = false }
        return turnRefreshRequested
    }

    private func takeResyncRequest() -> Bool {
        defer { resyncRequested = false }
        return resyncRequested
    }

    private func takeDrainRequest() -> Bool {
        defer { drainRequested = false }
        return drainRequested
    }

    /// Parses the `: connected, instance=<id>` handshake comment.
    private static func parseConnectedInstance(_ comment: String) -> String? {
        let prefix = "connected, instance="
        guard comment.hasPrefix(prefix) else { return nil }
        return String(comment.dropFirst(prefix.count)).trimmingCharacters(in: .whitespaces)
    }

    // MARK: Event projection

    // MARK: Delta coalescing

    /// One buffered model-stream delta. Text/reasoning/tool-arg deltas
    /// arrive per token (tens per second); applying each one straight
    /// to `draft` fired an @Observable change per token, re-rendering
    /// the transcript at token rate. They are now buffered and applied
    /// in a batch at display cadence (~25fps); any non-delta event
    /// flushes first so event ordering is preserved.
    private enum PendingDelta {
        case text(String)
        case reasoning(String)
        case toolArgs(index: Int, name: String?, toolId: String?, arguments: String?)
    }

    private var deltaBuffer: [PendingDelta] = []
    private var deltaFlushTask: Task<Void, Never>?
    private static let deltaFlushInterval: Duration = .milliseconds(40)

    private func scheduleDeltaFlush() {
        guard deltaFlushTask == nil else { return }
        deltaFlushTask = Task { [weak self] in
            try? await Task.sleep(for: Self.deltaFlushInterval)
            guard let self, !Task.isCancelled else { return }
            deltaFlushTask = nil
            flushPendingDeltas()
        }
    }

    /// Applies the buffered deltas to the draft in arrival order. Called
    /// by the flush timer and synchronously before any non-delta event
    /// (response_completed seals text, tool.prepared seals reasoning —
    /// their draft mutations must observe every preceding delta).
    private func flushPendingDeltas() {
        deltaFlushTask?.cancel()
        deltaFlushTask = nil
        guard !deltaBuffer.isEmpty else { return }
        let pending = deltaBuffer
        deltaBuffer.removeAll(keepingCapacity: true)
        if draft == nil {
            draft = DraftTurn()
        }
        for delta in pending {
            switch delta {
            case let .text(text):
                draft?.appendText(text)
            case let .reasoning(text):
                draft?.appendReasoning(text)
            case let .toolArgs(index, name, toolId, arguments):
                var entry = draft?.pendingArgs[index] ?? DraftTurn.PendingToolArgs()
                if let name {
                    entry.name = name
                }
                if let toolId {
                    entry.toolId = toolId
                }
                if let arguments {
                    entry.arguments += arguments
                }
                draft?.pendingArgs[index] = entry
            }
        }
        streamRevision += 1
    }

    private func apply(_ event: RuntimeEvent) {
        switch event.kind {
        case .modelTextDelta, .modelReasoningDelta, .modelToolCallDelta:
            break // buffered below; applied at display cadence
        default:
            flushPendingDeltas()
        }
        switch event.kind {
        case .turnStarted, .modelRequestStarted, .modelResponseCompleted,
             .toolPrepared, .toolStarted, .toolCompleted,
             .approvalRequested, .approvalResolved, .questionAsked, .questionAnswered,
             .contextCompacted, .modelRequestFailed, .runCancelRequested, .turnFinished,
             .subagentStarted, .subagentFinished, .budgetUpdated:
            activityGeneration += 1
        default:
            break
        }
        switch event.kind {
        case .turnStarted:
            let payload = tryDecode(TurnStartedPayload.self, from: event)
            turnFeedback = nil
            dismissedTurnFailure = nil
            state = .running
            pendingSteers = []
            if let prompt = payload?.prompt, !prompt.isEmpty {
                messages.append(Message(
                    id: "local-\(event.sequence)", role: .user,
                    status: .final, parts: [.text(prompt)], createdAt: nil,
                ))
            }
            draft = DraftTurn(turnIndex: payload?.turnIndex)

        case .runPhaseChanged:
            let payload = tryDecode(RunPhaseChangedPayload.self, from: event)
            switch payload?.phase {
            case .awaitingApproval: state = .awaitingApproval
            case .none, .unknown: break
            default: state = .running
            }

        case .modelRequestStarted:
            let payload = tryDecode(ModelRequestStartedPayload.self, from: event)
            if draft == nil {
                draft = DraftTurn()
            }
            draft?.beginRequest(payload?.requestId)
            if let name = payload?.modelName, !name.isEmpty {
                modelName = name
            }

        case .modelTextDelta:
            if let payload = tryDecode(ModelDeltaPayload.self, from: event) {
                deltaBuffer.append(.text(payload.delta))
                scheduleDeltaFlush()
            }

        case .modelReasoningDelta:
            if let payload = tryDecode(ModelDeltaPayload.self, from: event) {
                deltaBuffer.append(.reasoning(payload.delta))
                scheduleDeltaFlush()
            }

        case .modelToolCallDelta:
            if let payload = tryDecode(ModelToolCallDeltaPayload.self, from: event) {
                deltaBuffer.append(.toolArgs(
                    index: payload.toolIndex,
                    name: payload.toolName,
                    toolId: payload.toolId,
                    arguments: payload.arguments,
                ))
                scheduleDeltaFlush()
            }

        case .modelResponseCompleted:
            let payload = tryDecode(ModelResponseCompletedPayload.self, from: event)
            if draft == nil {
                draft = DraftTurn()
            }
            draft?.completeResponse(canonicalText: payload?.text)
            if let input = payload?.inputTokens, let output = payload?.outputTokens {
                usage = Usage(
                    turns: usage?.turns, toolCalls: usage?.toolCalls,
                    inputTokens: input, outputTokens: output,
                    cachedInputTokens: usage?.cachedInputTokens,
                    contextTokens: usage?.contextTokens,
                    reasoningTokens: usage?.reasoningTokens,
                    costUsd: usage?.costUsd,
                )
            }

        case .toolPrepared:
            if let payload = tryDecode(ToolPreparedPayload.self, from: event) {
                materializeTool(payload)
            }

        case .toolStarted:
            if let payload = tryDecode(ToolStartedPayload.self, from: event) {
                updateTool(payload.callId, name: payload.toolName) { $0.status = .running }
            }

        case .toolCompleted:
            if let payload = tryDecode(ToolCompletedPayload.self, from: event) {
                updateTool(payload.callId, name: payload.toolName) { tool in
                    tool.status = ToolCallState.Status(from: payload.status)
                    tool.durationMs = payload.durationMs
                    tool.preview = payload.preview
                    tool.errorMessage = payload.errorMessage
                    tool.artifacts = payload.artifacts ?? []
                }
            }

        case .approvalRequested:
            if let payload = tryDecode(ApprovalRequestedPayload.self, from: event),
               !pendingApprovals.contains(where: { $0.approvalId == payload.approvalId })
            {
                pendingApprovals.append(payload)
                state = .awaitingApproval
                onTurnActivity?()
            }

        case .approvalResolved:
            if let payload = tryDecode(ApprovalResolvedPayload.self, from: event) {
                pendingApprovals.removeAll { $0.approvalId == payload.approvalId }
                if pendingApprovals.isEmpty, state == .awaitingApproval {
                    state = .running
                }
                onTurnActivity?()
            }

        case .questionAsked:
            if let payload = tryDecode(QuestionAskedPayload.self, from: event),
               !pendingQuestions.contains(where: { $0.id == payload.questionId })
            {
                pendingQuestions.append(PendingRequest.Question(
                    id: payload.questionId, text: payload.text,
                    options: payload.options, allowMultiple: payload.allowMultiple,
                ))
            }

        case .questionAnswered:
            if let payload = tryDecode(QuestionAnsweredPayload.self, from: event) {
                pendingQuestions.removeAll { $0.id == payload.questionId }
            }

        case .budgetUpdated:
            if let payload = tryDecode(BudgetUpdatedPayload.self, from: event) {
                usage = Usage(
                    turns: payload.turns ?? usage?.turns,
                    toolCalls: payload.toolCalls ?? usage?.toolCalls,
                    inputTokens: payload.inputTokens ?? usage?.inputTokens,
                    outputTokens: payload.outputTokens ?? usage?.outputTokens,
                    cachedInputTokens: payload.cachedInputTokens ?? usage?.cachedInputTokens,
                    contextTokens: payload.contextTokens ?? usage?.contextTokens,
                    reasoningTokens: usage?.reasoningTokens,
                    costUsd: usage?.costUsd,
                )
                // NOTE: budget.updated's context_tokens is the CUMULATIVE
                // consumption counter (statusbar in/out), not the live
                // window occupancy — feeding it into the gauge pinned it
                // at 100% after every submitted turn. Only snapshot,
                // context.usage and context.compacted move occupancy.
            }

        case .usageUpdated:
            break // budget.updated is the richer source; both fire per turn

        case .contextUsage:
            if let payload = tryDecode(ContextUsagePayload.self, from: event) {
                occupancy = payload.occupancyTokens
            }

        case .contextCompacted:
            if let payload = tryDecode(ContextCompactedPayload.self, from: event) {
                if let after = payload.estTokensAfter {
                    occupancy = after
                }
                addNotice("Context compacted (\(formatTokens(payload.estTokensBefore)) → \(formatTokens(payload.estTokensAfter)))")
            }

        case .budgetNotice:
            if let payload = tryDecode(BudgetNoticePayload.self, from: event) {
                addNotice(payload.text)
            }

        case .planUpdated:
            setPlan(tryDecode(PlanPayload.self, from: event))

        case .steerQueued:
            if let payload = tryDecode(SteerQueuedPayload.self, from: event) {
                pendingSteers.append(payload.text)
            }

        case .steerInjected:
            if let payload = tryDecode(SteerInjectedPayload.self, from: event) {
                if let index = pendingSteers.firstIndex(of: payload.text) {
                    pendingSteers.remove(at: index)
                }
                messages.append(Message(
                    id: "local-\(event.sequence)", role: .user,
                    status: .final, parts: [.text(payload.text)], createdAt: nil,
                ))
            }

        case .subagentStarted:
            if let payload = tryDecode(SubagentStartedPayload.self, from: event) {
                updateTool(payload.callId, name: "subagent") { $0.subagentTask = payload.task }
            }

        case .subagentFinished:
            if let payload = tryDecode(SubagentFinishedPayload.self, from: event) {
                updateTool(payload.callId, name: "subagent") { $0.subagentOutcome = payload.outcome }
            }

        case .subagentProgress, .toolProgress, .modelRequestRetrying:
            break // progress ticks: not rendered (yet)

        case .modelRequestFailed:
            draft?.sealAll()
            state = .idle

        case .runCancelRequested:
            state = .cancelling

        case .turnFinished:
            // The server publishes run.completed/run.cancelled BEFORE turn.finished.
            // Only this final boundary warrants a snapshot; do not spawn a task
            // that can finish after a newer turn.started and erase its draft.
            draft?.sealAll()
            state = .idle
            turnRefreshRequested = true
            onTurnActivity?()

        case .runCancelled:
            turnFeedback = .cancelled
            dismissedTurnFailure = nil

        case .runCompleted:
            break // turn.finished follows with the final canonical projection

        case .sessionClosed:
            state = .closed

        case .runtimeWarning:
            if let payload = tryDecode(RuntimeMessagePayload.self, from: event) {
                addNotice(payload.message)
            }

        case .runtimeFatal:
            if let payload = tryDecode(RuntimeMessagePayload.self, from: event) {
                if payload.message.hasPrefix("run failed") {
                    let reason = payload.message.hasPrefix("run failed: ")
                        ? String(payload.message.dropFirst("run failed: ".count)) : payload.message
                    setTurnFailure(reason)
                } else {
                    addNotice(payload.message)
                }
            }

        case .sessionOpened, .unknown:
            break
        }
    }

    // MARK: Draft helpers

    private func setPlan(_ next: PlanPayload?) {
        let active = next?.items.isEmpty == true ? nil : next
        if plan == nil, active != nil {
            planIdentity = UUID()
        }
        plan = active
    }

    private func materializeTool(_ payload: ToolPreparedPayload) {
        if draft == nil {
            draft = DraftTurn()
        }
        // Entering the tool phase = thinking is over (tool-call
        // responses may not carry response_completed — WebUI parity).
        draft?.sealReasoning()
        // Match a pending argument stream: prefer exact tool_id, else an
        // unmaterialized entry with the same tool name.
        var arguments = ""
        if let match = draft?.pendingArgs.first(where: { $0.value.toolId == payload.callId }) {
            arguments = match.value.arguments
            draft?.pendingArgs[match.key]?.materialized = true
        } else if let match = draft?.pendingArgs.first(where: {
            !$0.value.materialized && $0.value.name == payload.toolName
        }) {
            arguments = match.value.arguments
            draft?.pendingArgs[match.key]?.materialized = true
        }

        if var existing = draft?.tool(callId: payload.callId) {
            existing.name = payload.toolName
            existing.target = payload.target ?? existing.target
            existing.diff = payload.diff ?? existing.diff
            draft?.upsertTool(existing)
        } else {
            draft?.upsertTool(ToolCallState(
                id: payload.callId, name: payload.toolName, arguments: arguments,
                risk: payload.risk, target: payload.target, diff: payload.diff,
            ))
        }
    }

    private func updateTool(
        _ callId: String, name: String, mutate: (inout ToolCallState) -> Void,
    ) {
        if draft == nil {
            draft = DraftTurn()
        }
        draft?.updateTool(callId: callId, name: name, mutate: mutate)
    }

    private func tryDecode<T: Decodable>(_: T.Type, from event: RuntimeEvent) -> T? {
        guard let payload = event.payload else { return nil }
        return try? payload.decoded(as: T.self, using: LoomJSON.decoder)
    }

    private func formatTokens(_ value: Int64?) -> String {
        guard let value else { return "?" }
        if value >= 1000 {
            return String(format: "%.1fk", Double(value) / 1000)
        }
        return "\(value)"
    }
}

extension ToolCallState.Status {
    init(from wire: ToolCompletedPayload.Status) {
        switch wire {
        case .success: self = .success
        case .error: self = .error
        case .timeout: self = .timeout
        case .cancelled: self = .cancelled
        case .unknown: self = .unknown
        }
    }
}
