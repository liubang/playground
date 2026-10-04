import CoreLocation
import Foundation

/// Wraps CLLocationManager for the weather module's auto-location mode.
/// City-level weather needs no better than a few kilometers of accuracy,
/// which is faster and far less power-hungry than precise fixes.
///
/// Desktop Macs have no GPS: positioning comes from Wi-Fi scanning, so
/// an Ethernet-only machine (a typical home Mac mini) gets a wildly
/// inaccurate IP-level fix or nothing at all. When the CoreLocation fix
/// is missing or worse than `accuracyThreshold`, we fall back to an
/// independent IP geolocation service before giving up.
@MainActor
final class LocationService: NSObject, ObservableObject {
    @Published private(set) var authorization: CLAuthorizationStatus
    @Published private(set) var isLocating = false
    @Published private(set) var lastError: String?

    var authorizationDenied: Bool {
        authorization == .denied || authorization == .restricted
    }

    private var authorized: Bool {
        // macOS only ever grants .authorizedAlways.
        authorization == .authorizedAlways
    }

    private let manager = CLLocationManager()
    fileprivate var continuation: CheckedContinuation<CLLocation?, Never>?

    override init() {
        authorization = manager.authorizationStatus
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyThreeKilometers
    }

    /// Fixes worse than 50km are IP-level guesses from the system and
    /// not trustworthy even for city-level weather.
    private static let accuracyThreshold: CLLocationAccuracy = 50000

    /// Which source produced the last fix (for the settings UI).
    enum Source {
        case coreLocation
        case ipGeo
    }

    @Published private(set) var source: Source?

    /// Current coordinates, or nil when unauthorized/unavailable. Asks
    /// for permission first when the user hasn't been asked yet; falls
    /// back to IP geolocation when CoreLocation can't do better.
    func locate() async -> CLLocation? {
        if authorizationDenied {
            return nil
        }
        // A second locate() while one is in flight would clobber the
        // pending continuation and hang the first caller forever.
        guard !isLocating else { return nil }
        if authorization == .notDetermined {
            manager.requestWhenInUseAuthorization()
        }
        isLocating = true
        defer { isLocating = false }

        if let fix = await coreLocationFix(), fix.horizontalAccuracy <= Self.accuracyThreshold {
            source = .coreLocation
            return fix
        }
        // Ethernet-only desktop or timeout: try the IP-based fallback.
        if let fix = await ipGeoFix() {
            source = .ipGeo
            return fix
        }
        return nil
    }

    /// CoreLocation can stay silent indefinitely (no fix, no error),
    /// so race the request against a timeout: whichever path answers
    /// first resumes the continuation, the loser no-ops.
    private static let fixTimeout: Duration = .seconds(10)

    private func coreLocationFix() async -> CLLocation? {
        await withTaskGroup(of: CLLocation?.self) { group in
            group.addTask { @MainActor in
                await withCheckedContinuation { continuation in
                    self.continuation = continuation
                    if self.authorized {
                        self.manager.requestLocation()
                    }
                    // Otherwise the authorization-change delegate resumes it.
                }
            }
            group.addTask { @MainActor in
                try? await Task.sleep(for: Self.fixTimeout)
                // Resuming keeps the continuation from leaking; a late
                // delegate answer then finds continuation == nil.
                self.resume(with: nil)
                return nil
            }
            let fix = await group.next() ?? nil
            group.cancelAll()
            return fix
        }
    }

    /// IP-based fallback: ipinfo.io first (rate-limited on some shared
    /// egress IPs), then myip.ipip.net + system geocoding.
    private func ipGeoFix() async -> CLLocation? {
        if let fix = await ipinfoFix() {
            return fix
        }
        return await ipipFix()
    }

    /// The shared session's 60s default is far too patient for a
    /// location fallback — the user is staring at a loading spinner.
    private let ipGeoSession: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 10
        return URLSession(configuration: config)
    }()

    /// ipinfo.io: free HTTPS IP geolocation, "loc" is "lat,lon".
    private func ipinfoFix() async -> CLLocation? {
        guard let url = URL(string: "https://ipinfo.io/json"),
              let (data, _) = try? await ipGeoSession.data(from: url),
              let payload = try? JSONDecoder().decode(IPGeoPayload.self, from: data)
        else {
            return nil
        }
        let parts = payload.loc.split(separator: ",").compactMap { Double($0) }
        guard parts.count == 2 else { return nil }
        return CLLocation(latitude: parts[0], longitude: parts[1])
    }

    private struct IPGeoPayload: Decodable {
        let loc: String
    }

    /// myip.ipip.net returns "来自于：中国 省 市 运营商" as plain text —
    /// parse the city and resolve it with the system geocoder.
    private func ipipFix() async -> CLLocation? {
        guard let url = URL(string: "https://myip.ipip.net"),
              let (data, _) = try? await ipGeoSession.data(from: url),
              let text = String(data: data, encoding: .utf8),
              let range = text.range(of: "来自于：中国") else { return nil }
        let tail = text[range.upperBound...].trimmingCharacters(in: .whitespacesAndNewlines)
        let parts = tail.split(separator: " ")
        let city = parts.count >= 2 ? String(parts[1]) : parts.first.map(String.init)
        guard let city, !city.isEmpty else { return nil }
        guard let placemark = try? await CLGeocoder()
            .geocodeAddressString("\(city), 中国").first else { return nil }
        return placemark.location
    }

    /// City-level display name for a coordinate ("北京", "上海"…).
    func placeName(for location: CLLocation) async -> String? {
        guard let placemark = try? await CLGeocoder().reverseGeocodeLocation(location).first else {
            return nil
        }
        return placemark.locality
            ?? placemark.subAdministrativeArea
            ?? placemark.administrativeArea
    }

    fileprivate func resume(with location: CLLocation?) {
        guard let continuation else { return }
        self.continuation = nil
        continuation.resume(returning: location)
    }
}

/// Delegate callbacks arrive on the main run loop (the manager was
/// created on the main thread), so hopping onto the main actor with
/// assumeIsolated is safe.
extension LocationService: @preconcurrency CLLocationManagerDelegate {
    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        MainActor.assumeIsolated {
            authorization = manager.authorizationStatus
            switch authorization {
            case .authorizedAlways:
                if continuation != nil {
                    manager.requestLocation()
                }
            case .denied, .restricted:
                resume(with: nil)
            default:
                break
            }
        }
    }

    func locationManager(_: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        MainActor.assumeIsolated {
            // Per CLLocationManager docs the last element is the most
            // recent fix.
            resume(with: locations.last)
        }
    }

    func locationManager(_: CLLocationManager, didFailWithError error: Error) {
        MainActor.assumeIsolated {
            lastError = error.localizedDescription
            resume(with: nil)
        }
    }
}
