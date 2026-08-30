import Foundation
import IOKit

/// Cumulative disk I/O counters from the kernel's block storage drivers,
/// plus the boot volume's capacity snapshot. Rates are derived by
/// diffing consecutive counter snapshots in DiskStore.
enum DiskSampler {
    /// (read, write) bytes since boot, summed across every
    /// IOBlockStorageDriver (internal + external disks). Each driver
    /// publishes a "Statistics" dictionary with cumulative "Bytes (Read)"
    /// / "Bytes (Write)" counters. nil only when no driver reports at
    /// all — never on a real Mac with storage attached.
    static func bytes() -> (read: UInt64, write: UInt64)? {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(
            kIOMainPortDefault,
            IOServiceMatching("IOBlockStorageDriver"),
            &iterator,
        ) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(iterator) }

        var read: UInt64 = 0
        var write: UInt64 = 0
        var found = false
        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            guard let stats = IORegistryEntryCreateCFProperty(
                service, "Statistics" as CFString, kCFAllocatorDefault, 0,
            )?.takeRetainedValue() as? [String: Any] else { continue }
            found = true
            read += (stats["Bytes (Read)"] as? NSNumber)?.uint64Value ?? 0
            write += (stats["Bytes (Write)"] as? NSNumber)?.uint64Value ?? 0
        }
        return found ? (read, write) : nil
    }

    /// Boot volume capacity in the Finder sense.
    struct VolumeInfo: Equatable, Sendable {
        var name: String
        var total: UInt64
        /// Space Finder counts as free (purgeable included).
        var available: UInt64

        var used: UInt64 {
            total - min(available, total)
        }
    }

    /// The boot volume's capacity snapshot. Uses
    /// volumeAvailableCapacityForImportantUsage — the number Finder and
    /// 关于本机 report — rather than the raw free-block count, so
    /// purgeable space (caches, deleted-but-held snapshots) reads as
    /// free like the user expects.
    static func bootVolume() -> VolumeInfo? {
        let keys: Set<URLResourceKey> = [
            .volumeNameKey,
            .volumeTotalCapacityKey,
            .volumeAvailableCapacityForImportantUsageKey,
        ]
        guard let values = try? URL(fileURLWithPath: "/").resourceValues(forKeys: keys),
              let total = values.volumeTotalCapacity, total > 0,
              let available = values.volumeAvailableCapacityForImportantUsage, available >= 0
        else { return nil }
        return VolumeInfo(
            name: values.volumeName ?? "Macintosh HD",
            total: UInt64(total),
            available: UInt64(available),
        )
    }
}
