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

import SwiftUI

/// Approval request card — pixel-faithful port of the WebUI's
/// `.card-approval` (blocks/cards.tsx): 2px warning border on bg0,
/// title row with risk pill + mono tool name, desc / consequence /
/// target cmd block, then Allow / Always allow / Trust / Deny with
/// rule-preview memos.
///
/// Resolutions are one-shot: the first client to decide wins, everyone
/// else gets 409 binding_mismatch (SERVE_DESIGN §4.6).
struct ApprovalCard: View {
    let approval: ApprovalRequestedPayload
    /// (decision, always, trust) — mirrors the WebUI's onResolve.
    let onResolve: (ApprovalDecision, _ always: Bool, _ trust: String?) -> Void

    /// An empty rule_preview means this call cannot be remembered
    /// (backend ApprovalRulePreview); hide "Always allow" in that case
    /// to avoid offering a silently ineffective option.
    private var rulePreview: String {
        approval.rulePreview ?? ""
    }

    private var trustPreview: String {
        approval.trustPreview ?? ""
    }

    /// Mirrors domain.RiskLevel (render.RiskDescription): R0 none,
    /// R1 read-only, R2 write, R3 destructive, R4 critical.
    private var riskLabel: String {
        switch approval.risk {
        case 0: "none"
        case 1: "read-only"
        case 2: "write"
        case 3: "destructive"
        case 4: "critical"
        default: "unknown"
        }
    }

    /// Destructive-and-above operations escalate the pill to error red.
    private var riskColor: Color {
        approval.risk >= 3 ? Theme.error : Theme.warning
    }

    /// run_cmd approval descriptions always end with a display-only
    /// "; args_hash=<prefix>" suffix (buildApprovalDesc): strip it.
    /// The full signed hash lives in the permission audit events;
    /// showing a prefix here is noise.
    private static func stripArgsHash(_ text: String) -> String {
        guard let range = text.range(of: #"; args_hash=\S+$"#, options: .regularExpression) else {
            return text
        }
        return String(text[..<range.lowerBound])
    }

    private var displayDescription: String {
        Self.stripArgsHash(approval.description)
    }

    private var displayTarget: String? {
        guard let target = approval.target, !target.isEmpty else { return nil }
        return Self.stripArgsHash(target)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            titleRow

            // When the backend embeds the full command in BOTH the
            // description and the target, suppress the prose copy and
            // show it only in the mono block below: the command is the
            // thing being approved, and it must stay monospaced and
            // selectable, never reflowed as body text.
            let target = displayTarget

            if !displayDescription.isEmpty, displayDescription != target {
                Text(displayDescription)
                    // With the mono cmd block present, the description
                    // demotes to supplementary exec context (env / cwd /
                    // timeout / network / note).
                    .font(.system(size: target != nil ? Theme.textSm : Theme.textMd))
                    .foregroundStyle(target != nil ? Theme.muted : Theme.fg)
                    .textSelection(.enabled)
                    .padding(.top, 6)
            }

            // Consequence row: what this operation "will do" (the
            // derived effect), not the command text itself. Muted and
            // one size down — it's the analysis, secondary to the
            // command being approved.
            if let consequence = approval.consequence, !consequence.isEmpty {
                Text(consequence)
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.muted)
                    .textSelection(.enabled)
                    .padding(.top, 6)
            }

            if let target {
                Text(target)
                    .font(Theme.monoSm)
                    .foregroundStyle(Theme.fg)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 9)
                    .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
                    .padding(.vertical, 10)
            }

            pathsView

            actionsRow
                .padding(.top, 12)
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 16)
        // Cap the line length: edge-to-edge cards made the consequence
        // text unreadable on wide windows.
        .frame(maxWidth: 720, alignment: .leading)
        .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.warning, lineWidth: 2),
        )
    }

    // MARK: Title (.card-title)

    private var titleRow: some View {
        HStack(spacing: 8) {
            Label("Approval required", systemImage: "exclamationmark.shield")
                .font(.system(size: 13.5, weight: .bold))

            // .risk pill: text-xs 600, 1px border, full radius. The
            // label spells out the level ("R2 · write") so the pill is
            // self-explanatory; >= R3 escalates to error red.
            Text("R\(approval.risk) · \(riskLabel)")
                .font(.system(size: Theme.textXs, weight: .semibold))
                .foregroundStyle(riskColor)
                .padding(.horizontal, 8)
                .overlay(Capsule().strokeBorder(riskColor, lineWidth: 1))
                .help("Risk level R\(approval.risk): \(riskLabel)")

            Text(approval.toolName)
                .font(Theme.monoSm)

            Spacer(minLength: 0)
        }
        .foregroundStyle(Theme.warning)
    }

    // MARK: Paths (read/write)

    @ViewBuilder private var pathsView: some View {
        let reads = approval.readPaths ?? []
        let writes = approval.writePaths ?? []
        if !reads.isEmpty || !writes.isEmpty {
            VStack(alignment: .leading, spacing: 2) {
                ForEach(reads, id: \.self) { path in
                    Label(path, systemImage: "doc.text")
                        .font(Theme.monoXs)
                        .foregroundStyle(Theme.muted)
                }
                ForEach(writes, id: \.self) { path in
                    Label(path, systemImage: "pencil.line")
                        .font(Theme.monoXs)
                        .foregroundStyle(Theme.highlight)
                }
            }
            .padding(.top, 6)
        }
    }

    // MARK: Actions (.actions)

    private var actionsRow: some View {
        // The rule/trust memo sits on its own line: squeezed in beside
        // the buttons it compressed them on narrow windows.
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 10) {
                // ⌥Return, NOT ⌘Return: the composer's send button owns
                // ⌘Return window-wide, and a ⌘Return here would shadow
                // (or be shadowed by) it whenever the card and the
                // composer are both visible.
                Button("Allow") { onResolve(.allow, false, nil) }
                    .buttonStyle(PrimaryButtonStyle())
                    .keyboardShortcut(.return, modifiers: .option)
                    .help("Allow (⌥Return)")

                if !rulePreview.isEmpty {
                    Button("Always allow") { onResolve(.allow, true, nil) }
                        .buttonStyle(OutlineButtonStyle())
                }

                if !trustPreview.isEmpty {
                    Button("Trust (no sandbox)") { onResolve(.allow, true, "unsandboxed") }
                        .buttonStyle(OutlineButtonStyle(color: Theme.error))
                }

                Button("Deny") { onResolve(.deny, false, nil) }
                    .buttonStyle(OutlineButtonStyle(color: Theme.error))
                    .keyboardShortcut(.delete, modifiers: .command)
                    .help("Deny (⌘⌫)")

                Spacer(minLength: 0)
            }

            if !rulePreview.isEmpty {
                Text("\"Always allow\" remembers \"\(rulePreview)\" as a rule for this workspace")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .lineLimit(2)
            } else if !trustPreview.isEmpty {
                Text("\"Trust\" remembers \"\(trustPreview)\" with full user permissions")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .lineLimit(2)
            }
        }
    }
}
