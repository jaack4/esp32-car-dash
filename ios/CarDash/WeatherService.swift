import CoreLocation
import UIKit

struct WeatherReport {
    var city: String
    var temperature: Int
    var code: Int          // WMO weather code
    var isDay: Bool
    var high: Int
    var low: Int
    var wind: Int
    var rainMinutes: Int?  // minutes until rain starts, 0 = raining now, nil = none in the next 2 hours
    var fetched: Date

    var condition: String {
        switch code {
        case 0: return "Clear"
        case 1: return "Mostly clear"
        case 2: return "Partly cloudy"
        case 3: return "Overcast"
        case 45, 48: return "Fog"
        case 51...57: return "Drizzle"
        case 61...67: return "Rain"
        case 71...77: return "Snow"
        case 80...82: return "Showers"
        case 85, 86: return "Snow showers"
        case 95...: return "Thunderstorm"
        default: return "Cloudy"
        }
    }

    /// What the ESP32 expects (see phone_link.h). The board's font is ASCII only, so the city
    /// loses its accents ("San José" -> "San Jose").
    func payload(now: Date = Date()) -> String {
        let city = city.folding(options: .diacriticInsensitive, locale: nil)
            .filter { $0.isASCII && $0 != "|" }
            .prefix(20)
        // rain is counted from when the forecast was fetched; the board counts down from there
        let rain = rainMinutes.map { max(0, $0 - Int(now.timeIntervalSince(fetched) / 60)) } ?? -1
        let fields: [String] = [
            "W", String(Int(now.timeIntervalSince1970)), String(TimeZone.current.secondsFromGMT(for: now)),
            String(city), String(temperature), String(code), isDay ? "1" : "0",
            String(high), String(low), String(wind), String(rain),
        ]
        return fields.joined(separator: "|")
    }
}

/// Gets the phone's location, turns it into a city name, and fetches the weather from
/// Open-Meteo (free, no API key). With "Always" location permission, iOS also wakes the app
/// in the background after you've moved a few miles, so the city stays current while driving.
final class WeatherService: NSObject, ObservableObject {
    @Published private(set) var report: WeatherReport?
    @Published private(set) var authorization: CLAuthorizationStatus
    @Published private(set) var lastError: String?

    /// Called with every new report, and with the cached one when it's still fresh.
    var onUpdate: ((WeatherReport) -> Void)?

    private let manager = CLLocationManager()
    private let geocoder = CLGeocoder()
    private static let askedAlwaysKey = "askedAlwaysLocation"

    override init() {
        authorization = manager.authorizationStatus
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyKilometer
    }

    func start() {
        if manager.authorizationStatus == .notDetermined {
            manager.requestWhenInUseAuthorization()
        }
        manager.startMonitoringSignificantLocationChanges()
    }

    /// Fetch new weather, unless the last report is younger than `maxAge` seconds.
    func refresh(maxAge: TimeInterval = 0) {
        if let report, Date().timeIntervalSince(report.fetched) < maxAge {
            onUpdate?(report)
        } else {
            manager.requestLocation()
        }
    }

    private func update(for location: CLLocation) async {
        // keep running long enough to finish if iOS woke us in the background
        let task = await UIApplication.shared.beginBackgroundTask(withName: "weather")
        defer { Task { @MainActor in UIApplication.shared.endBackgroundTask(task) } }
        do {
            let placemark = try? await geocoder.reverseGeocodeLocation(location).first
            let city = placemark?.locality ?? placemark?.subAdministrativeArea ?? placemark?.name ?? "Here"
            let report = try await Self.fetchWeather(at: location.coordinate, city: city)
            await MainActor.run {
                self.report = report
                self.lastError = nil
                self.onUpdate?(report)
            }
        } catch {
            await MainActor.run { self.lastError = "Weather failed: \(error.localizedDescription)" }
        }
    }

    private static func fetchWeather(at c: CLLocationCoordinate2D, city: String) async throws -> WeatherReport {
        var url = URLComponents(string: "https://api.open-meteo.com/v1/forecast")!
        url.queryItems = [
            .init(name: "latitude", value: String(c.latitude)),
            .init(name: "longitude", value: String(c.longitude)),
            .init(name: "current", value: "temperature_2m,weather_code,wind_speed_10m,is_day,precipitation"),
            // rain in 15-minute steps for the next 2 hours, for the "Rain in 20 min" alert
            .init(name: "minutely_15", value: "precipitation"),
            .init(name: "past_minutely_15", value: "1"),
            .init(name: "forecast_minutely_15", value: "8"),
            .init(name: "timeformat", value: "unixtime"),
            .init(name: "daily", value: "temperature_2m_max,temperature_2m_min"),
            .init(name: "temperature_unit", value: "fahrenheit"),
            .init(name: "wind_speed_unit", value: "mph"),
            .init(name: "timezone", value: "auto"),
            .init(name: "forecast_days", value: "1"),
        ]
        let (data, _) = try await URLSession.shared.data(from: url.url!)
        let r = try JSONDecoder().decode(OpenMeteoResponse.self, from: data)
        return WeatherReport(
            city: city,
            temperature: Int(r.current.temperature_2m.rounded()),
            code: r.current.weather_code,
            isDay: r.current.is_day == 1,
            high: Int((r.daily.temperature_2m_max.first ?? r.current.temperature_2m).rounded()),
            low: Int((r.daily.temperature_2m_min.first ?? r.current.temperature_2m).rounded()),
            wind: Int(r.current.wind_speed_10m.rounded()),
            rainMinutes: minutesUntilRain(r),
            fetched: Date())
    }

    /// First 15-minute slot in the next 2 hours with at least 0.1 mm of rain.
    private static func minutesUntilRain(_ r: OpenMeteoResponse, now: Date = Date()) -> Int? {
        if (r.current.precipitation ?? 0) > 0 { return 0 }
        guard let slots = r.minutely_15 else { return nil }
        let nowT = now.timeIntervalSince1970
        for (time, mm) in zip(slots.time, slots.precipitation) {
            let start = TimeInterval(time)
            guard start + 15 * 60 > nowT, start < nowT + 2 * 3600 else { continue }
            if (mm ?? 0) >= 0.1 { return max(0, Int((start - nowT) / 60)) }
        }
        return nil
    }
}

extension WeatherService: CLLocationManagerDelegate {
    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        authorization = manager.authorizationStatus
        switch manager.authorizationStatus {
        case .authorizedWhenInUse:
            // iOS only shows the "Change to Always" prompt once, so only ask once
            if !UserDefaults.standard.bool(forKey: Self.askedAlwaysKey) {
                UserDefaults.standard.set(true, forKey: Self.askedAlwaysKey)
                manager.requestAlwaysAuthorization()
            }
            manager.requestLocation()
        case .authorizedAlways:
            manager.requestLocation()
        default:
            break
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let location = locations.last else { return }
        Task { await update(for: location) }
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        lastError = "Location failed: \(error.localizedDescription)"
    }
}

private struct OpenMeteoResponse: Decodable {
    struct Current: Decodable {
        let temperature_2m: Double
        let weather_code: Int
        let wind_speed_10m: Double
        let is_day: Int
        let precipitation: Double?
    }
    struct Minutely15: Decodable {
        let time: [Int]
        let precipitation: [Double?]
    }
    struct Daily: Decodable {
        let temperature_2m_max: [Double]
        let temperature_2m_min: [Double]
    }
    let current: Current
    let daily: Daily
    let minutely_15: Minutely15?
}
