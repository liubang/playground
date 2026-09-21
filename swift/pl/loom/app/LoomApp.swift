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

/// Loom (Swift) — a pure-SwiftUI peer client of `loom serve`'s REST+SSE
/// protocol (SERVE_DESIGN §3.1: every frontend is an equal pure-rendering
/// client). No Go code is linked; the Go backend is untouched.
@main
struct LoomApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var appState = AppState()
    /// WebUI loom_theme: the header's toggle flips it; every token
    /// color re-resolves from the new effective appearance.
    @AppStorage("loom.theme") private var theme = "dark"

    var body: some Scene {
        WindowGroup {
            RootView(appState: appState)
                .frame(minWidth: 900, minHeight: 580)
                // The WebUI's default (and reference) theme is dark
                // Everforest; light is its Everforest Light Medium.
                .preferredColorScheme(theme == "light" ? .light : .dark)
        }
        // The WebUI shell has no native chrome: content — including the
        // sidebar's bg1 — runs edge to edge under the traffic lights.
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentMinSize)
        .defaultSize(width: 1200, height: 780)
        .commands {
            CommandGroup(after: .newItem) {
                Button("New Session") {
                    NotificationCenter.default.post(name: .loomNewSession, object: nil)
                }
                .keyboardShortcut("n", modifiers: .command)
                .disabled(appState.sessionList == nil)
            }
        }
    }
}

extension Notification.Name {
    static let loomNewSession = Notification.Name("loom.newSession")
}

/// hiddenTitleBar windows have no grab-able chrome, so make the whole
/// background a drag region (scroll views still win drags first).
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_: Notification) {
        NSApp.windows.forEach { $0.isMovableByWindowBackground = true }
    }

    func applicationDidBecomeActive(_: Notification) {
        NSApp.windows.forEach { $0.isMovableByWindowBackground = true }
    }
}
