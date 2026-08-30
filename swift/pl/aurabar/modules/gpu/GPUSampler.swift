import Foundation
import IOKit

/// GPU snapshot: display name, live device utilization and — when the
/// driver reports it — GPU-allocated memory.
struct GPUInfo: Equatable, Sendable {
    /// Display title ("Apple M4 Pro", "AMD Radeon Pro 5500M"…).
    var name: String
    /// 0...1, device utilization in the driver sense.
    var utilization: Double
    /// GPU-allocated memory in bytes when the driver reports it. On
    /// Apple silicon this is unified memory currently held by the GPU.
    var memoryUsed: UInt64?
}

/// Reads the primary GPU through IOKit's IOAccelerator class. Every Mac
/// exposes at least one accelerator and each publishes a
/// "PerformanceStatistics" dictionary; its keys vary by vendor:
/// - Apple (AGXAccelerator*): "Device Utilization %",
///   "In use system memory" / "Alloc system memory" (bytes, unified memory)
/// - Intel: "Utilization %"
/// - AMD: "GPU Activity(%)"
///
/// Properties are fetched one at a time (CreateCFProperty): the full
/// property set of an AGX accelerator includes the driver's
/// IOReportLegend — a several-hundred-kilobyte histogram schema that
/// copying every sample would brand the menu bar its own benchmark.
enum GPUSampler {
    static func info() -> GPUInfo? {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(
            kIOMainPortDefault,
            IOServiceMatching("IOAccelerator"),
            &iterator,
        ) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(iterator) }

        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            guard let stats = property("PerformanceStatistics", of: service) as? [String: Any],
                  let utilization = utilization(from: stats)
            else { continue }
            return GPUInfo(
                name: name(for: service),
                utilization: min(max(utilization / 100, 0), 1),
                memoryUsed: memory(from: stats),
            )
        }
        return nil
    }

    /// Utilization percentage under whichever vendor key the driver
    /// uses; discrete GPUs can exceed 100, clamped by the caller.
    private static func utilization(from stats: [String: Any]) -> Double? {
        for key in ["Device Utilization %", "Utilization %", "GPU Activity(%)"] {
            if let value = (stats[key] as? NSNumber)?.doubleValue {
                return value
            }
        }
        return nil
    }

    /// GPU-allocated memory in bytes. Apple silicon exposes both "In
    /// use" and "Alloc" unified-memory counters — "In use" is the
    /// Activity Monitor reading; desktop GPUs may expose neither.
    private static func memory(from stats: [String: Any]) -> UInt64? {
        for key in ["In use system memory", "Alloc system memory"] {
            if let value = (stats[key] as? NSNumber)?.uint64Value {
                return value
            }
        }
        return nil
    }

    /// Display title resolution order: the accelerator's own "model"
    /// string ("Apple M4 Pro" on AGX), then the PCI parent's model
    /// ("Intel UHD Graphics 630", "AMD Radeon Pro 5500M" on discrete
    /// GPUs), then the SoC brand via sysctl, then a generic fallback.
    private static func name(for service: io_registry_entry_t) -> String {
        if let model = modelString(of: service) {
            return model
        }
        if let model = pciModelName(for: service) {
            return model
        }
        var size = 0
        sysctlbyname("machdep.cpu.brand_string", nil, &size, nil, 0)
        if size > 0 {
            var buffer = [CChar](repeating: 0, count: size)
            sysctlbyname("machdep.cpu.brand_string", &buffer, &size, nil, 0)
            let brand = String(cString: buffer)
            if !brand.isEmpty {
                return brand
            }
        }
        return "GPU"
    }

    /// The accelerator's PCI parent registry entry carries the marketed
    /// GPU name in its "model" property. nil on Apple silicon, where the
    /// AGX accelerator is not a PCI device.
    private static func pciModelName(for service: io_registry_entry_t) -> String? {
        var parent: io_registry_entry_t = 0
        // kIOPCIDevicePlane ("IOPCIDevice") isn't exposed to Swift —
        // IOKit.pci has no Swift module — use the plane name directly.
        guard IORegistryEntryGetParentEntry(
            service,
            "IOPCIDevice",
            &parent,
        ) == KERN_SUCCESS, parent != 0 else { return nil }
        defer { IOObjectRelease(parent) }
        return modelString(of: parent)
    }

    /// A registry entry's "model" string. Registry strings arrive as
    /// NUL-terminated CFData on some nodes and CFString on others.
    private static func modelString(of entry: io_registry_entry_t) -> String? {
        switch property("model", of: entry) {
        case let string as String:
            return string.isEmpty ? nil : string
        case let data as Data:
            let string = String(data: data, encoding: .utf8)?
                .trimmingCharacters(in: CharacterSet(charactersIn: "\0"))
            return string?.isEmpty == false ? string : nil
        default:
            return nil
        }
    }

    private static func property(_ name: String, of entry: io_registry_entry_t) -> Any? {
        IORegistryEntryCreateCFProperty(entry, name as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue()
    }
}
