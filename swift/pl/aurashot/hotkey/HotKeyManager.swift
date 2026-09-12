import AppKit
import Carbon

/// Global hotkeys via Carbon RegisterEventHotKey.
///
/// Carbon's hotkey API is ancient but remains the lightest way to get
/// system-wide shortcuts without the Accessibility permission — events
/// are delivered on the main run loop to GetApplicationEventTarget(),
/// so no extra isolation is needed around the action table.
final class HotKeyManager {
    /// 'ACLP' — identifies AuraShot's hotkeys among all Carbon clients.
    private static let signature = OSType(0x4143_4C50)

    /// Shared instance: the settings window's recorder needs to suspend
    /// the registrations while it listens for a new combo, otherwise
    /// pressing e.g. ⌘⇧X into the recorder would trigger a capture
    /// instead of being recorded.
    static let shared = HotKeyManager()

    /// Called by resume() so the owner can re-register from Settings.
    var onResumeNeeded: (() -> Void)?

    private var refs: [UInt32: EventHotKeyRef] = [:]
    private var actions: [UInt32: () -> Void] = [:]
    private var handlerRef: EventHandlerRef?
    private var nextID: UInt32 = 1
    private var suspended = false

    /// Temporarily unregisters every hotkey (recorder capture).
    func suspend() {
        guard !suspended else { return }
        suspended = true
        unregisterAll()
    }

    /// Ends a suspension; the owner re-registers via onResumeNeeded.
    func resume() {
        guard suspended else { return }
        suspended = false
        onResumeNeeded?()
    }

    /// Registers a hotkey. `carbonModifiers` uses Carbon's modifier
    /// masks (cmdKey, shiftKey, optionKey, controlKey). Returns false
    /// when the combination is already taken by another app.
    @discardableResult
    func register(
        keyCode: UInt32,
        carbonModifiers: UInt32,
        action: @escaping () -> Void,
    ) -> Bool {
        guard !suspended else { return false }
        if handlerRef == nil {
            installHandler()
        }
        let id = nextID
        nextID += 1
        let hotKeyID = EventHotKeyID(signature: Self.signature, id: id)
        var ref: EventHotKeyRef?
        let status = RegisterEventHotKey(
            keyCode,
            carbonModifiers,
            hotKeyID,
            GetApplicationEventTarget(),
            0,
            &ref,
        )
        guard status == noErr, let ref else { return false }
        refs[id] = ref
        actions[id] = action
        return true
    }

    func unregisterAll() {
        for (_, ref) in refs {
            UnregisterEventHotKey(ref)
        }
        refs.removeAll()
        actions.removeAll()
    }

    private func installHandler() {
        var spec = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed),
        )
        let userData = Unmanaged.passUnretained(self).toOpaque()
        InstallEventHandler(
            GetApplicationEventTarget(),
            { _, event, userData -> OSStatus in
                guard let event, let userData else {
                    return OSStatus(eventNotHandledErr)
                }
                var hotKeyID = EventHotKeyID()
                GetEventParameter(
                    event,
                    UInt32(kEventParamDirectObject),
                    UInt32(typeEventHotKeyID),
                    nil,
                    MemoryLayout<EventHotKeyID>.size,
                    nil,
                    &hotKeyID,
                )
                let manager = Unmanaged<HotKeyManager>.fromOpaque(userData).takeUnretainedValue()
                manager.actions[hotKeyID.id]?()
                return noErr
            },
            1,
            &spec,
            userData,
            &handlerRef,
        )
    }

    deinit {
        unregisterAll()
        if let handlerRef {
            RemoveEventHandler(handlerRef)
        }
    }
}
