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

/// Renders one precomputed TranscriptModel.Row. All derivation (tool
/// call/result pairing, diff computation, the action-row turn gate)
/// happened in TranscriptModel.build — body evaluation here is pure
/// layout, so streaming frames stay cheap.
struct MessageRow: View {
    let row: TranscriptModel.Row
    /// Authenticated artifact loader (SessionStore.artifactData);
    /// artifact parts fall back to a plain label when absent.
    var artifactLoader: ((ContentPart.Artifact) async -> (data: Data, mediaType: String?)?)?

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
/// it. The image fills ~86% of the screen (scaled down when larger,
/// scaled up for small bitmaps, capped at 2.5x to avoid mush); pinch /
/// ctrl-scroll zooms to 4x, and dragging pans while zoomed.
struct ImageLightboxView: View {
    let image: NSImage
    let close: () -> Void

    @State private var copied = false
    @State private var zoom: CGFloat = 1
    @State private var pan: CGSize = .zero
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
                }
                .help("Copy image (⌘C)")
                .accessibilityLabel("Copy image")

                Button(action: close) {
                    Image(systemName: "xmark")
                }
                .help("Close (Esc)")
                .accessibilityLabel("Close image preview")
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
                        // live: code blocks skip hljs highlighting (an
                        // unterminated fence would re-run the JS
                        // highlighter on the whole growing block every
                        // frame, on the main thread); colors pop in when
                        // the segment seals.
                        MarkdownText(source: text.text + (text.live ? " ▍" : ""), live: text.live)
                    }
                case let .tool(tool):
                    ToolBlock(live: tool, artifactLoader: artifactLoader)
                }
            }
        }
    }
}
