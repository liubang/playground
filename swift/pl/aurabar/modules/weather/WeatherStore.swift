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
            autoRefresh()
        }
    }

    /// 和风天气 API key; only consulted when providerKind == .qweather.
    @Published var qweatherKey: String {
        didSet {
            UserDefaults.standard.set(qweatherKey, forKey: Self.qweatherKeyKey)
            // A freshly entered key takes effect immediately, without
            // waiting for the next manual refresh.
            if providerKind == .qweather {
                autoRefresh()
            }
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

    private var checkmarkTask: Task<Void, Never>?

    /// Fetches only matter while the module is visible (status item
    /// inserted or popover open) — a hidden weather module shouldn't
    /// hit the network every 30 minutes. The first activation after a
    /// pause refetches if the snapshot is older than one interval.
    private lazy var sampler = SamplingController(
        interval: Self.refreshInterval,
        tolerance: 300,
        onStart: { [weak self] in self?.refetchIfStale() },
        onTick: { [weak self] in
            Task { await self?.refresh() }
        },
    )

    func statusItemVisibilityChanged(_ visible: Bool) {
        sampler.statusItemVisibilityChanged(visible)
    }

    func popoverVisibilityChanged(_ open: Bool) {
        sampler.popoverVisibilityChanged(open)
    }

    private func refetchIfStale() {
        let stale = snapshot.map {
            Date().timeIntervalSince($0.fetchedAt) > Self.refreshInterval
        } ?? true
        if stale {
            Task { await refresh() }
        }
    }

    /// Unattended triggers (wake, provider switch) refetch only while
    /// the module is on screen; otherwise the next activation does.
    private func autoRefresh() {
        guard sampler.isActive else { return }
        Task { await refresh() }
    }

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
            Task { @MainActor in self?.autoRefresh() }
        }
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

    private func makeProvider() -> any WeatherProvider {
        switch providerKind {
        case .openMeteo: OpenMeteoProvider()
        case .qweather: QWeatherProvider(apiKey: qweatherKey)
        case .apple: AppleWeatherProvider()
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
        defer { endLoading() }
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
            // No manual isLoading reset here: selectLocation's refresh
            // is scheduled as a Task, which runs after the defer below
            // has already released the loading state.
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

    /// Concurrency contract: one fetch at a time, but a refresh
    /// requested mid-flight (city/provider switched while loading) is
    /// never dropped — it invalidates the in-flight result via
    /// fetchGeneration and is replayed when the current fetch ends, so
    /// the UI can never settle on "new city name + old city weather".
    func refresh() async {
        guard !isLoading else {
            refreshPending = true
            fetchGeneration += 1
            return
        }
        isLoading = true
        fetchGeneration += 1
        let generation = fetchGeneration
        defer { endLoading() }
        guard let target = await effectiveTarget() else { return }
        do {
            let snap = try await makeProvider().fetch(location: target)
            // Superseded while fetching — discard the stale result and
            // let the pending refresh publish its own.
            guard generation == fetchGeneration else { return }
            // Committed only on success: location always describes the
            // snapshot being shown, even in the error state.
            location = target
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
            guard generation == fetchGeneration else { return }
            lastError = error.localizedDescription
        }
    }

    private var fetchGeneration = 0
    private var refreshPending = false

    /// Release the loading state and replay a refresh that arrived
    /// while a fetch (or geocode) was in flight.
    private func endLoading() {
        isLoading = false
        if refreshPending {
            refreshPending = false
            Task { await refresh() }
        }
    }
}
