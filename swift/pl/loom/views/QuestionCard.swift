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

/// ask_user question card — pixel-faithful port of the WebUI's
/// `.card-question` (blocks/cards.tsx): 2px primary border, "? Loom
/// asks" title, radio/checkbox options with inline " — desc", a
/// custom-answer input with the focus halo, Submit / Skip actions.
struct QuestionCard: View {
    let question: PendingRequest.Question
    let onAnswer: (_ selected: [String], _ customText: String?, _ skipped: Bool) -> Void

    @State private var selected: Set<String> = []
    @State private var customText = ""
    @FocusState private var customFocused: Bool
    @ScaledMetric(relativeTo: .body) private var inputFontSize: CGFloat = Theme.textMd

    private var allowMultiple: Bool {
        question.allowMultiple ?? false
    }

    private var canSubmit: Bool {
        !selected.isEmpty || !customText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // .card-title
            HStack(spacing: 6) {
                Text("? Loom asks")
                    .font(.system(size: 13.5, weight: .bold))
                    .foregroundStyle(Theme.primary)
                if allowMultiple {
                    Text("(multi-select)")
                        .font(.system(size: 12))
                        .foregroundStyle(Theme.muted)
                }
                Spacer(minLength: 0)
            }

            // .q-text
            Text(question.text)
                .font(.system(size: Theme.textMd))
                .foregroundStyle(Theme.fg)
                .padding(.top, 8)
                .padding(.bottom, 10)

            // .opt rows
            VStack(alignment: .leading, spacing: 12) {
                ForEach(question.options, id: \.label) { option in
                    Button {
                        toggle(option.label)
                    } label: {
                        HStack(alignment: .firstTextBaseline, spacing: 8) {
                            Image(systemName: isSelected(option.label)
                                ? (allowMultiple ? "checkmark.square.fill" : "checkmark.circle.fill")
                                : (allowMultiple ? "square" : "circle"))
                                .foregroundStyle(isSelected(option.label) ? Theme.primary : Theme.muted)
                            optionLabel(option)
                        }
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(option.label)
                    .accessibilityValue(isSelected(option.label) ? "Selected" : "Not selected")
                    .accessibilityHint(option.description ?? (allowMultiple ? "Toggle option" : "Select option"))
                }
            }

            // input[type='text'] with the gate-style focus halo
            TextField("Custom answer… (optional)", text: $customText)
                .textFieldStyle(.plain)
                .font(.system(size: inputFontSize))
                .foregroundStyle(Theme.fg)
                .focused($customFocused)
                .accessibilityLabel("Custom answer")
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.radiusSm)
                        .strokeBorder(customFocused ? Theme.primary : Theme.muted, lineWidth: 1),
                )
                .shadow(color: customFocused ? Theme.ring : .clear, radius: 2)
                .padding(.top, 8)
                .padding(.bottom, 4)
                .onSubmit(submit)

            // .actions
            HStack(spacing: 10) {
                Button("Submit", action: submit)
                    .buttonStyle(PrimaryButtonStyle())
                    .disabled(!canSubmit)
                // Multiple pending cards can be enabled at once; Return belongs
                // only to the focused custom-answer field above.

                Button("Skip") {
                    onAnswer([], nil, true)
                }
                .buttonStyle(OutlineButtonStyle())

                Spacer(minLength: 0)
            }
            .padding(.top, 12)
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.primary, lineWidth: 2),
        )
    }

    /// label + optional inline " — description" (desc: muted, text-sm).
    private func optionLabel(_ option: QuestionAskedPayload.Option) -> some View {
        var text = Text(option.label)
            .font(.system(size: 13.5))
            .foregroundStyle(Theme.fg)
        if let description = option.description, !description.isEmpty {
            text = text + Text(" — \(description)")
                .font(.system(size: Theme.textSm))
                .foregroundStyle(Theme.muted)
        }
        return text
    }

    private func isSelected(_ label: String) -> Bool {
        selected.contains(label)
    }

    private func toggle(_ label: String) {
        if allowMultiple {
            if selected.contains(label) {
                selected.remove(label)
            } else {
                selected.insert(label)
            }
        } else {
            // Clearing a radio selection is local only; Skip is the explicit skipped answer.
            selected = selected.contains(label) ? [] : [label]
        }
    }

    private func submit() {
        guard canSubmit else { return }
        let custom = customText.trimmingCharacters(in: .whitespacesAndNewlines)
        let orderedSelection = question.options.map(\.label).filter { selected.contains($0) }
        onAnswer(orderedSelection, custom.isEmpty ? nil : custom, false)
    }
}
