import ServiceManagement

/// Login-item registration backed by SMAppService (macOS 13+; the app
/// targets 14). The status is read straight from the system rather
/// than mirrored into UserDefaults, so the checkbox always reflects
/// reality — including changes made in System Settings → 登录项.
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
}
