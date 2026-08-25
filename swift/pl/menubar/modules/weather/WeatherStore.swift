import AppKit
import Foundation

/// Owns the weather module's state: the location mode (auto via
/// CoreLocation, or a manually picked city), the saved city list, the
/// latest snapshot, loading/error feedback, and the refresh cadence.
///
/// Refresh triggers: a 30-minute repeating timer (heavily tolerated so it
/// coalesces with other system work), app launch, wake from sleep, and
/// manual refreshes from the popover.
@MainActor
final class WeatherStore: ObservableObject {
    @Published private(set) var snapshot: WeatherSnapshot?
    @Published private(set) var isLoading = false
    @Published private(set) var justRefreshed = false
    @Published private(set) var lastError: String?

    @Published var providerKind: WeatherProviderKind {
        didSet {
            UserDefaults.standard.set(providerKind.rawValue, forKey: Self.providerKey)
            Task { await refresh() }
        }
    }

    /// 和风天气 API key; only consulted when providerKind == .qweather.
    @Published var qweatherKey: String {
        didSet {
            UserDefaults.standard.set(qweatherKey, forKey: Self.qweatherKeyKey)
        }
    }

    /// Auto-location mode: the effective location is resolved via
    /// CoreLocation on every refresh. Persisted.
    @Published private(set) var autoLocation: Bool {
        didSet {
            UserDefaults.standard.set(autoLocation, forKey: Self.autoLocationKey)
        }
    }

    /// Cities the user added; persisted. The selected one is used when
    /// autoLocation is off.
    @Published private(set) var savedLocations: [WeatherLocation]

    /// The city used when autoLocation is off; persisted.
    @Published private(set) var manualSelection: WeatherLocation

    /// The location the current snapshot was fetched for.
    @Published private(set) var location: WeatherLocation?

    /// CoreLocation front-end for auto mode; exposed for the popover's
    /// permission UI.
    let locationService = LocationService()

    var lastUpdated: Date? {
        snapshot?.fetchedAt
    }

    private var timer: Timer?
    private var checkmarkTask: Task<Void, Never>?

    private static let providerKey = "AuraBar.weather.provider"
    private static let qweatherKeyKey = "AuraBar.weather.qweatherKey"
    private static let locationKey = "AuraBar.weather.location"
    private static let autoLocationKey = "AuraBar.weather.autoLocation"
    private static let savedLocationsKey = "AuraBar.weather.savedLocations"
    private static let refreshInterval: TimeInterval = 30 * 60

    /// Fallback before the user picks a city.
    private static let defaultLocation = WeatherLocation(
        name: "北京",
        latitude: 39.9042,
        longitude: 116.4074,
    )

    init() {
        let defaults = UserDefaults.standard
        providerKind = WeatherProviderKind(
            rawValue: defaults.string(forKey: Self.providerKey) ?? "",
        ) ?? .openMeteo
        qweatherKey = defaults.string(forKey: Self.qweatherKeyKey) ?? ""
        autoLocation = defaults.bool(forKey: Self.autoLocationKey)

        let saved = Self.decode(WeatherLocation.self, forKey: Self.locationKey)
            ?? Self.defaultLocation
        manualSelection = saved
        // Migration: previously a single stored location — seed the list
        // with it.
        savedLocations = Self.decode([WeatherLocation].self, forKey: Self.savedLocationsKey)
            ?? [saved]
        location = saved

        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification,
            object: nil,
            queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in await self?.refresh() }
        }
        startTimer()
        Task { await refresh() }
    }

    private static func decode<T: Decodable>(_: T.Type, forKey key: String) -> T? {
        guard let data = UserDefaults.standard.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }

    private static func encode(_ value: some Encodable, forKey key: String) {
        if let data = try? JSONEncoder().encode(value) {
            UserDefaults.standard.set(data, forKey: key)
        }
    }

    private func startTimer() {
        let t = Timer(timeInterval: Self.refreshInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.refresh() }
        }
        t.tolerance = 300
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func makeProvider() -> any WeatherProvider {
        switch providerKind {
        case .openMeteo: OpenMeteoProvider()
        case .qweather: QWeatherProvider(apiKey: qweatherKey)
        }
    }

    // MARK: - Location management

    /// Switch auto-location mode on/off and refetch.
    func setAutoLocation(_ enabled: Bool) {
        autoLocation = enabled
        if !enabled {
            location = manualSelection
        }
        Task { await refresh() }
    }

    /// Select a city from the saved list (turns auto mode off).
    func selectLocation(_ loc: WeatherLocation) {
        manualSelection = loc
        Self.encode(loc, forKey: Self.locationKey)
        autoLocation = false
        location = loc
        Task { await refresh() }
    }

    /// Geocode a free-form city name, add the best match to the saved
    /// list, select it and refetch.
    func setCity(_ city: String) async {
        let trimmed = city.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        isLoading = true
        defer { isLoading = false }
        do {
            let results = try await makeProvider().geocode(city: trimmed)
            guard let first = results.first else {
                lastError = WeatherError.cityNotFound(trimmed).localizedDescription
                return
            }
            if !savedLocations.contains(first) {
                savedLocations.append(first)
                Self.encode(savedLocations, forKey: Self.savedLocationsKey)
            }
            isLoading = false
            selectLocation(first)
        } catch {
            lastError = error.localizedDescription
        }
    }

    /// Remove a city from the saved list; falls back to the first
    /// remaining city when the selected one is removed.
    func removeLocation(_ loc: WeatherLocation) {
        savedLocations.removeAll { $0 == loc }
        Self.encode(savedLocations, forKey: Self.savedLocationsKey)
        guard manualSelection == loc else { return }
        manualSelection = savedLocations.first ?? Self.defaultLocation
        Self.encode(manualSelection, forKey: Self.locationKey)
        if !autoLocation {
            location = manualSelection
            Task { await refresh() }
        }
    }

    // MARK: - Refresh

    /// The location to fetch for, resolving CoreLocation in auto mode.
    private func effectiveTarget() async -> WeatherLocation? {
        guard autoLocation else { return manualSelection }
        guard let fix = await locationService.locate() else {
            if locationService.authorizationDenied {
                lastError = "定位未授权，可在系统设置中开启"
            } else {
                lastError = locationService.lastError ?? "无法获取当前位置"
            }
            return nil
        }
        let name = await locationService.placeName(for: fix) ?? "当前位置"
        return WeatherLocation(
            name: name,
            latitude: fix.coordinate.latitude,
            longitude: fix.coordinate.longitude,
        )
    }

    func refresh() async {
        guard !isLoading else { return }
        isLoading = true
        defer { isLoading = false }
        guard let target = await effectiveTarget() else { return }
        location = target
        do {
            let snap = try await makeProvider().fetch(location: target)
            snapshot = snap
            lastError = nil
            // Green checkmark flash in the refresh button.
            checkmarkTask?.cancel()
            justRefreshed = true
            checkmarkTask = Task {
                try? await Task.sleep(for: .seconds(1.5))
                guard !Task.isCancelled else { return }
                justRefreshed = false
            }
        } catch {
            lastError = error.localizedDescription
        }
    }
}
