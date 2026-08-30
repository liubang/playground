import AppKit
import ServiceManagement
import SwiftUI

// MARK: - App Nap

/// Keeps App Nap away for the app's lifetime: as an LSUIElement app with
/// no visible windows, timers would otherwise be coalesced by minutes
/// whenever all popovers are closed. The "allowing idle system sleep"
/// variant is deliberate: plain `.userInitiated` also asserts against
/// idle system sleep, which would keep the machine awake for as long as
/// the app runs and make the battery module's 防休眠 toggle a no-op.
final class AppNapDisabler {
    static let shared = AppNapDisabler()

    private var activity: NSObjectProtocol?

    private init() {
        activity = ProcessInfo.processInfo.beginActivity(
            options: [.userInitiatedAllowingIdleSystemSleep],
            reason: "Menu bar updates",
        )
    }
}

// MARK: - Quit

/// terminate() is advisory in accessory apps; make sure we actually exit
/// even if something vetoes it.
func quitApp() {
    NSApplication.shared.terminate(nil)
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
        exit(0)
    }
}

// MARK: - Launch at login

enum LaunchAtLogin {
    static var isEnabled: Bool {
        SMAppService.mainApp.status == .enabled
    }

    static func setEnabled(_ enabled: Bool) throws {
        if enabled {
            try SMAppService.mainApp.register()
        } else {
            try SMAppService.mainApp.unregister()
        }
    }

    /// Binding for a settings Toggle; errors surface through `onError`.
    static func binding(onError: @escaping (String?) -> Void) -> Binding<Bool> {
        Binding(
            get: { isEnabled },
            set: { enabled in
                do {
                    try setEnabled(enabled)
                    onError(nil)
                } catch {
                    onError("开机自启设置失败: \(error.localizedDescription)")
                }
            },
        )
    }
}

// MARK: - Module visibility

/// Status-item visibility keys shared by the StatusItemControllers and
/// the settings window.
enum ModuleVisibility {
    static let calendarKey = "AuraBar.module.calendar"
    static let weatherKey = "AuraBar.module.weather"
    static let cpuKey = "AuraBar.module.cpu"
    static let memoryKey = "AuraBar.module.memory"
static let networkKey = "AuraBar.module.network"
static let gpuKey = "AuraBar.module.gpu"
static let batteryKey = "AuraBar.module.battery"
}

// MARK: - Themed settings field

/// Theme-aware text field: plain style with explicitly drawn Everforest
/// background/border, text, caret and placeholder colors.
struct SettingsField: View {
    let prompt: String
    @Binding var text: String
    var onSubmit: () -> Void = {}

    @Environment(\.theme) private var theme

    var body: some View {
        TextField(text: $text, prompt: Text(prompt).foregroundStyle(theme.textSecondary.opacity(0.65))) {
            EmptyView()
        }
        .labelsHidden()
        .textFieldStyle(.plain)
        .font(.caption)
        .foregroundStyle(theme.textPrimary)
        .tint(theme.textPrimary)
        .padding(.horizontal, 7)
        .padding(.vertical, 5)
        .background(theme.cardBackground, in: RoundedRectangle(cornerRadius: 6))
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .stroke(theme.cardBorder, lineWidth: 1),
        )
        .onSubmit(onSubmit)
    }
}

// MARK: - Refresh button

/// Refresh control with a three-state feedback loop: spinning while
/// loading, a green checkmark flash on success, plain arrow otherwise.
struct RefreshButton: View {
    let isLoading: Bool
    let justRefreshed: Bool
    let action: () -> Void

    @Environment(\.theme) private var theme
    @State private var hover = false

    var body: some View {
        Button(action: action) {
            ZStack {
                if isLoading {
                    ProgressView()
                        .controlSize(.small)
                        .transition(.opacity)
                } else if justRefreshed {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(theme.ok)
                        .transition(.scale.combined(with: .opacity))
                } else {
                    Image(systemName: "arrow.clockwise")
                        .foregroundStyle(theme.textSecondary)
                        .transition(.opacity)
                }
            }
            .font(.callout)
            .frame(width: 18, height: 18)
            .scaleEffect(hover && !isLoading ? 1.15 : 1)
        }
        .buttonStyle(.plain)
        .disabled(isLoading)
        .onHover { hover = $0 }
        .animation(.easeOut(duration: 0.15), value: hover)
        .animation(.easeInOut(duration: 0.2), value: isLoading)
        .help("刷新")
    }
}

// MARK: - Card style

/// The standard popover card: Everforest card background, 10pt corners,
/// 1pt border.
private struct CardStyle: ViewModifier {
    @Environment(\.theme) private var theme

    func body(content: Content) -> some View {
        content
            .padding(10)
            .background(theme.cardBackground, in: RoundedRectangle(cornerRadius: 10))
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(theme.cardBorder, lineWidth: 1),
            )
    }
}

extension View {
    func cardStyle() -> some View {
        modifier(CardStyle())
    }
}
