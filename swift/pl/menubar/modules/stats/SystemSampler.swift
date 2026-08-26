import CoreWLAN
import Darwin
import Foundation

/// Pure sampling functions over Mach/Darwin kernel interfaces — no
/// third-party dependencies, no IOKit. Each call is a self-contained
/// snapshot; rates and percentages are derived by diffing consecutive
/// snapshots in SystemStatsStore.
enum SystemSampler {
    // MARK: - CPU

    /// Cumulative jiffies of one core, from PROCESSOR_CPU_LOAD_INFO.
    struct CPUTicks {
        var user: UInt64 = 0
        var system: UInt64 = 0
        var idle: UInt64 = 0
        var nice: UInt64 = 0

        var total: UInt64 {
            user + system + idle + nice
        }

        var busy: UInt64 {
            user + system + nice
        }
    }

    /// Per-core ticks, in core order.
    static func cpuTicks() -> [CPUTicks]? {
        var numCPUs: natural_t = 0
        var info: processor_info_array_t?
        var count = mach_msg_type_number_t(0)
        let kr = host_processor_info(
            mach_host_self(),
            PROCESSOR_CPU_LOAD_INFO,
            &numCPUs,
            &info,
            &count,
        )
        guard kr == KERN_SUCCESS, let info else { return nil }
        defer {
            let size = vm_size_t(count) * vm_size_t(MemoryLayout<integer_t>.stride)
            vm_deallocate(mach_task_self_, vm_address_t(bitPattern: info), size)
        }

        // CPU_LOAD_INFO_COUNT (= CPU_STATE_MAX = 4: user/system/idle/nice
        // per core) is a typed macro that Swift doesn't import.
        let stride = 4
        return (0 ..< Int(numCPUs)).map { cpu in
            let base = cpu * stride
            return CPUTicks(
                user: UInt64(bitPattern: Int64(info[base + Int(CPU_STATE_USER)])),
                system: UInt64(bitPattern: Int64(info[base + Int(CPU_STATE_SYSTEM)])),
                idle: UInt64(bitPattern: Int64(info[base + Int(CPU_STATE_IDLE)])),
                nice: UInt64(bitPattern: Int64(info[base + Int(CPU_STATE_NICE)])),
            )
        }
    }

    // MARK: - Memory

    /// Physical memory breakdown in the Activity Monitor sense.
    struct MemoryBreakdown {
        /// App Memory: internal pages minus purgeable.
        var app: UInt64
        var wired: UInt64
        var compressed: UInt64
        var total: UInt64

        /// "Memory Used": app + wired + compressed.
        var used: UInt64 {
            app + wired + compressed
        }
    }

    static func memory() -> MemoryBreakdown? {
        var stats = vm_statistics64()
        var count = mach_msg_type_number_t(
            MemoryLayout<vm_statistics64>.stride / MemoryLayout<integer_t>.stride,
        )
        let kr = withUnsafeMutablePointer(to: &stats) { ptr in
            ptr.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &count)
            }
        }
        guard kr == KERN_SUCCESS else { return nil }

        let pageSize = UInt64(vm_page_size)
        var total: UInt64 = 0
        var size = MemoryLayout<UInt64>.stride
        sysctlbyname("hw.memsize", &total, &size, nil, 0)
        return MemoryBreakdown(
            app: (UInt64(stats.internal_page_count) - UInt64(stats.purgeable_count)) * pageSize,
            wired: UInt64(stats.wire_count) * pageSize,
            compressed: UInt64(stats.compressor_page_count) * pageSize,
            total: total,
        )
    }

    // MARK: - Processes

    /// Per-process snapshot for the top-N lists.
    struct ProcessInfo {
        var pid: Int32
        var name: String
        /// user + system time since process start, in nanoseconds.
        var cpuTicks: UInt64
        /// Resident memory in bytes.
        var memory: UInt64
    }

    /// All user-visible processes with their task info. This is ~1k
    /// syscalls — call it off the main thread.
    static func processInfos() -> [ProcessInfo] {
        let byteSize = proc_listpids(UInt32(PROC_ALL_PIDS), 0, nil, 0)
        guard byteSize > 0 else { return [] }
        var pids = [pid_t](repeating: 0, count: Int(byteSize) / MemoryLayout<pid_t>.stride)
        let filled = proc_listpids(UInt32(PROC_ALL_PIDS), 0, &pids, byteSize)
        guard filled > 0 else { return [] }

        var infos: [ProcessInfo] = []
        infos.reserveCapacity(Int(filled) / MemoryLayout<pid_t>.stride)
        for pid in pids where pid > 0 {
            var info = proc_taskinfo()
            let size = proc_pidinfo(
                pid,
                PROC_PIDTASKINFO,
                0,
                &info,
                Int32(MemoryLayout<proc_taskinfo>.stride),
            )
            guard size == Int32(MemoryLayout<proc_taskinfo>.stride) else { continue }
            infos.append(ProcessInfo(
                pid: pid,
                name: processName(pid),
                cpuTicks: info.pti_total_user + info.pti_total_system,
                memory: info.pti_resident_size,
            ))
        }
        return infos
    }

    /// Executable basename via proc_pidpath, falling back to the short
    /// kernel comm name.
    private static func processName(_ pid: Int32) -> String {
        var path = [CChar](repeating: 0, count: Int(MAXPATHLEN))
        if proc_pidpath(pid, &path, UInt32(MAXPATHLEN)) > 0 {
            let full = String(cString: path)
            if !full.isEmpty {
                return (full as NSString).lastPathComponent
            }
        }
        var short = [CChar](repeating: 0, count: 64)
        proc_name(pid, &short, 64)
        return String(cString: short)
    }

    // MARK: - Interface info

    /// Snapshot of the primary network interface for the "current
    /// connection" card.
    struct InterfaceInfo {
        enum Kind {
            case wifi
            case ethernet
            case other
        }

        var kind: Kind
        /// SSID on Wi-Fi (nil without location permission — macOS 14
        /// treats SSID as location privacy), or the interface name.
        var title: String
        /// dBm, Wi-Fi only.
        var rssi: Int?
        /// Link rate in Mbps, Wi-Fi only.
        var transmitRate: Double?
        /// IPv4 of the primary interface.
        var localIP: String?
    }

    static func interfaceInfo() -> InterfaceInfo {
        if let iface = CWWiFiClient.shared().interface() {
            // ssid() returns nil when the app lacks location permission.
            let ssid = iface.ssid()
            return InterfaceInfo(
                kind: .wifi,
                title: ssid ?? "Wi-Fi",
                rssi: iface.rssiValue(),
                transmitRate: iface.transmitRate(),
                localIP: primaryIPv4(preferred: iface.interfaceName),
            )
        }
        let ip = primaryIPv4(preferred: nil)
        return InterfaceInfo(
            kind: .ethernet,
            title: "以太网",
            rssi: nil,
            transmitRate: nil,
            localIP: ip,
        )
    }

    /// IPv4 address of the preferred interface, else the first
    /// non-loopback one.
    private static func primaryIPv4(preferred: String?) -> String? {
        var addrs: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&addrs) == 0 else { return nil }
        defer { freeifaddrs(addrs) }

        var fallback: String?
        var ptr = addrs
        while let iface = ptr?.pointee {
            defer { ptr = iface.ifa_next }
            guard let addr = iface.ifa_addr,
                  Int32(addr.pointee.sa_family) == AF_INET else { continue }
            let name = String(cString: iface.ifa_name)
            guard name != "lo0" else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            let rc = getnameinfo(
                addr,
                socklen_t(MemoryLayout<sockaddr_in>.stride),
                &host,
                socklen_t(host.count),
                nil,
                0,
                NI_NUMERICHOST,
            )
            guard rc == 0 else { continue }
            let ip = String(cString: host)
            if name == preferred {
                return ip
            }
            if fallback == nil {
                fallback = ip
            }
        }
        return fallback
    }

    // MARK: - Network

    /// Cumulative received/transmitted bytes across all interfaces except
    /// the loopback. Note ifi_obytes/ifi_ibytes are 32-bit counters that
    /// wrap; the store treats a decreasing sample as a wrap and skips it.
    static func networkBytes() -> (rx: UInt64, tx: UInt64) {
        var addrs: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&addrs) == 0 else { return (0, 0) }
        defer { freeifaddrs(addrs) }

        var rx: UInt64 = 0
        var tx: UInt64 = 0
        var seen = Set<String>()
        var ptr = addrs
        while let iface = ptr?.pointee {
            defer { ptr = iface.ifa_next }
            guard let addr = iface.ifa_addr,
                  Int32(addr.pointee.sa_family) == AF_LINK,
                  let data = iface.ifa_data else { continue }
            let name = String(cString: iface.ifa_name)
            guard name != "lo0", !seen.contains(name) else { continue }
            seen.insert(name)
            let counters = data.assumingMemoryBound(to: if_data.self).pointee
            rx += UInt64(counters.ifi_ibytes)
            tx += UInt64(counters.ifi_obytes)
        }
        return (rx, tx)
    }
}
