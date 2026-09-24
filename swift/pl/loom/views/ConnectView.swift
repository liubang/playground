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

/// Token gate — pixel-faithful port of the WebUI's Gate page
/// (Gate.tsx / gate.css): a centered 400px card on bg0 with the
/// "◆ loom" brand, halo-focused inputs and a full-width Connect
/// button. Normally the app starts its bundled `loom serve` and blows
/// straight past this screen; it appears only when that fails or when
/// attaching to a remote instance.
struct ConnectView: View {
    @Bindable var appState: AppState

    @FocusState private var focusedField: Field?

    private enum Field { case server, token }

    var body: some View {
        // .gate-wrap: grid place-items center on --bg0
        VStack(spacing: 0) {
            // .brand
            Text("◆ loom")
                .font(.system(size: 20, weight: .bold))
                .foregroundStyle(Theme.primary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.bottom, 4)

            Text("Enter the serve token to connect.")
                .font(.system(size: Theme.textMd))
                .foregroundStyle(Theme.muted)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.top, 14)
                .padding(.bottom, 10)

            VStack(spacing: 14) {
                gateField(
                    text: $appState.serverAddress,
                    prompt: "http://127.0.0.1:7680",
                    field: .server,
                    secure: false,
                )

                HStack(spacing: 8) {
                    gateField(
                        text: $appState.token,
                        prompt: "token",
                        field: .token,
                        secure: true,
                    )
                    GhostButton {
                        appState.refetchTokenFromDisk()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .help("Re-read serve.token from the loom data directory")
                    .accessibilityLabel("Re-read token from disk")
                }
            }

            if case let .failed(message) = appState.status {
                // .gate-error
                Text(message)
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.error)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.top, 10)
            }

            Group {
                if case .connecting = appState.status {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text(appState.statusDetail ?? "Connecting…")
                            .font(.system(size: Theme.textSm))
                            .foregroundStyle(Theme.muted)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
                } else {
                    Button {
                        appState.connect()
                    } label: {
                        Text("Connect")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 4)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(PrimaryButtonStyle())
                    .keyboardShortcut(.defaultAction)
                    .disabled(appState.token.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            .padding(.top, 14)

            // .hint
            Text("The token is printed when `loom serve` first starts; you can also find `serve.token` in the loom data directory. The form above is only needed to attach to a remote or manually managed instance.")
                .font(.system(size: 12))
                .foregroundStyle(Theme.muted)
                .lineSpacing(3.6) // line-height 1.55 at 12px
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.top, 14)
        }
        .padding(.horizontal, 36)
        .padding(.vertical, 40)
        .frame(width: 400)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusLg))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusLg)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg0)
        .windowDragSurface()
    }

    /// .gate input: bg0 field with a muted border that swaps to the
    /// primary + ring halo on focus (gate.css input:focus).
    @ViewBuilder
    private func gateField(
        text: Binding<String>, prompt: String, field: Field, secure: Bool,
    ) -> some View {
        let focused = focusedField == field
        Group {
            if secure {
                SecureField("", text: text, prompt: Text(prompt))
            } else {
                TextField("", text: text, prompt: Text(prompt))
            }
        }
        .textFieldStyle(.plain)
        .font(.system(size: Theme.textMd))
        .foregroundStyle(Theme.fg)
        .focused($focusedField, equals: field)
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
        .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusSm)
                .strokeBorder(focused ? Theme.primary : Theme.muted, lineWidth: 1),
        )
        .shadow(color: focused ? Theme.ring : .clear, radius: 2)
        .animation(.easeOut(duration: 0.12), value: focused)
    }
}
