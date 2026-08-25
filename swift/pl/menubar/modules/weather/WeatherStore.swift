import AppKit
import Foundation

/// Owns the weather module's state: the resolved location, the latest
/// snapshot, loading/error feedback, and the refresh cadence.
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

    @Published private(set) var location: WeatherLocation?

    var lastUpdated: Date? {
        snapshot?.fetchedAt
    }

    private var timer: Timer?
    private var checkmarkTask: Task<Void, Never>?

    private static let providerKey = "AuraBar.weather.provider"
    private static let qweatherKeyKey = "AuraBar.weather.qweatherKey"
    private static let locationKey = "AuraBar.weather.location"
    private static let refreshInterval: TimeInterval = 30 * 60

    /// Fallback before the user picks a city.
    private static let defaultLocation = WeatherLocation(
        name: "北京",
        latitude: 39.9042,
        longitude: 116.4074,
    )

    init() {
        providerKind = WeatherProviderKind(
            rawValue: UserDefaults.standard.string(forKey: Self.providerKey) ?? "",
        ) ?? .openMeteo
        qweatherKey = UserDefaults.standard.string(forKey: Self.qweatherKeyKey) ?? ""
        if let data = UserDefaults.standard.data(forKey: Self.locationKey),
           let saved = try? JSONDecoder().decode(WeatherLocation.self, from: data)
        {
            location = saved
        } else {
            location = Self.defaultLocation
        }

        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification, object: nil, queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in await self?.refresh() }
        }
        startTimer()
        Task { await refresh() }
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

    /// Resolve a free-form city name, persist the best match and refetch.
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
            location = first
            if let data = try? JSONEncoder().encode(first) {
                UserDefaults.standard.set(data, forKey: Self.locationKey)
            }
            isLoading = false
            await refresh()
        } catch {
            lastError = error.localizedDescription
        }
    }

    func refresh() async {
        guard let location else {
            lastError = "请先在设置中填写城市"
            return
        }
        guard !isLoading else { return }
        isLoading = true
        defer { isLoading = false }
        do {
            let snap = try await makeProvider().fetch(location: location)
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
