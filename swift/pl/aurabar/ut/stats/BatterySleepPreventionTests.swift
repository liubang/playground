@testable import AuraBar
import Foundation
import XCTest

final class BatterySleepPreventionTests: XCTestCase {
    @MainActor
    private final class Activities {
        final class Token: NSObject {
            let options: ProcessInfo.ActivityOptions

            init(_ options: ProcessInfo.ActivityOptions) {
                self.options = options
            }
        }

        var started: [Token] = []
        var ended: [Token] = []

        var active: [Token] {
            started.filter { token in !ended.contains { $0 === token } }
        }

        func makePrevention(
            locked: Bool = false,
            active: Bool = true,
        ) -> BatterySleepPrevention {
            BatterySleepPrevention(
                sessionState: .init(screenLocked: locked, isActive: active),
                beginActivity: { options, _ in
                    let token = Token(options)
                    self.started.append(token)
                    return token
                },
                endActivity: { token in
                    guard let token = token as? Token else {
                        XCTFail("unexpected activity token")
                        return
                    }
                    XCTAssertFalse(self.ended.contains { $0 === token }, "released twice")
                    self.ended.append(token)
                },
            )
        }
    }

    func testUnlockedEnableAcquiresIndependentSystemAndDisplayActivities() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention()
            XCTAssertTrue(activities.active.isEmpty)

            prevention.setEnabled(true)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated, .idleDisplaySleepDisabled])
            XCTAssertTrue(activities.active[0].options.contains(.idleSystemSleepDisabled))
            XCTAssertFalse(activities.active[0].options.contains(.idleDisplaySleepDisabled))
            XCTAssertFalse(activities.active[1].options.contains(.idleSystemSleepDisabled))
        }
    }

    func testLockReleasesOnlyDisplayAndUnlockRestoresIt() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention()
            prevention.setEnabled(true)
            let system = activities.active[0]
            let display = activities.active[1]

            prevention.setScreenLocked(true)
            XCTAssertEqual(activities.active.count, 1)
            XCTAssertTrue(activities.active[0] === system)
            XCTAssertEqual(activities.ended.count, 1)
            XCTAssertTrue(activities.ended[0] === display)

            prevention.setScreenLocked(false)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated, .idleDisplaySleepDisabled])
            XCTAssertTrue(activities.active[0] === system)
            XCTAssertEqual(activities.started.count, 3)
            XCTAssertEqual(activities.ended.count, 1)
        }
    }

    func testStartingLockedDoesNotAcquireDisplayActivity() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention(locked: true)
            prevention.setEnabled(true)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated])

            prevention.setScreenLocked(false)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated, .idleDisplaySleepDisabled])
        }
    }

    func testInactiveSessionNeverKeepsDisplayAwakeEvenAfterUnlock() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention()
            prevention.setEnabled(true)
            let system = activities.active[0]

            let display = activities.active[1]
            prevention.setSessionActive(false)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated])
            XCTAssertEqual(activities.ended.count, 1)
            XCTAssertTrue(activities.ended[0] === display)

            prevention.setScreenLocked(true)
            prevention.setScreenLocked(false)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated])
            XCTAssertTrue(activities.active[0] === system)

            // Switching back to a locked session must not relight it.
            prevention.updateSessionState(.init(screenLocked: true, isActive: true))
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated])
            prevention.setScreenLocked(false)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated, .idleDisplaySleepDisabled])
            XCTAssertTrue(activities.active[0] === system)
        }
    }

    func testStartingInactiveOnlyPreventsSystemSleep() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention(active: false)
            prevention.setEnabled(true)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated])

            prevention.updateSessionState(.init(screenLocked: false, isActive: true))
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated, .idleDisplaySleepDisabled])
        }
    }

    func testDisableReleasesBothAndSessionChangesCannotReenable() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention()
            prevention.setEnabled(true)
            prevention.setEnabled(false)
            XCTAssertTrue(activities.active.isEmpty)
            XCTAssertEqual(activities.ended.count, 2)

            prevention.setScreenLocked(true)
            prevention.setScreenLocked(false)
            prevention.setSessionActive(true)
            prevention.updateSessionState(.init(screenLocked: false, isActive: true))
            XCTAssertTrue(activities.active.isEmpty)
            XCTAssertEqual(activities.started.count, 2)
        }
    }

    func testDisableWhileLockedAndReenableKeepsOnlySystemAwake() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention()
            prevention.setEnabled(true)
            prevention.setScreenLocked(true)
            prevention.setEnabled(false)
            XCTAssertTrue(activities.active.isEmpty)

            prevention.setEnabled(true)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated])
            prevention.setScreenLocked(false)
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated, .idleDisplaySleepDisabled])
        }
    }

    func testRepeatedNotificationsAndToggleAreIdempotent() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention()
            prevention.setEnabled(true)
            prevention.setEnabled(true)
            prevention.setScreenLocked(false)
            prevention.setSessionActive(true)
            XCTAssertEqual(activities.started.count, 2)

            prevention.setScreenLocked(true)
            prevention.setScreenLocked(true)
            XCTAssertEqual(activities.ended.count, 1)
            prevention.setScreenLocked(false)
            prevention.setScreenLocked(false)
            XCTAssertEqual(activities.started.count, 3)

            prevention.setEnabled(false)
            prevention.setEnabled(false)
            XCTAssertTrue(activities.active.isEmpty)
            XCTAssertEqual(activities.ended.count, 3)
        }
    }

    func testWakeResamplesLockedStateWithoutDroppingSystemActivity() async {
        await MainActor.run {
            let activities = Activities()
            let prevention = activities.makePrevention()
            prevention.setEnabled(true)
            let system = activities.active[0]

            prevention.updateSessionState(.init(screenLocked: true, isActive: true))
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated])
            XCTAssertTrue(activities.active[0] === system)

            prevention.updateSessionState(.init(screenLocked: false, isActive: true))
            XCTAssertEqual(activities.active.map(\.options), [.userInitiated, .idleDisplaySleepDisabled])
            XCTAssertTrue(activities.active[0] === system)
        }
    }

    func testDeinitReleasesActivitiesUnlockedAndLocked() async {
        await MainActor.run {
            for locked in [false, true] {
                let activities = Activities()
                var prevention: BatterySleepPrevention? = activities.makePrevention(locked: locked)
                prevention?.setEnabled(true)
                XCTAssertEqual(activities.active.count, locked ? 1 : 2)

                prevention = nil
                XCTAssertTrue(activities.active.isEmpty)
                XCTAssertEqual(activities.ended.count, locked ? 1 : 2)
            }
        }
    }
}
