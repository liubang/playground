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

import AppKit
import SwiftUI

// MARK: - Message row (WebUI blocks.css)

struct MessageRow: View {
    let message: Message
    /// Cross-message call_id → result map (the WebUI's histTools):
    /// tool results live in their own assistant messages, so pairing
    /// cannot happen per-message.
    var toolResults: [String: ContentPart.ToolResult] = [:]
    /// WebUI closeTurn: the action row attaches only to the LAST text
    /// segment of a finished turn, not to every assistant message.
    var showActions = true
    /// Authenticated artifact loader (SessionStore.artifactData);
    /// artifact parts fall back to a plain label when absent.
    var artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)?

    var body: some View {
        switch message.role {
        case .user:
            userRow
        case .assistant:
            assistantRow
        case .system, .unknown:
            systemRow
        }
    }

    // MARK: User (.block-user)

    /// Right-aligned bubble (--bubble-user, asymmetric 14/14/4/14 corners,
    /// ~72% max width) with the action row below it.
    private var userRow: some View {
        VStack(alignment: .trailing, spacing: 4) {
            VStack(alignment: .leading, spacing: 8) {
                ForEach(Array(message.parts.enumerated()), id: \.offset) { _, part in
                    switch part {
                    case let .text(text):
                        Text(text)
                            .font(.system(size: 14))
                            .lineSpacing(4)
                            .foregroundStyle(Theme.fg)
                            .textSelection(.enabled)
                    case let .image(image):
                        InlineImageView(mediaType: image.mediaType, data: image.data, maxDim: 320)
                    default:
                        EmptyView()
                    }
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .background(
                Theme.bubbleUser,
                in: UnevenRoundedRectangle(
                    topLeadingRadius: 14,
                    bottomLeadingRadius: 14,
                    bottomTrailingRadius: 4,
                    topTrailingRadius: 14,
                ),
            )

            // .block-user .msg-actions: right-aligned, time left of copy.
            MessageActionsView(
                text: message.copyText,
                createdAt: message.createdAt,
                timeOnLeft: true,
            )
        }
        .frame(maxWidth: 660, alignment: .trailing)
        .frame(maxWidth: .infinity, alignment: .trailing)
    }

    // MARK: Assistant (.block-assistant + tool/notice blocks)

    private var assistantRow: some View {
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(message.assistantItems(toolResults: toolResults).enumerated()), id: \.offset) { _, item in
                switch item {
                case let .markdown(text):
                    MarkdownText(source: text)
                case let .reasoning(reasoning):
                    ReasoningBlock(
                        text: reasoning.text ?? "",
                        durationMs: reasoning.durationMs,
                        live: false,
                    )
                case let .tool(tool):
                    ToolBlock(item: tool, artifactLoader: artifactLoader)
                case let .image(image):
                    InlineImageView(mediaType: image.mediaType, data: image.data, maxDim: 360)
                case let .artifact(artifact):
                    if let artifactLoader {
                        ArtifactBlockView(artifact: artifact, loader: artifactLoader)
                    } else {
                        Label(
                            "Artifact \(artifact.id) (\(formatTokenCount(artifact.size))B)",
                            systemImage: "paperclip",
                        )
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted)
                    }
                }
            }

            if message.status == .interrupted {
                // .block-interrupted: warning text on a 9% warning wash.
                Text("interrupted")
                    .font(.system(size: Theme.textMd))
                    .foregroundStyle(Theme.warning)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(
                        Theme.warning.opacity(0.09),
                        in: RoundedRectangle(cornerRadius: Theme.radiusMd),
                    )
            }

            // .msg-actions: copy + time, the "this message is finished"
            // marker — attached by ChatView at the turn boundary only.
            if showActions, !message.copyText.isEmpty {
                MessageActionsView(
                    text: message.copyText,
                    createdAt: message.createdAt,
                    timeOnLeft: false,
                )
            }
        }
    }

    // MARK: System (.notice)

    private var systemRow: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(Array(message.parts.enumerated()), id: \.offset) { _, part in
                if case let .text(text) = part {
                    Text(text)
                        .font(.system(size: Theme.textMd).italic())
                        .foregroundStyle(Theme.muted)
                        .textSelection(.enabled)
                }
            }
        }
        .padding(.leading, 16)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// MARK: - Assistant content model (tool_call + tool_result merged)

/// A tool call merged with its result (same call_id) — the WebUI's
/// single .block-tool per invocation.
struct ToolBlockItem {
    let call: ContentPart.ToolCall?
    let result: ContentPart.ToolResult?

    var name: String {
        call?.name ?? "tool"
    }
}

enum AssistantItem {
    case markdown(String)
    case reasoning(ContentPart.Reasoning)
    case tool(ToolBlockItem)
    case image(ContentPart.ImageContent)
    case artifact(ContentPart.Artifact)
}

extension Message {
    /// Text copied by the message action row (prose parts only).
    var copyText: String {
        parts.compactMap { part in
            if case let .text(text) = part {
                return text
            }
            return nil
        }.joined(separator: "\n\n")
    }

    /// Flattens parts into render items, folding each tool_call together
    /// with the tool_result that shares its call_id. Pairing is
    /// cross-message (the WebUI's buildFromSnapshot: results live in
    /// their own assistant messages and only PATCH the block created
    /// for the call — they never create a block of their own).
    func assistantItems(toolResults: [String: ContentPart.ToolResult]) -> [AssistantItem] {
        var items: [AssistantItem] = []
        for part in parts {
            switch part {
            case let .text(text):
                if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                    items.append(.markdown(text))
                }
            case let .reasoning(reasoning):
                items.append(.reasoning(reasoning))
            case let .toolCall(call):
                items.append(.tool(ToolBlockItem(call: call, result: toolResults[call.id])))
            case .toolResult:
                // Consumed by the pairing map; an orphaned result (no
                // call anywhere) is dropped, exactly like the WebUI.
                break
            case let .image(image):
                items.append(.image(image))
            case let .artifact(artifact):
                // WebUI transcript.ts: model_only artifacts (view_image)
                // skip the display channel entirely.
                if !artifact.modelOnly {
                    items.append(.artifact(artifact))
                }
            case .unknown:
                break
            }
        }
        return items
    }
}

// MARK: - Message actions (.msg-actions)

/// The action row under a finished message: ghost copy button + short
/// time. `timeOnLeft` matches the user bubble's right-aligned variant.
struct MessageActionsView: View {
    let text: String
    let createdAt: Date?
    var timeOnLeft = false

    @State private var copied = false

    var body: some View {
        HStack(spacing: 4) {
            if timeOnLeft {
                // .block-user: right-aligned row (the caller trails it),
                // time on the left, copy on the right.
                timeTip
                copyButton
            } else {
                // Assistant (.msg-actions): left-aligned, copy then time.
                copyButton
                timeTip
            }
        }
        .font(.system(size: Theme.textXs))
        .foregroundStyle(Theme.muted)
    }

    private var copyButton: some View {
        Button(action: copy) {
            Image(systemName: copied ? "checkmark" : "doc.on.doc")
                .font(.system(size: 11))
                .foregroundStyle(copied ? Theme.success : Theme.muted)
                .frame(width: 26, height: 24)
                .background(Color.clear, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("Copy this message")
    }

    @ViewBuilder private var timeTip: some View {
        if let createdAt {
            Text(formatMessageTime(createdAt))
                .opacity(0.85)
        }
    }

    private func copy() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        copied = true
        Task {
            try? await Task.sleep(for: .seconds(1.5))
            copied = false
        }
    }
}

// MARK: - Reasoning (.block-reasoning — deliberately de-carded)

/// The reasoning header's bulb: solid + breathing while the block is
/// live, outline + muted once finalized (WebUI .block-reasoning.is-live).
private struct LightbulbIcon: View {
    let active: Bool
    @State private var dim = false

    var body: some View {
        Image(systemName: active ? "lightbulb.fill" : "lightbulb")
            .font(.system(size: 12))
            .foregroundStyle(active ? Theme.primary : Theme.muted)
            .opacity(active && dim ? 0.4 : 1)
            .onAppear {
                guard active else { return }
                withAnimation(.easeInOut(duration: 1.6).repeatForever(autoreverses: true)) {
                    dim = true
                }
            }
    }
}

struct ReasoningBlock: View {
    let text: String
    var durationMs: Int64?
    var live: Bool
    @State private var expanded = false

    /// active = live and no finalized duration (WebUI's is-live gate).
    private var active: Bool {
        live && durationMs == nil
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Button {
                withAnimation(.easeInOut(duration: 0.15)) { expanded.toggle() }
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: expanded ? "chevron.down" : "chevron.right")
                        .font(.system(size: 10))
                    LightbulbIcon(active: active)
                    Text(head)
                        .font(.system(size: Theme.textSm, weight: .medium))
                        .foregroundStyle(active ? Theme.primary : Theme.muted)
                    if !expanded, let summary = summaryLine {
                        Text(summary)
                            .font(.system(size: Theme.textSm).italic())
                            .foregroundStyle(Theme.muted)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                }
            }
            .buttonStyle(.plain)

            // Streaming tail preview: last line while collapsed.
            if active, !expanded, let tail = tailLine {
                Text(tail)
                    .font(.system(size: 12).italic())
                    .foregroundStyle(Theme.muted)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .padding(.leading, 24)
            }

            if expanded {
                ScrollView {
                    Text(text)
                        .font(.system(size: Theme.textSm))
                        .lineSpacing(3)
                        .foregroundStyle(Theme.muted)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                // Follow the reasoning stream while expanded, same
                // release-on-scroll-up semantics as the transcript.
                .defaultScrollAnchor(.bottom)
                .frame(maxHeight: 320)
                .padding(.leading, 12)
                .overlay(alignment: .leading) {
                    Rectangle().fill(Theme.bg2).frame(width: 1)
                }
                .padding(.leading, 5)
            }
        }
        .padding(.vertical, 3)
        .padding(.horizontal, 2)
    }

    private var head: String {
        if active {
            return "thinking…"
        }
        if let durationMs {
            return "thought for \(formatDuration(durationMs))"
        }
        return "reasoning"
    }

    /// First non-empty line, truncated to ~96 chars (WebUI .r-summary).
    private var summaryLine: String? {
        excerpt(fromEnd: false)
    }

    /// Last non-empty line, shown while streaming (WebUI .reasoning-tail).
    private var tailLine: String? {
        excerpt(fromEnd: true)
    }

    private func excerpt(fromEnd: Bool) -> String? {
        let lines = text.components(separatedBy: "\n")
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        guard let line = fromEnd ? lines.last : lines.first else { return nil }
        return line.count > 96 ? String(line.prefix(96)) + "…" : line
    }
}

// MARK: - Tool block (.block-tool)

/// The WebUI's tool card: bg1 panel, header row (kind icon · verb ·
/// target · status · duration), error line, collapsible output preview,
/// and an optional diff.
struct ToolBlock: View {
    /// Normalized render data, built from either a finalized
    /// ToolBlockItem (snapshot history) or a live ToolCallState (draft
    /// turn) — both paths share this one layout, so a tool card looks
    /// identical during the turn and after it (WebUI: a single
    /// ToolBlock.tsx renders the live and the rebuilt block alike).
    struct Model {
        var name = "tool"
        var target: String?
        var status = Status.running
        var durationMs: Int64?
        var errorMessage: String?
        /// Display excerpt: 600 chars + "\n…" for history; the live
        /// preview arrives pre-bounded from tool.completed.
        var output: String?
        /// Copy source (WebUI getFullText); the live path only has the
        /// bounded preview.
        var fullOutput: String?
        var diff: String?
        var images: [ContentPart.ImageContent] = []
        var artifacts: [ContentPart.Artifact] = []
    }

    enum Status {
        case running, success, failed, cancelled
    }

    private let model: Model
    /// Authenticated artifact loader for result artifacts (image
    /// results from the image tool, stdout attachments from run_cmd).
    private let artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)?

    /// Finalized history block (snapshot rebuild).
    init(
        item: ToolBlockItem,
        artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)? = nil,
    ) {
        model = Self.model(from: item)
        self.artifactLoader = artifactLoader
    }

    /// Live draft-turn block: the event stream's ToolCallState —
    /// server-provided target/diff, tool.completed's bounded preview in
    /// the same output disclosure history uses, and the completion's
    /// display-bound artifact refs (present_image renders live, without
    /// waiting for the post-turn snapshot rebuild).
    init(
        live state: ToolCallState,
        artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)? = nil,
    ) {
        var model = Model()
        model.name = state.name
        model.target = state.target
        switch state.status {
        case .prepared, .running:
            model.status = .running
        case .success, .unknown:
            model.status = .success
        case .error, .timeout:
            model.status = .failed
        case .cancelled:
            model.status = .cancelled
        }
        model.durationMs = state.durationMs
        model.errorMessage = state.errorMessage
        if let preview = state.preview, !preview.isEmpty {
            model.output = preview
            model.fullOutput = preview
        }
        model.diff = state.diff
        model.artifacts = state.artifacts
        self.model = model
        self.artifactLoader = artifactLoader
    }

    @State private var targetExpanded = false
    @State private var outputExpanded = false
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header

            if let error = model.errorMessage, !error.isEmpty {
                Text(error)
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.error)
                    .textSelection(.enabled)
                    .padding(.top, 8)
            }

            if let output = model.output, !output.isEmpty {
                outputDisclosure(output)
                    .padding(.top, 8)
            }

            // Result media (WebUI ToolBlock): inline base64 images
            // first; the artifact path is used only when there are no
            // inline images. An artifact is not necessarily an image
            // (run_cmd's stdout artifact is text) — the block dispatches
            // by media type.
            ForEach(model.images.indices, id: \.self) { index in
                InlineImageView(
                    mediaType: model.images[index].mediaType,
                    data: model.images[index].data,
                    maxDim: 360,
                )
                .padding(.top, 8)
            }
            if let artifactLoader {
                ForEach(model.artifacts.indices, id: \.self) { index in
                    ArtifactBlockView(artifact: model.artifacts[index], loader: artifactLoader)
                        .padding(.top, 8)
                }
            }

            if let diff = model.diff, !diff.isEmpty {
                DiffView(diff: diff)
                    .padding(.top, 8)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: 10))
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
    }

    // MARK: Header (.tool-head)

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: ToolMeta.icon(for: model.name))
                .font(.system(size: 12))
                .foregroundStyle(Theme.muted)
                .help(model.name)

            Text(ToolMeta.verb(for: model.name))
                .font(.system(size: Theme.textSm, weight: .semibold, design: .monospaced))
                .foregroundStyle(Theme.fg)
                .fixedSize()

            if let target = model.target, !target.isEmpty {
                Text(target)
                    .font(Theme.monoSm)
                    .foregroundStyle(Theme.muted)
                    .lineLimit(targetExpanded ? nil : 1)
                    .truncationMode(.tail)
                    .onTapGesture { targetExpanded.toggle() }
                    .help(target)
            }

            statusView
                .fixedSize()

            Spacer(minLength: 4)

            if let durationMs = model.durationMs {
                Text(formatDuration(durationMs))
                    .font(Theme.monoSm)
                    .foregroundStyle(Theme.muted)
                    .fixedSize()
            }
        }
        .font(.system(size: Theme.textSm))
    }

    /// Success is icon-only (the common case — repeated "Succeeded"
    /// labels are noise); failure and cancellation keep icon + label.
    @ViewBuilder private var statusView: some View {
        switch model.status {
        case .running:
            HStack(spacing: 4) {
                PulsingDot(color: Theme.success, period: 1.2)
                Text("Running")
            }
            .font(.system(size: Theme.textSm, weight: .semibold))
            .foregroundStyle(Theme.success)
        case .success:
            Image(systemName: "checkmark")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Theme.success)
                .help("Succeeded")
        case .failed:
            HStack(spacing: 4) {
                Image(systemName: "xmark")
                Text("Failed")
            }
            .font(.system(size: Theme.textSm, weight: .semibold))
            .foregroundStyle(Theme.error)
        case .cancelled:
            HStack(spacing: 4) {
                Image(systemName: "nosign")
                Text("Cancelled")
            }
            .font(.system(size: Theme.textSm, weight: .semibold))
            .foregroundStyle(Theme.muted)
        }
    }

    // MARK: Output (.tool-output disclosure)

    private func outputDisclosure(_ output: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Button {
                withAnimation(.easeInOut(duration: 0.15)) { outputExpanded.toggle() }
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: outputExpanded ? "chevron.down" : "chevron.right")
                        .font(.system(size: 9))
                    Text("Output · \(output.count) chars\(output.hasSuffix("\n…") ? " · truncated" : "")")
                        .font(.system(size: 12))
                    Spacer()
                    Button(action: copyOutput) {
                        Text(copied ? "✓ Copied" : "Copy")
                            .font(.system(size: Theme.textXs))
                            .foregroundStyle(copied ? Theme.success : Theme.muted)
                            .padding(.horizontal, 10)
                            .padding(.vertical, 1)
                            .overlay(
                                RoundedRectangle(cornerRadius: 5)
                                    .strokeBorder(copied ? Theme.success : Theme.bg2, lineWidth: 1),
                            )
                    }
                    .buttonStyle(.plain)
                    .help("Copy full output")
                }
                .foregroundStyle(Theme.muted)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            if outputExpanded {
                ScrollView {
                    Text(output)
                        .font(Theme.monoSm)
                        .lineSpacing(2)
                        .foregroundStyle(Theme.fg)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                }
                .frame(maxHeight: 200)
                .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
            }
        }
    }

    /// Copy always uses the FULL output (WebUI getFullText), not the
    /// bounded display excerpt.
    private func copyOutput() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(model.fullOutput ?? "", forType: .string)
        copied = true
        Task {
            try? await Task.sleep(for: .seconds(1.5))
            copied = false
        }
    }

    // MARK: Model derivation (snapshot history)

    /// Builds the render model from a finalized call+result pair —
    /// one-to-one with the WebUI's histTarget/histCompletion and the
    /// lazy diffForToolCall.
    private static func model(from item: ToolBlockItem) -> Model {
        var model = Model()
        model.name = item.name
        if let result = item.result {
            if result.error != nil {
                model.status = .failed
            } else {
                switch result.status {
                case "success", "ok", "done": model.status = .success
                case "cancelled", "canceled": model.status = .cancelled
                case "error", "timeout", "failed": model.status = .failed
                default: model.status = .success
                }
            }
            model.errorMessage = result.error?.message
            // WebUI histCompletion: duration = finished_at − started_at.
            model.durationMs = result.durationMs
        }
        model.target = displayTarget(of: item)
        if let full = fullOutputText(of: item) {
            model.fullOutput = full
            // Display excerpt (WebUI histCompletion: 600 chars + "\n…").
            model.output = full.count > 600 ? String(full.prefix(600)) + "\n…" : full
        }
        model.images = resultImages(of: item)
        model.artifacts = resultArtifacts(of: item, images: model.images)
        model.diff = diffText(of: item)
        return model
    }

    /// Base64 image parts inside the tool result's content.
    private static func resultImages(of item: ToolBlockItem) -> [ContentPart.ImageContent] {
        guard let content = item.result?.content else { return [] }
        return content.compactMap { part in
            if case let .image(image) = part {
                return image
            }
            return nil
        }
    }

    /// Artifact references inside the result content — only rendered
    /// when the result carries no inline images (WebUI precedence).
    /// model_only artifacts (view_image) are for the model only: the
    /// WebUI's histCompletion filters them out, and so do we.
    private static func resultArtifacts(
        of item: ToolBlockItem, images: [ContentPart.ImageContent],
    ) -> [ContentPart.Artifact] {
        guard images.isEmpty, let content = item.result?.content else { return [] }
        return content.compactMap { part in
            if case let .artifact(artifact) = part, !artifact.modelOnly {
                return artifact
            }
            return nil
        }
    }

    /// Full output text: the result's text parts joined, falling back
    /// to the call's arguments while the call is still running.
    private static func fullOutputText(of item: ToolBlockItem) -> String? {
        if let content = item.result?.content {
            let text = content.compactMap { part -> String? in
                if case let .text(text) = part {
                    return text
                }
                return nil
            }.joined(separator: "\n")
            if !text.isEmpty {
                return text
            }
        }
        if item.result == nil, let args = item.call?.arguments?.prettyPrinted, !args.isEmpty {
            return args
        }
        return nil
    }

    /// Header target: the canonical file path / command from the args.
    private static func displayTarget(of item: ToolBlockItem) -> String? {
        guard let args = item.call?.arguments else { return nil }
        for key in ["path", "file_path", "cmd", "command", "pattern", "query", "goal", "task"] {
            if let value = args[key]?.stringValue, !value.isEmpty {
                return value.count > 120 ? String(value.prefix(120)) + "…" : value
            }
        }
        return nil
    }

    /// Snapshot rebuilds carry no diff; recompute one from edit/write
    /// args — one-to-one with the WebUI's diffForToolCall (diff.ts):
    /// write = pure addition; edit = two-sided LCS (400-line cap per
    /// side), and the `+++ b/{path}` header feeds the DiffView's file
    /// label and highlight language.
    private static func diffText(of item: ToolBlockItem) -> String? {
        guard let name = item.call?.name, let args = item.call?.arguments else { return nil }
        let text: String
        switch name {
        case "edit":
            guard let newString = args["new_string"]?.stringValue else { return nil }
            text = Self.diffTexts(args["old_string"]?.stringValue ?? "", newString)
        case "write":
            guard let content = args["content"]?.stringValue else { return nil }
            text = Self.diffTexts("", content)
        default:
            return nil
        }
        guard !text.isEmpty else { return nil }
        if let path = args["path"]?.stringValue ?? args["file_path"]?.stringValue, !path.isEmpty {
            return "+++ b/\(path)\n" + text
        }
        return text
    }

    /// diff.ts DIFF_MAX_INPUT_LINES.
    private static let diffMaxInputLines = 400

    private static func splitDiffLines(_ text: String) -> [String] {
        guard !text.isEmpty else { return [] }
        var text = text
        if text.hasSuffix("\n") {
            text.removeLast()
        }
        return text.components(separatedBy: "\n")
    }

    private struct DiffOp {
        enum Kind { case ctx, del, add }
        let kind: Kind
        let line: String
    }

    /// Line-level LCS diff — the same DP and backtrack direction as
    /// diff.ts (del wins ties), so both clients render identical hunks.
    private static func lcsDiff(_ oldLines: [String], _ newLines: [String]) -> [DiffOp] {
        let n = oldLines.count
        let m = newLines.count
        var dp = [[Int]](repeating: [Int](repeating: 0, count: m + 1), count: n + 1)
        if n > 0, m > 0 {
            for i in stride(from: n - 1, through: 0, by: -1) {
                for j in stride(from: m - 1, through: 0, by: -1) {
                    dp[i][j] = oldLines[i] == newLines[j]
                        ? dp[i + 1][j + 1] + 1
                        : max(dp[i + 1][j], dp[i][j + 1])
                }
            }
        }
        var ops: [DiffOp] = []
        var i = 0
        var j = 0
        while i < n, j < m {
            if oldLines[i] == newLines[j] {
                ops.append(DiffOp(kind: .ctx, line: oldLines[i]))
                i += 1
                j += 1
            } else if dp[i + 1][j] >= dp[i][j + 1] {
                ops.append(DiffOp(kind: .del, line: oldLines[i]))
                i += 1
            } else {
                ops.append(DiffOp(kind: .add, line: newLines[j]))
                j += 1
            }
        }
        while i < n {
            ops.append(DiffOp(kind: .del, line: oldLines[i]))
            i += 1
        }
        while j < m {
            ops.append(DiffOp(kind: .add, line: newLines[j]))
            j += 1
        }
        return ops
    }

    /// diff.ts diffTexts: changed lines keep 1 line of context above
    /// and below; unchanged runs collapse into "...".
    private static func diffTexts(_ oldText: String, _ newText: String) -> String {
        if oldText == newText {
            return ""
        }
        let ops: [DiffOp] = if oldText.isEmpty || newText.isEmpty {
            splitDiffLines(oldText).map { DiffOp(kind: .del, line: $0) }
                + splitDiffLines(newText).map { DiffOp(kind: .add, line: $0) }
        } else {
            lcsDiff(
                Array(splitDiffLines(oldText).prefix(diffMaxInputLines)),
                Array(splitDiffLines(newText).prefix(diffMaxInputLines)),
            )
        }
        var show = [Bool](repeating: false, count: ops.count)
        for (index, op) in ops.enumerated() where op.kind != .ctx {
            show[index] = true
            if index > 0 {
                show[index - 1] = true
            }
            if index + 1 < ops.count {
                show[index + 1] = true
            }
        }
        var out: [String] = []
        var skipped = false
        for (index, op) in ops.enumerated() {
            guard show[index] else {
                skipped = true
                continue
            }
            if skipped, !out.isEmpty {
                out.append("...")
            }
            skipped = false
            let prefix = switch op.kind {
            case .ctx: "  "
            case .del: "- "
            case .add: "+ "
            }
            out.append(prefix + op.line)
        }
        return out.joined(separator: "\n")
    }
}

// MARK: - Tool kind metadata (WebUI TOOL_META)

enum ToolMeta {
    static func icon(for toolName: String) -> String {
        if toolName.hasPrefix("mcp__") {
            return "cable.connector"
        }
        switch toolName {
        case "read_file": return "eye"
        case "list_dir": return "folder"
        case "glob", "search", "search_text", "web_search": return "magnifyingglass"
        case "edit": return "square.and.pencil"
        case "write": return "doc"
        case "run_cmd", "exec_session", "write_stdin": return "terminal"
        case "delegate_task": return "cpu"
        case "update_plan": return "checkmark.square"
        case "update_goal": return "star"
        case "view_image", "present_image", "generate_image": return "photo"
        case "web_fetch": return "arrow.down.circle"
        case "kb_search", "kb_read": return "externaldrive"
        case "read_skill": return "puzzlepiece"
        default: return "gearshape"
        }
    }

    static func verb(for toolName: String) -> String {
        if toolName.hasPrefix("mcp__") {
            let stripped = String(toolName.dropFirst(5))
            return stripped.isEmpty ? "mcp" : stripped
        }
        switch toolName {
        case "read_file": return "read"
        case "list_dir": return "list"
        case "glob": return "glob"
        case "search", "search_text": return "search"
        case "edit": return "edit"
        case "write": return "write"
        case "run_cmd": return "run"
        case "exec_session": return "exec"
        case "write_stdin": return "stdin"
        case "delegate_task": return "delegate"
        case "update_plan": return "plan"
        case "update_goal": return "goal"
        case "view_image": return "view image"
        case "present_image": return "image"
        case "generate_image": return "generate image"
        case "web_fetch": return "fetch"
        case "web_search": return "web search"
        case "kb_search": return "kb search"
        case "kb_read": return "kb read"
        case "read_skill": return "skill"
        default: return toolName.isEmpty ? "tool" : toolName
        }
    }
}

// MARK: - Artifact blocks (.block-artifact)

/// Artifact reference, dispatched by media type (WebUI ArtifactBlock):
/// images render inline (click to zoom), text-like artifacts get an
/// expandable mono preview + download, other binaries a download row.
struct ArtifactBlockView: View {
    let artifact: ContentPart.Artifact
    let loader: (ContentPart.Artifact) async -> (data: Data, mediaType: String?)?

    @State private var entry: (data: Data, mediaType: String?)?
    @State private var failed = false
    @State private var expanded = false

    private var mediaType: String {
        artifact.mediaType ?? entry?.mediaType ?? ""
    }

    private var isImage: Bool {
        mediaType.hasPrefix("image/")
    }

    private var isText: Bool {
        mediaType.hasPrefix("text/") || mediaType == "application/json"
    }

    var body: some View {
        content
            .task {
                guard entry == nil, !failed else { return }
                if let loaded = await loader(artifact) {
                    entry = loaded
                } else {
                    failed = true
                }
            }
    }

    @ViewBuilder private var content: some View {
        if isImage {
            imageContent
        } else {
            fileContent
        }
    }

    // MARK: image/* → inline image (+ zoom sheet)

    @ViewBuilder private var imageContent: some View {
        if failed {
            ArtifactNotice(text: "Image failed to load")
        } else if let data = entry?.data, let image = NSImage(data: data) {
            Button {
                NotificationCenter.default.post(name: .loomZoomImage, object: image)
            } label: {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: 360, maxHeight: 360)
                    .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMd))
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.radiusMd)
                            .strokeBorder(Theme.bg2, lineWidth: 1),
                    )
            }
            .buttonStyle(.plain)
            .help("Click to enlarge")
        } else {
            // Loading: an empty block, like the WebUI's placeholder.
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .fill(Theme.bg1)
                .frame(width: 240, height: 120)
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.radiusMd)
                        .strokeBorder(Theme.bg2, lineWidth: 1),
                )
        }
    }

    // MARK: non-image → summary row (+ text preview / download)

    private var fileContent: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                if isText {
                    Image(systemName: expanded ? "chevron.down" : "chevron.right")
                        .font(.system(size: 9))
                        .foregroundStyle(Theme.muted)
                        .frame(width: 12)
                }
                Text("\(isText ? "output attachment" : "attachment") · \(mediaType.isEmpty ? "binary" : mediaType) · \(Self.formatBytes(artifact.size ?? Int64(entry?.data.count ?? 0)))")
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.fg)
                Spacer()
                if let data = entry?.data {
                    Button("Download") { save(data: data) }
                        .font(.system(size: Theme.textXs))
                        .buttonStyle(.plain)
                        .foregroundStyle(Theme.primary)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .contentShape(Rectangle())
            .onTapGesture {
                if isText {
                    expanded.toggle()
                }
            }

            if isText, expanded, let data = entry?.data {
                Text(String(decoding: data, as: UTF8.self).prefix(8000))
                    .font(Theme.monoSm)
                    .foregroundStyle(Theme.fg)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .overlay(alignment: .top) {
                        Hairline(axis: .horizontal)
                    }
            }
        }
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
    }

    private func save(data: Data) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = artifact.id
            .replacingOccurrences(of: "art_sha256_", with: "artifact-")
            .prefix(24) + Self.fileExtension(for: mediaType)
        if panel.runModal() == .OK, let url = panel.url {
            try? data.write(to: url)
        }
    }

    private static func fileExtension(for mediaType: String) -> String {
        switch mediaType {
        case "image/png": ".png"
        case "image/jpeg": ".jpg"
        case "image/gif": ".gif"
        case "image/webp": ".webp"
        case "application/json": ".json"
        case let t where t.hasPrefix("text/"): ".txt"
        default: ""
        }
    }

    /// fmtBytes (WebUI lib/format): 741B · 189kB · 2.3MB.
    private static func formatBytes(_ value: Int64) -> String {
        if value < 1024 {
            return "\(value)B"
        }
        if value < 1024 * 1024 {
            return "\(value / 1024)kB"
        }
        return String(format: "%.1fMB", Double(value) / 1_048_576)
    }
}

/// The warn notice shown when an artifact/image fails to load.
private struct ArtifactNotice: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: Theme.textSm))
            .foregroundStyle(Theme.warning)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(
                Theme.warning.opacity(0.09),
                in: RoundedRectangle(cornerRadius: Theme.radiusMd),
            )
    }
}

extension Notification.Name {
    /// Posted by any zoomable image (object: NSImage) — RootView hosts
    /// the singleton lightbox overlay, like the WebUI's module-level
    /// lightbox element.
    static let loomZoomImage = Notification.Name("loom.zoomImage")
}

/// Window-level image lightbox (WebUI images.tsx): a dim overlay over
/// the whole window — backdrop click, the × button, or Esc all close
/// it; the image fills ~86% of the screen (scaled down when larger,
/// scaled up for small bitmaps, capped at 2.5x to avoid mush).
struct ImageLightboxView: View {
    let image: NSImage
    let close: () -> Void

    @State private var copied = false
    @FocusState private var focused: Bool

    var body: some View {
        let screen = NSScreen.main?.visibleFrame.size ?? NSSize(width: 1512, height: 945)
        let imageSize = image.size.width > 1 ? image.size : NSSize(width: 800, height: 600)
        let scale = min(
            screen.width * 0.86 / imageSize.width,
            screen.height * 0.86 / imageSize.height,
            2.5,
        )
        ZStack(alignment: .topTrailing) {
            // Backdrop: a click anywhere off the chrome closes (the
            // WebUI's overlay.onclick), and it swallows every hit so
            // the transcript below is inert while zoomed.
            Theme.bg0.opacity(0.92)
                .contentShape(Rectangle())
                .onTapGesture { close() }

            Image(nsImage: image)
                .resizable()
                .interpolation(.high)
                .aspectRatio(contentMode: .fit)
                .frame(width: imageSize.width * scale, height: imageSize.height * scale)
                .clipShape(RoundedRectangle(cornerRadius: Theme.radiusSm))
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.radiusSm)
                        .strokeBorder(Theme.bg2, lineWidth: 1),
                )
                .shadow(color: .black.opacity(0.5), radius: 24)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .onTapGesture { close() }

            HStack(spacing: 8) {
                Button(action: copy) {
                    Image(systemName: copied ? "checkmark" : "doc.on.doc")
                }
                .help("Copy image (⌘C)")

                Button(action: close) {
                    Image(systemName: "xmark")
                }
                .help("Close (Esc)")
            }
            .font(.system(size: 13))
            .foregroundStyle(copied ? Theme.success : Theme.muted)
            .buttonStyle(.plain)
            .padding(8)
            .background(Theme.bg1, in: Capsule())
            .overlay(Capsule().strokeBorder(Theme.bg2, lineWidth: 1))
            .padding(20)
        }
        .transition(.opacity)
        .focusable()
        .focusEffectDisabled()
        .focused($focused)
        .onAppear { focused = true }
        .onKeyPress(.escape) {
            close()
            return .handled
        }
        .onKeyPress(phases: .down) { press in
            guard press.modifiers.contains(.command), press.key == .init("c")
            else { return .ignored }
            copy()
            return .handled
        }
    }

    private func copy() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.writeObjects([image])
        copied = true
        Task {
            try? await Task.sleep(for: .seconds(1.5))
            copied = false
        }
    }
}

// MARK: - Inline images

struct InlineImageView: View {
    let mediaType: String
    let data: String
    var maxDim: CGFloat = 320

    var body: some View {
        if let image = decodedImage {
            // Zoomable like the WebUI's InlineImage (zoomableProps).
            Button {
                NotificationCenter.default.post(name: .loomZoomImage, object: image)
            } label: {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: maxDim, maxHeight: maxDim)
                    .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMd))
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.radiusMd)
                            .strokeBorder(Theme.bg2, lineWidth: 1),
                    )
            }
            .buttonStyle(.plain)
            .help("Click to enlarge")
        } else {
            Label("Image attachment", systemImage: "photo")
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.muted)
        }
    }

    private var decodedImage: NSImage? {
        guard let data = Data(base64Encoded: data) else { return nil }
        return NSImage(data: data)
    }
}

// MARK: - Diff (.diff — tinted add/del rows with a sign gutter)

/// diff.ts parseDiff: the `+++ b/…` header supplies the file label;
/// unmatched lines degrade to context (lenient, same as the WebUI).
struct ParsedDiff {
    enum Kind { case hunk, add, del, ctx }

    struct Line {
        let kind: Kind
        let sign: String
        let text: String
    }

    var file = ""
    var lines: [Line] = []
    var adds = 0
    var dels = 0
}

func parseDiff(_ diffText: String) -> ParsedDiff {
    var out = ParsedDiff()
    var sawContent = false
    for raw in diffText.components(separatedBy: "\n") {
        if raw.hasPrefix("+++ ") {
            var file = String(raw.dropFirst(4))
            if file.hasPrefix("b/") {
                file.removeFirst(2)
            }
            out.file = file
            continue
        }
        if raw.hasPrefix("--- ") || raw.hasPrefix("diff ") || raw.hasPrefix("index ") {
            continue
        }
        if raw.hasPrefix("@@") {
            out.lines.append(ParsedDiff.Line(kind: .hunk, sign: "", text: raw))
            sawContent = true
            continue
        }
        var kind: ParsedDiff.Kind = .ctx
        var sign = " "
        var text = raw
        if raw.hasPrefix("+") {
            kind = .add
            sign = "+"
            text = String(raw.dropFirst())
            out.adds += 1
        } else if raw.hasPrefix("-") {
            kind = .del
            sign = "−"
            text = String(raw.dropFirst())
            out.dels += 1
        } else if raw.hasPrefix(" ") {
            text = String(raw.dropFirst())
        } else if raw.isEmpty, !sawContent {
            continue
        }
        out.lines.append(ParsedDiff.Line(kind: kind, sign: sign, text: text))
        sawContent = true
    }
    return out
}

/// Unified diff rendering (WebUI DiffView): diffs longer than
/// DIFF_COLLAPSE_LINES collapse into a disclosure whose head carries
/// the file and the `N lines · +adds −dels` stat; short diffs render
/// flat under the same .d-head strip.
struct DiffView: View {
    let diff: String

    /// WebUI DIFF_COLLAPSE_LINES (diff.ts).
    private static let collapseLines = 30

    @State private var expanded = false

    private var parsed: ParsedDiff {
        parseDiff(diff)
    }

    var body: some View {
        let parsed = parsed
        VStack(alignment: .leading, spacing: 0) {
            if parsed.lines.count > Self.collapseLines {
                Button {
                    withAnimation(.easeInOut(duration: 0.15)) { expanded.toggle() }
                } label: {
                    head(parsed, collapsible: true)
                }
                .buttonStyle(.plain)
                if expanded {
                    bodyLines(parsed)
                }
            } else {
                head(parsed, collapsible: false)
                bodyLines(parsed)
            }
        }
        .background(Theme.bg1)
        .clipShape(RoundedRectangle(cornerRadius: Theme.radiusSm))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusSm)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
    }

    /// .d-head: bg2 strip; the collapsible variant adds the disclosure
    /// chevron and the trailing line/+/− stat.
    private func head(_ parsed: ParsedDiff, collapsible: Bool) -> some View {
        HStack(spacing: 8) {
            if collapsible {
                Image(systemName: expanded ? "chevron.down" : "chevron.right")
                    .font(.system(size: 9))
            }
            Text(parsed.file.isEmpty ? "diff" : parsed.file)
                .lineLimit(1)
                .truncationMode(.middle)
            if collapsible {
                Spacer(minLength: 8)
                Text("\(parsed.lines.count) lines · +\(parsed.adds) −\(parsed.dels)")
                    .fixedSize()
            }
        }
        .font(.system(size: 12, design: .monospaced))
        .foregroundStyle(Theme.muted)
        .padding(.horizontal, 12)
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.bg2)
        .contentShape(Rectangle())
    }

    private func bodyLines(_ parsed: ParsedDiff) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(parsed.lines.enumerated()), id: \.offset) { _, line in
                    DiffLineView(line: line)
                }
            }
            .padding(.vertical, 6)
        }
    }

    private struct DiffLineView: View {
        let line: ParsedDiff.Line

        var body: some View {
            Group {
                if line.kind == .hunk {
                    Text(line.text)
                        .foregroundStyle(Theme.purple)
                        .padding(.horizontal, 12)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Theme.bg1)
                } else {
                    HStack(spacing: 0) {
                        Text(line.sign)
                            .frame(width: 28, alignment: .center)
                            .foregroundStyle(signColor)
                        Text(line.text.isEmpty ? " " : line.text)
                            .foregroundStyle(textColor)
                        Spacer(minLength: 12)
                    }
                    .background(background)
                }
            }
            .font(Theme.monoSm)
            .lineSpacing(1.5)
        }

        private var signColor: Color {
            switch line.kind {
            case .add: Theme.success
            case .del: Theme.error
            default: Theme.muted
            }
        }

        private var textColor: Color {
            line.kind == .hunk ? Theme.purple : Theme.fg
        }

        private var background: Color {
            switch line.kind {
            case .add: Theme.success.opacity(0.12)
            case .del: Theme.error.opacity(0.12)
            case .hunk: Theme.bg1
            case .ctx: .clear
            }
        }
    }
}

// MARK: - Live draft (streaming turn)

struct DraftView: View {
    let draft: DraftTurn
    /// Authenticated artifact loader for live result artifacts
    /// (present_image's display-bound refs on tool.completed).
    var artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            // Segments arrive in chronological order (WebUI transcript
            // blocks): reasoning cards interleave with tool cards, and
            // the SAME ReasoningBlock/ToolBlock/MarkdownText history
            // uses renders them — the live turn lays out exactly like
            // the rebuilt history.
            ForEach(draft.segments) { segment in
                switch segment {
                case let .reasoning(reasoning):
                    ReasoningBlock(
                        text: reasoning.text,
                        durationMs: reasoning.durationMs,
                        live: reasoning.live,
                    )
                case let .text(text):
                    if !text.text.isEmpty {
                        MarkdownText(source: text.text + (text.live ? " ▍" : ""))
                    }
                case let .tool(tool):
                    ToolBlock(live: tool, artifactLoader: artifactLoader)
                }
            }
        }
    }
}
