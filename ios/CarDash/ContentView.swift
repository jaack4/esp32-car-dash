import CoreLocation
import SwiftUI

struct ContentView: View {
    @ObservedObject var link: DashLink
    @ObservedObject var weather: WeatherService

    var body: some View {
        NavigationStack {
            List {
                Section("Dash") {
                    HStack {
                        Circle()
                            .fill(link.status == .connected ? .green : .orange)
                            .frame(width: 10, height: 10)
                        Text(link.status.rawValue)
                    }
                    if let sent = link.lastSent {
                        LabeledContent("Last sent") { Text(sent, style: .relative) + Text(" ago") }
                    }
                    Button("Send weather now") { weather.refresh() }
                        .disabled(link.status != .connected)
                }

                Section {
                    Toggle("Bass boost", isOn: effect(\.bassBoostOn))
                    if link.effects.bassBoostOn {
                        Picker("Strength", selection: effect(\.bassBoostLevel)) {
                            Text("Light").tag(1)
                            Text("Medium").tag(2)
                            Text("Heavy").tag(3)
                        }
                        .pickerStyle(.segmented)
                    }

                    Toggle("Equalizer", isOn: effect(\.eqOn))
                    if link.effects.eqOn {
                        GainSlider(label: "Bass", value: link.effects.bass, pending: link.effectsPending) { db in
                            link.updateEffects { $0.bass = db }
                        }
                        GainSlider(label: "Mid", value: link.effects.mid, pending: link.effectsPending) { db in
                            link.updateEffects { $0.mid = db }
                        }
                        GainSlider(label: "Treble", value: link.effects.treble, pending: link.effectsPending) { db in
                            link.updateEffects { $0.treble = db }
                        }
                        Button("Reset to flat") {
                            link.updateEffects { $0.bass = 0; $0.mid = 0; $0.treble = 0 }
                        }
                    }

                    Toggle("Stereo widener", isOn: effect(\.widenerOn))
                    if link.effects.widenerOn {
                        Picker("Width", selection: effect(\.widthLevel)) {
                            Text("Subtle").tag(1)
                            Text("Wide").tag(2)
                            Text("Extra").tag(3)
                        }
                        .pickerStyle(.segmented)
                    }

                    Toggle("Reverb", isOn: effect(\.reverbOn))
                        .disabled(!link.reverbAvailable)
                    if link.effects.reverbOn {
                        Picker("Venue", selection: effect(\.venue)) {
                            ForEach(DashLink.ReverbPreset.allCases) { preset in
                                Text(preset.title).tag(preset)
                            }
                        }
                    }

                    if link.effectsPending {
                        Text("Applying…").foregroundStyle(.secondary)
                    }
                    if let error = link.effectsError {
                        Text(error).foregroundStyle(.red).font(.footnote)
                    } else if link.status == .connected && !link.effectsAvailable {
                        Text("This dash’s firmware doesn’t have audio effects yet.")
                            .foregroundStyle(.secondary).font(.footnote)
                    } else if link.effectsAvailable && !link.reverbAvailable {
                        Text("Reverb is unavailable: the dash is short on memory.")
                            .foregroundStyle(.secondary).font(.footnote)
                    }
                } header: {
                    Text("Audio")
                } footer: {
                    Text(audioFooter)
                }
                .disabled(link.status != .connected || !link.effectsAvailable || link.effectsPending)

                Section("Weather") {
                    if let r = weather.report {
                        LabeledContent("City", value: r.city)
                        LabeledContent("Now", value: "\(r.temperature)° \(r.condition)")
                        LabeledContent("High / low", value: "\(r.high)° / \(r.low)°")
                        LabeledContent("Wind", value: "\(r.wind) mph")
                        LabeledContent("Rain", value: rainLabel(r.rainMinutes))
                        LabeledContent("Updated") { Text(r.fetched, style: .relative) + Text(" ago") }
                    } else {
                        Text("Waiting for your location…").foregroundStyle(.secondary)
                    }
                    if let error = weather.lastError {
                        Text(error).foregroundStyle(.red).font(.footnote)
                    }
                }

                Section {
                    LabeledContent("Location access", value: locationLabel)
                    if weather.authorization != .authorizedAlways {
                        Button("Change to Always in Settings") {
                            UIApplication.shared.open(URL(string: UIApplication.openSettingsURLString)!)
                        }
                    }
                } footer: {
                    Text("With Always, the dash gets your city and weather as soon as the car starts, "
                         + "without opening this app. Don't swipe the app away in the app switcher.")
                }

                Section {
                    Button("Forget this dash", role: .destructive) { link.forget() }
                }
            }
            .navigationTitle("Car Dash")
            .onAppear { weather.refresh(maxAge: 10 * 60) }
        }
    }

    /// A binding to one audio effect setting that sends changes to the dash.
    private func effect<T>(_ keyPath: WritableKeyPath<DashLink.AudioEffects, T>) -> Binding<T> {
        Binding(get: { link.effects[keyPath: keyPath] },
                set: { value in link.updateEffects { $0[keyPath: keyPath] = value } })
    }

    private var audioFooter: String {
        var text = "Every effect starts off when the dash powers on."
        if link.effects.bassBoostOn || (link.effects.eqOn && max(link.effects.bass, link.effects.mid,
                                                                     link.effects.treble) > 0) {
            text += " Boosts lower the overall level a little so loud songs don’t distort; "
                + "turn the car up to make up for it."
        }
        if link.effects.reverbOn { text += " " + link.effects.venue.summary }
        return text
    }

    private func rainLabel(_ minutes: Int?) -> String {
        switch minutes {
        case nil: return "None in the next 2 hours"
        case 0?: return "Raining now"
        case let m?: return "In \(m) min"
        }
    }

    private var locationLabel: String {
        switch weather.authorization {
        case .authorizedAlways: return "Always"
        case .authorizedWhenInUse: return "While using"
        case .denied, .restricted: return "Off"
        default: return "Not asked yet"
        }
    }
}

/// -12...+12 dB slider that only sends its value when you let go, so dragging doesn't flood
/// the dash with commands. Follows the dash's confirmed value when you're not dragging, and
/// snaps back to it if a change didn't go through.
private struct GainSlider: View {
    let label: String
    let value: Int
    let pending: Bool
    let onCommit: (Int) -> Void
    @State private var draft = 0.0
    @State private var editing = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(label)
                Spacer()
                Text(Int(draft.rounded()) == 0 ? "0 dB" : String(format: "%+d dB", Int(draft.rounded())))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            }
            Slider(value: $draft, in: -12...12, step: 1) { isEditing in
                editing = isEditing
                if !isEditing && Int(draft.rounded()) != value { onCommit(Int(draft.rounded())) }
            }
        }
        .onAppear { draft = Double(value) }
        .onChange(of: value) { _, confirmed in
            if !editing { draft = Double(confirmed) }
        }
        .onChange(of: pending) { _, isPending in
            if !isPending && !editing { draft = Double(value) }
        }
    }
}
