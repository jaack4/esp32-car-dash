import SwiftUI

/// Ties the Bluetooth link to the weather and the phone battery: whenever the dash connects,
/// send it the weather (refreshing if it's over 10 minutes old) and the battery level, then
/// send updates as they happen.
final class CarDash {
    static let shared = CarDash()

    let link = DashLink()
    let weather = WeatherService()

    private init() {
        link.onReady = { [weak self] in
            self?.weather.refresh(maxAge: 10 * 60)
            self?.sendBattery()
        }
        weather.onUpdate = { [weak self] report in
            self?.link.send(report.payload())
            self?.sendBattery()
        }
        weather.start()

        UIDevice.current.isBatteryMonitoringEnabled = true
        for name in [UIDevice.batteryLevelDidChangeNotification, UIDevice.batteryStateDidChangeNotification] {
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.sendBattery()
            }
        }
    }

    private func sendBattery() {
        let device = UIDevice.current
        guard device.batteryLevel >= 0 else { return }   // unknown (e.g. simulator)
        let charging = device.batteryState == .charging || device.batteryState == .full
        link.send("B|\(Int((device.batteryLevel * 100).rounded()))|\(charging ? 1 : 0)")
    }
}

/// iOS relaunches the app in the background for Bluetooth and location events without showing
/// any UI, so the connection is set up here rather than in a view.
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        _ = CarDash.shared
        return true
    }
}

@main
struct CarDashApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView(link: CarDash.shared.link, weather: CarDash.shared.weather)
        }
    }
}
