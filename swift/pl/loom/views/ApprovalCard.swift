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

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            titleRow

            if !approval.description.isEmpty {
                Text(approval.description)
                    .font(.system(size: Theme.textMd))
                    .foregroundStyle(Theme.fg)
                    .padding(.top, 6)
            }

            // Consequence row: what this operation "will do" (the
            // derived effect), not the command text itself.
            if let consequence = approval.consequence, !consequence.isEmpty {
                Text(consequence)
                    .font(.system(size: Theme.textMd))
                    .foregroundStyle(Theme.fg)
                    .padding(.top, 6)
            }

            // Show the cmd block only when the target differs from the
            // description, avoiding rendering the same text twice.
            if let target = approval.target,
               !target.isEmpty, target != approval.description
            {
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
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.warning, lineWidth: 2),
        )
    }

    // MARK: Title (.card-title)

    private var titleRow: some View {
        HStack(spacing: 8) {
            Label("Approval required", systemImage: "questionmark.circle")
                .font(.system(size: 13.5, weight: .bold))

            // .risk pill: text-xs 600, 1px warning border, full radius.
            Text("R\(approval.risk)")
                .font(.system(size: Theme.textXs, weight: .semibold))
                .padding(.horizontal, 8)
                .overlay(Capsule().strokeBorder(Theme.warning, lineWidth: 1))

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
