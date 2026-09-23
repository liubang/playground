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
    /// Mirrors RootView's key: the menu's Toggle Sidebar flips it
    /// directly (AppStorage shares the UserDefaults value, so the
    /// sidebar reacts without a notification hop).
    @AppStorage("loom.sidebarCollapsed") private var sidebarCollapsed = false

    var body: some Scene {
        // Single-window scene: WindowGroup's "New Window" spawned
        // mirror windows sharing this one AppState — confusing for a
        // single-connection client.
        Window("Loom", id: "main") {
            RootView(appState: appState)
                .onAppear { delegate.appState = appState }
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
            // The stock Settings… entry (⌘,) — opens the same config
            // sheet as the sidebar gear.
            CommandGroup(replacing: .appSettings) {
                Button("Settings…") {
                    NotificationCenter.default.post(name: .loomOpenSettings, object: nil)
                }
                .keyboardShortcut(",", modifiers: .command)
                .disabled(appState.sessionList == nil)
            }
            // View menu = navigation: sidebar toggle (the macOS ⌃⌘S
            // idiom) plus keyboard session switching (⌘[ / ⌘]) — the
            // sidebar list itself has no focus/arrow-key navigation.
            CommandGroup(after: .sidebar) {
                Button(sidebarCollapsed ? "Show Sidebar" : "Hide Sidebar") {
                    sidebarCollapsed.toggle()
                }
                .keyboardShortcut("s", modifiers: [.control, .command])
                .disabled(appState.sessionList == nil)
                Divider()
                Button("Previous Session") {
                    NotificationCenter.default.post(name: .loomPrevSession, object: nil)
                }
                .keyboardShortcut("[", modifiers: .command)
                .disabled(appState.sessionList == nil)
                Button("Next Session") {
                    NotificationCenter.default.post(name: .loomNextSession, object: nil)
                }
                .keyboardShortcut("]", modifiers: .command)
                .disabled(appState.sessionList == nil)
            }
            // Keep the system Help menu (including its searchable menu
            // commands) even though Loom does not ship a help book.
        }
    }
}

extension Notification.Name {
    static let loomNewSession = Notification.Name("loom.newSession")
    static let loomPrevSession = Notification.Name("loom.prevSession")
    static let loomNextSession = Notification.Name("loom.nextSession")
    static let loomOpenSettings = Notification.Name("loom.openSettings")
}

/// Window dragging on a hiddenTitleBar window: macOS 15+ uses explicit
/// WindowDragGesture surfaces (chat header, statusbar, landing states)
/// instead of the movable-by-background flag — that flag claims every
/// mouseDown as a potential window drag and raced SwiftUI's own hit
/// tracking, which is why header buttons (sidebar/theme toggles)
/// intermittently ignored clicks. macOS 14 has no WindowDragGesture;
/// keep the old flag there as a fallback (and the divider's
/// NonDraggableStrip still covers its worst symptom).
final class AppDelegate: NSObject, NSApplicationDelegate {
    var appState: AppState?

    func applicationWillTerminate(_: Notification) {
        appState?.shutdown()
    }

    func applicationDidFinishLaunching(_: Notification) {
        enableBackgroundDragFallback()
    }

    func applicationDidBecomeActive(_: Notification) {
        enableBackgroundDragFallback()
    }

    private func enableBackgroundDragFallback() {
        if #unavailable(macOS 15) {
            NSApp.windows.forEach { $0.isMovableByWindowBackground = true }
        }
    }
}
