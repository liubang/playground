/// Carbon key codes and modifier masks.
///
/// The Swift importer no longer surfaces kVK_* / cmdKey / shiftKey from
/// Carbon.HIToolbox, so the handful we use is restated here. Values are
/// the long-stable Carbon ABI constants (HIToolbox/Events.h).
enum Carbon {
    enum KeyCode {
        static let ansiO: UInt32 = 0x1F
        static let ansiX: UInt32 = 0x07
        static let escape: UInt16 = 0x35
        static let ansiReturn: UInt16 = 0x24
        static let keypadEnter: UInt16 = 0x4C
    }

    enum Modifier {
        static let cmd: UInt32 = 1 << 8 // cmdKey
        static let shift: UInt32 = 1 << 9 // shiftKey
        static let option: UInt32 = 1 << 11 // optionKey
        static let control: UInt32 = 1 << 12 // controlKey
    }
}
