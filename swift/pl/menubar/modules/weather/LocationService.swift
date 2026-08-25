import CoreLocation
import Foundation

/// Wraps CLLocationManager for the weather module's auto-location mode.
/// City-level weather needs no better than a few kilometers of accuracy,
/// which is faster and far less power-hungry than precise fixes.
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

    /// Current coordinates, or nil when unauthorized/unavailable. Asks
    /// for permission first when the user hasn't been asked yet.
    func locate() async -> CLLocation? {
        if authorizationDenied {
            return nil
        }
        if authorization == .notDetermined {
            manager.requestWhenInUseAuthorization()
        }
        isLocating = true
        defer { isLocating = false }
        return await withCheckedContinuation { continuation in
            self.continuation = continuation
            if authorized {
                manager.requestLocation()
            }
            // Otherwise the authorization-change delegate resumes it.
        }
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
            resume(with: locations.first)
        }
    }

    func locationManager(_: CLLocationManager, didFailWithError error: Error) {
        MainActor.assumeIsolated {
            lastError = error.localizedDescription
            resume(with: nil)
        }
    }
}
