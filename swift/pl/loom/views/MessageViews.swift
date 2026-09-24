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

/// Renders one precomputed TranscriptModel.MessageRowModel. All
/// derivation (tool call/result pairing, diff computation, the
/// action-row turn gate) happened in TranscriptModel.build — body
/// evaluation here is pure layout, so streaming frames stay cheap.
struct MessageRow: View {
    let row: TranscriptModel.MessageRowModel
    /// Authenticated artifact loader (SessionStore.artifactData);
    /// artifact parts fall back to a plain label when absent.
    var artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)?
    var hidesInterruptedStatus = false

    private var message: Message {
        row.message
    }

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
                            .lineSpacing(5.5) // ≈ the WebUI's 1.6 line-height at 14px
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
            ForEach(Array(row.items.enumerated()), id: \.offset) { _, item in
                switch item {
                case let .markdown(text):
                    MarkdownText(source: text)
                case let .reasoning(reasoning):
                    ReasoningBlock(
                        text: reasoning.text ?? "",
                        durationMs: reasoning.durationMs,
                        live: false,
                    )
                case let .tool(model):
                    ToolBlock(model: model, artifactLoader: artifactLoader)
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

            if message.status == .interrupted, !hidesInterruptedStatus {
                Label("Response interrupted", systemImage: "exclamationmark.circle")
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.muted)
            }

            // .msg-actions: copy + time, the "this message is finished"
            // marker — attached at the turn boundary only (precomputed).
            if row.showActions, !message.copyText.isEmpty {
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

extension MessageRow: Equatable {
    /// The body depends only on the precomputed render model: the
    /// artifact loader is a stable behavior closure (SessionStore's
    /// artifactData) whose closure IDENTITY changes on every ChatView
    /// rebuild — comparing it would defeat the memoization. With
    /// `.equatable()` SwiftUI skips body evaluation for history rows
    /// untouched by a streaming frame (previously every 40ms delta
    /// flush re-evaluated every row in the transcript).
    static func == (lhs: MessageRow, rhs: MessageRow) -> Bool {
        lhs.row == rhs.row && lhs.hidesInterruptedStatus == rhs.hidesInterruptedStatus
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
    @State private var hovered = false

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
                // .msg-action:hover: fg glyph on a bg2 wash.
                .foregroundStyle(copied ? Theme.success : (hovered ? Theme.fg : Theme.muted))
                .frame(width: 26, height: 24)
                .background(
                    hovered ? Theme.bg2 : Color.clear,
                    in: RoundedRectangle(cornerRadius: Theme.radiusSm),
                )
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
        .help("Copy this message")
        .accessibilityLabel("Copy this message")
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
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        Image(systemName: active ? "lightbulb.fill" : "lightbulb")
            .font(.system(size: 12))
            .foregroundStyle(active ? Theme.primary : Theme.muted)
            .opacity(active && dim && !reduceMotion ? 0.4 : 1)
            .onAppear {
                guard active, !reduceMotion else { return }
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
                ScrollViewReader { proxy in
                    ScrollView {
                        Text(text)
                            .font(.system(size: Theme.textSm))
                            .lineSpacing(5) // ≈ the WebUI's 1.6 line-height (.block-reasoning .body)
                            .foregroundStyle(Theme.muted)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        Color.clear.frame(height: 1).id("reasoning-bottom")
                    }
                    .defaultScrollAnchor(.bottom)
                    .onChange(of: text.count) { _, _ in
                        if active {
                            proxy.scrollTo("reasoning-bottom", anchor: .bottom)
                        }
                    }
                }
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

    /// Scans from the requested end and stops at the first non-empty
    /// line — the previous implementation trimmed and filtered EVERY
    /// line on every body evaluation (per streaming frame).
    private func excerpt(fromEnd: Bool) -> String? {
        let lines = text.components(separatedBy: "\n")
        let ordered = fromEnd ? Array(lines.reversed()) : lines
        for raw in ordered {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if !line.isEmpty {
                return line.count > 96 ? String(line.prefix(96)) + "…" : line
            }
        }
        return nil
    }
}

// MARK: - Tool block (.block-tool)

/// The WebUI's tool card: bg1 panel, header row (kind icon · verb ·
/// target · status · duration), error line, collapsible output preview,
/// and an optional diff. Pure layout — the render model arrives fully
/// derived (ToolRenderModel, built by TranscriptModel or the live
/// draft path), so a tool card looks identical during the turn and
/// after it, and body evaluation never recomputes the diff.
struct ToolBlock: View {
    private let model: ToolRenderModel
    /// Authenticated artifact loader for result artifacts (image
    /// results from the image tool, stdout attachments from run_cmd).
    private let artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)?

    init(
        model: ToolRenderModel,
        artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)? = nil,
    ) {
        self.model = model
        self.artifactLoader = artifactLoader
    }

    /// Live draft-turn block: converts the event stream's ToolCallState
    /// through the same ToolRenderModel history uses (WebUI: a single
    /// ToolBlock.tsx renders the live and the rebuilt block alike).
    init(
        live state: ToolCallState,
        artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)? = nil,
    ) {
        self.init(model: ToolRenderModel(live: state), artifactLoader: artifactLoader)
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
            HStack(spacing: 8) {
                Button {
                    withAnimation(.easeInOut(duration: 0.15)) { outputExpanded.toggle() }
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: outputExpanded ? "chevron.down" : "chevron.right")
                            .font(.system(size: 9))
                        Text(model.commandOutputFormatted
                            ? "Output · preview\(model.commandOutputTruncated ? " · truncated" : "")"
                            : "Output · \(output.count) chars\(output.hasSuffix("\n…") ? " · truncated" : "")")
                            .font(.system(size: 12))
                        Spacer()
                    }
                    .foregroundStyle(Theme.muted)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)

                Button(action: copyOutput) {
                    Text(copied ? "✓ Copied" : (model.commandOutputFormatted ? "Copy preview" : "Copy"))
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(copied ? Theme.success : Theme.muted)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 1)
                        .contentShape(Rectangle())
                        .overlay(
                            RoundedRectangle(cornerRadius: 5)
                                .strokeBorder(copied ? Theme.success : Theme.bg2, lineWidth: 1),
                        )
                }
                .buttonStyle(.plain)
                .help(model.commandOutputFormatted ? "Copy command preview; use the attachment to copy the original stream" : "Copy full output")
            }

            if outputExpanded {
                ScrollView {
                    Text(output)
                        .font(Theme.monoSm)
                        .lineSpacing(4.5) // ≈ the WebUI's 1.55 line-height (.tool-preview)
                        .foregroundStyle(Theme.fg)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                }
                .frame(maxHeight: 200)
                .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
                if let artifactLoader {
                    ForEach(model.outputAttachments, id: \.name) { entry in
                        RunCmdAttachmentView(name: entry.name, artifact: entry.artifact, loader: artifactLoader)
                    }
                }
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
}

/// Original command stream controls live inside the single output disclosure.
private struct RunCmdAttachmentView: View {
    let name: String
    let artifact: ContentPart.Artifact
    let loader: (ContentPart.Artifact) async -> (data: Data, mediaType: String?)?

    @State private var data: Data?
    @State private var expanded = false
    @State private var failed = false
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 8) {
                Button {
                    expanded.toggle()
                    if expanded {
                        load()
                    }
                } label: {
                    HStack(spacing: 6) {
                        Image(systemName: expanded ? "chevron.down" : "chevron.right")
                        Text("\(name) · full output · \(artifact.size.map(String.init) ?? "?")B")
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                Spacer()
                Button(copied ? "Copied" : "Copy full") {
                    load { bytes in
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(String(decoding: bytes, as: UTF8.self), forType: .string)
                        copied = true
                    }
                }
                .buttonStyle(.plain)
                Button("Download") {
                    load { bytes in
                        let panel = NSSavePanel()
                        panel.nameFieldStringValue = "\(name).txt"
                        if panel.runModal() == .OK, let url = panel.url {
                            try? bytes.write(to: url)
                        }
                    }
                }
                .buttonStyle(.plain)
            }
            .font(.system(size: Theme.textXs))
            .foregroundStyle(Theme.muted)
            if failed {
                Text("Output attachment failed to load").foregroundStyle(Theme.warning)
            }
            if expanded, let data {
                ScrollView {
                    Text(String(decoding: data, as: UTF8.self).prefix(8000))
                        .font(Theme.monoSm)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 200)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
    }

    private func load(_ onSuccess: ((Data) -> Void)? = nil) {
        if let data {
            onSuccess?(data); return
        }
        Task {
            if let entry = await loader(artifact) {
                data = entry.data
                onSuccess?(entry.data)
            } else {
                failed = true
            }
        }
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
                if isText, !failed {
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
                if isText, !failed {
                    expanded.toggle()
                }
            }

            if failed {
                ArtifactNotice(text: "Attachment failed to load")
            } else if isText, expanded, let data = entry?.data {
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
/// it. The image fits ~86% of the WINDOW (scaled down when larger,
/// scaled up for small bitmaps, capped at 2.5x to avoid mush); pinch /
/// ctrl-scroll zooms to 4x, and dragging pans while zoomed. Sizing is
/// relative to the lightbox container, never the screen — the earlier
/// screen-based fit let the image flood a small window.
struct ImageLightboxView: View {
    let image: NSImage
    let close: () -> Void

    @State private var copied = false
    @State private var zoom: CGFloat = 1
    @State private var pan: CGSize = .zero
    @FocusState private var focused: Bool

    var body: some View {
        GeometryReader { proxy in
            let imageSize = image.size.width > 1 ? image.size : NSSize(width: 800, height: 600)
            let scale = min(
                proxy.size.width * 0.86 / imageSize.width,
                proxy.size.height * 0.86 / imageSize.height,
                2.5,
            )
            ZStack(alignment: .topTrailing) {
                // Backdrop: a click anywhere off the chrome closes (the
                // WebUI's overlay.onclick), and it swallows every hit so
                // the transcript below is inert while zoomed. Black at
                // 72%, same as the WebUI's .lightbox.
                Color.black.opacity(0.72)
                    .contentShape(Rectangle())
                    .onTapGesture { close() }

                Image(nsImage: image)
                    .resizable()
                    .interpolation(.high)
                    .aspectRatio(contentMode: .fit)
                    .frame(width: imageSize.width * scale, height: imageSize.height * scale)
                    .scaleEffect(zoom)
                    .offset(pan)
                    .clipShape(RoundedRectangle(cornerRadius: Theme.radiusSm))
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.radiusSm)
                            .strokeBorder(Theme.bg2, lineWidth: 1)
                            .scaleEffect(zoom)
                            .offset(pan),
                    )
                    .shadow(color: .black.opacity(0.5), radius: 24)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentShape(Rectangle())
                    .gesture(
                        MagnifyGesture()
                            .onChanged { value in
                                zoom = min(max(value.magnification, 0.5), 4)
                            }
                            .onEnded { _ in
                                if zoom <= 1 {
                                    withAnimation(.easeOut(duration: 0.15)) {
                                        zoom = 1
                                        pan = .zero
                                    }
                                }
                            },
                    )
                    .simultaneousGesture(
                        DragGesture()
                            .onChanged { value in
                                guard zoom > 1 else { return }
                                pan = value.translation
                            },
                    )
                    .onTapGesture(count: 2) {
                        withAnimation(.easeOut(duration: 0.15)) {
                            if zoom > 1 {
                                zoom = 1
                                pan = .zero
                            } else {
                                zoom = 2
                            }
                        }
                    }
                    .onTapGesture(count: 1) {
                        if zoom <= 1 {
                            close()
                        }
                    }

                HStack(spacing: 8) {
                    Button(action: copy) {
                        Image(systemName: copied ? "checkmark" : "doc.on.doc")
                            .padding(8)
                            .contentShape(Rectangle())
                    }
                    .help("Copy image (⌘C)")
                    .accessibilityLabel("Copy image")

                    Button(action: close) {
                        Image(systemName: "xmark")
                            .padding(8)
                            .contentShape(Rectangle())
                    }
                    .help("Close (Esc)")
                    .accessibilityLabel("Close image preview")
                }
                .font(.system(size: 13))
                .foregroundStyle(copied ? Theme.success : Theme.muted)
                .buttonStyle(.plain)
                .background(Theme.bg1, in: Capsule())
                .overlay(Capsule().strokeBorder(Theme.bg2, lineWidth: 1))
                .padding(20)
            }
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
        // InlineImageCache: the transcript re-evaluates on every
        // streaming frame; decoding base64 + creating the bitmap here
        // each time was one of the hot paths.
        if let image = InlineImageCache.image(base64: data) {
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
}

// MARK: - Diff (.diff — tinted add/del rows with a sign gutter)

/// Unified diff rendering (WebUI DiffView): diffs longer than
/// DIFF_COLLAPSE_LINES collapse into a disclosure whose head carries
/// the file and the `N lines · +adds −dels` stat; short diffs render
/// flat under the same .d-head strip. The parsed model is memoized by
/// diff text — this view sits inside every tool card and its body is
/// re-evaluated on every transcript frame.
struct DiffView: View {
    let diff: String

    /// WebUI DIFF_COLLAPSE_LINES (diff.ts).
    private static let collapseLines = 30

    @State private var expanded = false

    private var parsed: ParsedDiff {
        DiffParseCache.parse(diff)
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

    @ViewBuilder
    private func bodyLines(_ parsed: ParsedDiff) -> some View {
        if parsed.lines.count > Self.collapseLines {
            ScrollView(.horizontal, showsIndicators: false) {
                ScrollView(.vertical) {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(parsed.lines.indices, id: \.self) { index in
                            DiffLineView(line: parsed.lines[index])
                        }
                    }
                    .padding(.vertical, 6)
                }
                .frame(maxHeight: 400)
            }
        } else {
            ScrollView(.horizontal, showsIndicators: false) {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(parsed.lines.indices, id: \.self) { index in
                        DiffLineView(line: parsed.lines[index])
                    }
                }
                .padding(.vertical, 6)
            }
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
            .lineSpacing(4.5) // ≈ the WebUI's 1.55 line-height (.diff)
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
                        // live: code blocks skip hljs highlighting (an
                        // unterminated fence would re-run the JS
                        // highlighter on the whole growing block every
                        // frame, on the main thread); colors pop in when
                        // the segment seals. The gradient stream cursor
                        // (.stream-cursor) rides the end of the text.
                        MarkdownText(source: text.text, live: text.live, streamCursor: text.live)
                    }
                case let .tool(tool):
                    ToolBlock(live: tool, artifactLoader: artifactLoader)
                }
            }
        }
    }
}

// MARK: - Turn summary (.block-turn-summary)

/// The closing review card of a finished turn (blocks.tsx
/// TurnSummaryBlock): which files the turn's write tools touched,
/// with per-file +/− stats and inline workspace diffs fetched from
/// the run-changes endpoint. The file LIST is expanded by default;
/// per-file diffs start collapsed and reset when the card collapses.
/// Diffs are ledger-before vs CURRENT workspace content — after a
/// revert the numbers drop to 0 ("unchanged").
struct TurnSummaryView: View {
    let summary: TurnSummary
    /// Per-run review stats loader (SessionStore.runStats); nil →
    /// static rows, no chevron (share-view parity).
    var statsLoader: ((_ runId: String, _ forceRefresh: Bool) async -> [RunFileStat]?)?
    /// Revert action (SessionStore.revertRun) → (note, warn).
    var reverter: ((_ runId: String) async -> (note: String, warn: Bool))?

    /// The file list is expanded by default; the whole-card collapse
    /// clears every per-file diff (blocks.tsx toggleCard).
    @State private var open = true
    @State private var openFiles: Set<String> = []
    /// nil before the fetch resolves (or when it answered no ledger
    /// data) — rows stay static in both cases; revert is independent.
    @State private var stats: [RunFileStat]?
    @State private var statsReady = false
    @State private var reverting = false
    @State private var revertNote: String?
    @State private var revertWarn = false

    private var changes: [TurnFileChange] {
        summary.changes ?? []
    }

    private var runId: String? {
        summary.runId.flatMap { $0.isEmpty ? nil : $0 }
    }

    /// Only rows backed by ledger stats offer inline diffs. An empty
    /// response or failed fetch leaves static rows, but does not prevent
    /// trying the independent revert endpoint.
    private var expandable: Bool {
        runId != nil && statsLoader != nil && stats != nil
    }

    private var canRevert: Bool {
        runId != nil && reverter != nil
    }

    /// SessionStore.revertRun prefixes only request failures this way;
    /// warn also covers successful reverts with conflicts or skipped files.
    private var revertFailed: Bool {
        revertNote?.hasPrefix("Revert failed:") == true
    }

    private var statByPath: [String: RunFileStat] {
        Dictionary(uniqueKeysWithValues: (stats ?? []).map { ($0.path, $0) })
    }

    /// Aggregate only comparable entries; when none can be compared,
    /// there is no trustworthy total to display.
    private var totals: (added: Int, removed: Int)? {
        guard let stats else { return nil }
        let comparable = stats.filter { ($0.notComparable ?? "").isEmpty }
        guard !comparable.isEmpty else { return nil }
        return (
            comparable.reduce(0) { $0 + $1.added },
            comparable.reduce(0) { $0 + $1.removed },
        )
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            head
            if open {
                fileList
            }
            foot
        }
        .font(.system(size: Theme.textMd))
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.fg.opacity(0.12), lineWidth: 1),
        )
        .task { await loadStats() }
    }

    // MARK: Head (.tsm-head)

    private var head: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.15)) {
                open.toggle()
                if !open {
                    openFiles.removeAll()
                }
            }
        } label: {
            HStack(spacing: 8) {
                Image(systemName: open ? "chevron.down" : "chevron.right")
                    .font(.system(size: 9, weight: .bold))
                    .foregroundStyle(Theme.muted)
                Text("Turn changes \(changes.count) \(changes.count == 1 ? "file" : "files")")
                    .font(.system(size: Theme.textMd, weight: .semibold))
                    .foregroundStyle(Theme.fg)
                if let totals {
                    (Text("+\(totals.added)").foregroundStyle(Theme.success)
                        + Text("  ")
                        + Text("−\(totals.removed)").foregroundStyle(Theme.error))
                        .font(.system(size: 12, design: .monospaced))
                }
                if summary.cancelled == true {
                    tag("Cancelled", icon: "nosign", tint: Theme.muted, border: Theme.fg.opacity(0.18))
                }
                if summary.failed == true {
                    tag("Failed", icon: "exclamationmark.triangle",
                        tint: Theme.warning, border: Theme.warning.opacity(0.40))
                }
                Spacer(minLength: 0)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    /// .tsm-tag: 11px chip, radius 10px, padding 0/8, hairline border.
    private func tag(_ text: String, icon: String, tint: Color, border: Color) -> some View {
        Label(text, systemImage: icon)
            .font(.system(size: 11))
            .foregroundStyle(tint)
            .padding(.horizontal, 8)
            .padding(.vertical, 1)
            .overlay(Capsule().strokeBorder(border, lineWidth: 1))
    }

    // MARK: File list (.tsm-list)

    private var fileList: some View {
        // Built once per evaluation, not once per row.
        let stats = statByPath
        return VStack(alignment: .leading, spacing: 3) {
            ForEach(changes, id: \.path) { change in
                TurnSummaryFileRow(
                    change: change,
                    stat: stats[change.path],
                    expandable: expandable,
                    statsReady: statsReady,
                    isOpen: openFiles.contains(change.path),
                    onToggle: { toggleFile(change.path) },
                )
            }
        }
        .padding(.top, 8)
        .padding(.bottom, 2)
        .overlay(alignment: .top) {
            // Hairline divider between head and list (.tsm-list).
            Rectangle().fill(Theme.fg.opacity(0.09)).frame(height: 1)
        }
    }

    private func toggleFile(_ path: String) {
        withAnimation(.easeInOut(duration: 0.15)) {
            if openFiles.contains(path) {
                openFiles.remove(path)
            } else {
                openFiles.insert(path)
            }
        }
    }

    // MARK: Foot (.tsm-foot / .tsm-note)

    @ViewBuilder
    private var foot: some View {
        if let revertNote {
            // A successful result replaces the footer; failed requests
            // retain the action so the user can retry.
            Label(revertNote, systemImage: revertWarn ? "exclamationmark.triangle" : "checkmark")
                .font(.system(size: 12))
                .foregroundStyle(revertWarn ? Theme.warning : Theme.success)
        }
        if revertNote == nil || revertFailed {
            HStack(alignment: .top, spacing: 12) {
                if canRevert {
                    revertButton
                }
                if revertNote == nil {
                    // .tsm-footnote
                    Text(footnote)
                        .font(.system(size: 11))
                        .foregroundStyle(Theme.muted)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding(.top, 7)
            .overlay(alignment: .top) {
                Rectangle().fill(Theme.fg.opacity(0.09)).frame(height: 1)
            }
        }
    }

    private var footnote: String {
        var text =
            "Only loom write-tool edits are counted; files written inside run_cmd (e.g. sed) are not."
        text += expandable
            ? " Click a file row to expand its workspace diff."
            : " Inline diffs are unavailable for this turn."
        return text
    }

    /// .tsm-btn.danger: bg2 chip; the border and hover tint are
    /// owned by TurnSummaryButtonStyle (one stroke, not two).
    private var revertButton: some View {
        Button(action: confirmRevert) {
            Label(reverting ? "Reverting…" : "Revert this turn", systemImage: "arrow.counterclockwise")
                .font(.system(size: 12))
                .padding(.horizontal, 10)
                .padding(.vertical, 3)
                .background(Theme.bg2, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
        }
        .buttonStyle(TurnSummaryButtonStyle())
        .disabled(reverting)
        .opacity(reverting ? 0.55 : 1)
        .help(
            "Restore files written this turn to their pre-turn contents "
                + "(external changes made after the turn are overwritten and reported)",
        )
    }

    /// The WebUI confirms each externally-modified file one by one;
    /// the native app collapses that into a single alert naming the
    /// overwrite semantics before the server does the same checks.
    private func confirmRevert() {
        guard !reverting, let runId, let reverter else { return }
        let alert = NSAlert()
        alert.messageText = "Revert this turn"
        alert.informativeText =
            "Restore files written this turn to their pre-turn contents. "
                + "External changes made after the turn will be overwritten and reported."
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Revert")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        reverting = true
        Task {
            let result = await reverter(runId)
            reverting = false
            revertNote = result.note
            revertWarn = result.warn
            if !revertFailed {
                await loadStats(forceRefresh: true)
            }
        }
    }

    private func loadStats(forceRefresh: Bool = false) async {
        guard let runId, let statsLoader else {
            statsReady = true
            return
        }
        stats = await statsLoader(runId, forceRefresh)
        statsReady = true
    }
}

/// .tsm-btn hover feedback (the plain style draws a pressed dim, which
/// the WebUI button never does): border/glyph tint toward error on
/// hover for the revert action, primary otherwise — .plain gives no
/// hover hook, so a custom style tracks it.
private struct TurnSummaryButtonStyle: ButtonStyle {
    @State private var hovering = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .foregroundStyle(hovering ? Theme.error : Theme.fg)
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusMd)
                    .strokeBorder(hovering ? Theme.error : Theme.fg.opacity(0.16), lineWidth: 1),
            )
            .onHover { hovering = $0 }
            .animation(.easeInOut(duration: 0.12), value: hovering)
    }
}

// MARK: - Turn summary file row (.tsm-file / .tsm-row)

/// One file row plus its expandable inline-diff region. The row is a
/// whole-row click target only when diffs are available (expandable).
private struct TurnSummaryFileRow: View {
    let change: TurnFileChange
    let stat: RunFileStat?
    let expandable: Bool
    let statsReady: Bool
    let isOpen: Bool
    let onToggle: () -> Void

    @State private var hovering = false

    private var deleted: Bool {
        stat.map { $0.afterSize == -1 && ($0.notComparable ?? "").isEmpty } ?? false
    }

    private var edits: Int {
        stat?.edits ?? change.edits
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: onToggle) {
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    badge
                    names
                    Spacer(minLength: 8)
                    right
                }
                .padding(.horizontal, 4)
                .padding(.vertical, 6)
                .background(
                    RoundedRectangle(cornerRadius: 6)
                        .fill(hovering && expandable ? Theme.fg.opacity(0.06) : .clear),
                )
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!expandable)
            .onHover { hovering = $0 }
            if isOpen {
                diffRegion
                    // .tsm-file-diff: aligns the box with the filename
                    // column (row padding 4 + badge 20 + names gap 8).
                    .padding(.leading, 32)
                    .padding(.top, 3)
                    .padding(.bottom, 5)
            }
        }
    }

    /// .tsm-badge: 20×20 radius 5, bold 11 — A: success on success 18%;
    /// M: warning on warning 20%.
    private var badge: some View {
        Text(change.created == true ? "A" : "M")
            .font(.system(size: 11, weight: .bold))
            .foregroundStyle(change.created == true ? Theme.success : Theme.warning)
            .frame(width: 20, height: 20)
            .background(
                change.created == true ? Theme.success.opacity(0.18) : Theme.warning.opacity(0.20),
                in: RoundedRectangle(cornerRadius: 5),
            )
    }

    /// .tsm-names: base name (fg, never truncates) + directory tail
    /// (muted, ellipsis) sharing one baseline.
    private var names: some View {
        let (base, dir) = Self.splitPath(change.path)
        return HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(base)
                .font(Theme.monoSm)
                .foregroundStyle(Theme.fg)
                .fixedSize()
            if !dir.isEmpty {
                Text(dir)
                    .font(Theme.monoSm)
                    .foregroundStyle(Theme.muted)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
        }
    }

    /// .tsm-right: mono 11 muted — Deleted (error) / "N edits" /
    /// +/− stats / the per-file chevron.
    private var right: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            if deleted {
                Text("Deleted")
                    .foregroundStyle(Theme.error)
            }
            if edits > 1 {
                Text("\(edits) edits")
            }
            if let stat, (stat.notComparable ?? "").isEmpty, stat.added > 0 || stat.removed > 0 {
                Text("+\(stat.added)")
                    .foregroundStyle(Theme.success)
                Text("−\(stat.removed)")
                    .foregroundStyle(Theme.error)
            }
            if expandable {
                Image(systemName: isOpen ? "chevron.down" : "chevron.right")
                    .font(.system(size: 9, weight: .bold))
            }
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(Theme.muted)
    }

    /// The inline diff region under the row (.tsm-file-diff content):
    /// loading / not-comparable / truncated + diff / no-op / no-ledger
    /// states, in blocks.tsx order.
    @ViewBuilder
    private var diffRegion: some View {
        if !statsReady {
            note("Loading…")
        } else if let notComparable = stat?.notComparable, !notComparable.isEmpty {
            note(notComparable)
        } else if let diff = stat?.diff, !diff.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                if stat?.diffTruncated == true {
                    note("Diff truncated; stats cover only the head of the file")
                }
                TurnSummaryInlineDiff(path: change.path, diffText: diff)
            }
        } else if stat != nil {
            note(
                "Current content matches the pre-turn state "
                    + "(the changes may have been reverted by later operations)",
            )
        } else {
            note("No ledger record for this file in this turn; diff unavailable")
        }
    }

    /// .tsm-diff-note: 11px muted on a bg0 rounded box.
    private func note(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 11))
            .foregroundStyle(Theme.muted)
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Theme.bg0, in: RoundedRectangle(cornerRadius: 8))
    }

    /// TurnSummaryBlock splitPath: the final component renders bold-fg,
    /// the directory tail trails it in muted.
    static func splitPath(_ path: String) -> (base: String, dir: String) {
        let i = path.lastIndex(of: "/")
        guard let i else { return (path, "") }
        return (String(path[path.index(after: i)...]), String(path[..<i]))
    }
}

// MARK: - Turn summary inline diff (.tsm-diff)

/// blocks.tsx InlineDiff: the review diff rendered WITHOUT DiffView's
/// frame — a shared bg0 box, lines COLORED BY KIND (whole-line
/// red/green, unlike the tool card's tinted-background-only style).
/// Overlong diffs cap at 80 lines with a fold note.
private struct TurnSummaryInlineDiff: View {
    let path: String
    let diffText: String

    /// INLINE_DIFF_MAX_LINES (blocks.tsx).
    private static let maxLines = 80

    /// parseDiff wants the +++ header for the file label; the review
    /// endpoint emits bare hunks, so the caller re-attaches one.
    private var parsed: ParsedDiff {
        DiffParseCache.parse("+++ b/\(path)\n" + diffText)
    }

    var body: some View {
        let lines = parsed.lines
        let shown = lines.count > Self.maxLines ? Array(lines.prefix(Self.maxLines)) : lines
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(shown.enumerated()), id: \.offset) { _, line in
                row(line)
            }
            if lines.count > shown.count {
                Text("⋯ \(lines.count - shown.count) more lines folded")
                    .foregroundStyle(Theme.muted)
                    .font(.system(size: 11))
                    .padding(.horizontal, 12)
                    .padding(.vertical, 4)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .font(Theme.monoSm)
        .lineSpacing(4.5) // .tsm-diff line-height 1.55 at 13px
        .background(Theme.bg0)
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    @ViewBuilder
    private func row(_ line: ParsedDiff.Line) -> some View {
        // Unified-hunk header: a full-width strip, no sign column.
        if line.kind == .hunk {
            Text(line.text)
                .foregroundStyle(Theme.purple)
                .padding(.horizontal, 12)
                .padding(.vertical, 1)
                .frame(maxWidth: .infinity, alignment: .leading)
        } else if line.kind == .ctx, line.text == "..." {
            // Compact-format region separator: a subtle ellipsis row.
            Text("⋯")
                .foregroundStyle(Theme.muted)
                .frame(maxWidth: .infinity)
        } else {
            HStack(alignment: .firstTextBaseline, spacing: 0) {
                Text(line.sign)
                    .frame(width: 28)
                    .foregroundStyle(signColor(for: line.kind))
                Text(line.text)
            }
            .padding(.trailing, 12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .foregroundStyle(textColor(for: line.kind))
            .background(backgroundColor(for: line.kind))
        }
    }

    /// Whole-line coloring (.tsm-dline.d-add/.d-del): text AND a 12%
    /// tint; context lines stay fg with a muted sign.
    private func textColor(for kind: ParsedDiff.Kind) -> Color {
        switch kind {
        case .add: Theme.success
        case .del: Theme.error
        default: Theme.fg
        }
    }

    private func signColor(for kind: ParsedDiff.Kind) -> Color {
        switch kind {
        case .add: Theme.success
        case .del: Theme.error
        default: Theme.muted
        }
    }

    private func backgroundColor(for kind: ParsedDiff.Kind) -> Color {
        switch kind {
        case .add: Theme.success.opacity(0.12)
        case .del: Theme.error.opacity(0.12)
        default: .clear
        }
    }
}
