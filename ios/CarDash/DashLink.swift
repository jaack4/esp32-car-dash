import CoreBluetooth
import Foundation

/// Bluetooth LE connection to the ESP32. Once the board has been found, it's remembered and
/// iOS keeps a pending connection open, so the app reconnects in the background whenever the
/// board powers on with the car, even when the app isn't running.
final class DashLink: NSObject, ObservableObject {
    static let serviceUUID = CBUUID(string: "6D0F0001-8F4C-4C38-9D51-6A4B1B0C2A10")
    static let dataUUID = CBUUID(string: "6D0F0002-8F4C-4C38-9D51-6A4B1B0C2A10")
    private static let savedIDKey = "dashPeripheralID"

    enum Status: String {
        case bluetoothOff = "Bluetooth off"
        case searching = "Searching for the dash"
        case waiting = "Waiting for the dash to power on"
        case connected = "Connected"
    }

    enum ReverbPreset: String, CaseIterable, Identifiable, Hashable {
        case concert, club
        var id: String { rawValue }
        var title: String { self == .concert ? "Concert" : "Club" }
        var summary: String {
            self == .concert
                ? "Longer, spacious venue ambience."
                : "Tighter, darker ambience with less bass in the reverb."
        }
    }

    /// Audio effects on the dash. All of them start off whenever the board powers on.
    struct AudioEffects: Equatable {
        var reverbOn = false
        var venue: ReverbPreset = .concert
        var eqOn = false
        var bass = 0, mid = 0, treble = 0   // dB, -12...12
        var bassBoostOn = false
        var bassBoostLevel = 2              // 1 light, 2 medium, 3 heavy
        var widenerOn = false
        var widthLevel = 2                  // 1 subtle, 2 wide, 3 extra

        /// Firmware commands (see phone_link.h) that turn `self` into `target`.
        func commands(to target: AudioEffects) -> [String] {
            func flag(_ on: Bool) -> Int { on ? 1 : 0 }
            var out: [String] = []
            if reverbOn != target.reverbOn || venue != target.venue {
                out.append("R|\(flag(target.reverbOn))|\(target.venue.rawValue)")
            }
            if eqOn != target.eqOn || bass != target.bass || mid != target.mid || treble != target.treble {
                out.append("EQ|\(flag(target.eqOn))|\(target.bass)|\(target.mid)|\(target.treble)")
            }
            if bassBoostOn != target.bassBoostOn || bassBoostLevel != target.bassBoostLevel {
                out.append("BASS|\(flag(target.bassBoostOn))|\(target.bassBoostLevel)")
            }
            if widenerOn != target.widenerOn || widthLevel != target.widthLevel {
                out.append("WIDE|\(flag(target.widenerOn))|\(target.widthLevel)")
            }
            return out
        }

        /// Parses the firmware's status: `FX|<reverb available>|<reverb on>|<venue>|<eq on>|<bass>|
        /// <mid>|<treble>|<bass boost on>|<level>|<widener on>|<level>`.
        static func parse(_ value: String) -> (effects: AudioEffects, reverbAvailable: Bool)? {
            let f = value.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
            guard f.count == 12, f[0] == "FX", let venue = ReverbPreset(rawValue: f[3]) else { return nil }
            func flag(_ i: Int) -> Bool? { f[i] == "1" ? true : f[i] == "0" ? false : nil }
            func number(_ i: Int, _ range: ClosedRange<Int>) -> Int? { Int(f[i]).flatMap { range.contains($0) ? $0 : nil } }
            guard let available = flag(1), let reverbOn = flag(2), let eqOn = flag(4),
                  let bass = number(5, -12...12), let mid = number(6, -12...12), let treble = number(7, -12...12),
                  let boostOn = flag(8), let boostLevel = number(9, 1...3),
                  let widenerOn = flag(10), let widthLevel = number(11, 1...3) else { return nil }
            let effects = AudioEffects(reverbOn: reverbOn, venue: venue, eqOn: eqOn, bass: bass, mid: mid,
                                       treble: treble, bassBoostOn: boostOn, bassBoostLevel: boostLevel,
                                       widenerOn: widenerOn, widthLevel: widthLevel)
            return (effects, available)
        }
    }

    @Published private(set) var status: Status = .bluetoothOff
    @Published private(set) var lastSent: Date?
    @Published private(set) var effects = AudioEffects()
    /// The board has reported its effects (firmware that predates them never does).
    @Published private(set) var effectsAvailable = false
    /// Reverb needs a memory buffer; the other effects work without it.
    @Published private(set) var reverbAvailable = false
    @Published private(set) var effectsPending = false
    @Published private(set) var effectsError: String?

    /// Called whenever the link is ready to receive data (first connection or a reconnect).
    var onReady: (() -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var dataCharacteristic: CBCharacteristic?
    private var pendingWrites: [Bool] = []   // true for effect commands, in send order
    private var effectsTimeout: DispatchWorkItem?
    private var requestedEffects: AudioEffects?
    private var awaitedStatuses = 0           // the board sends one status per command

    override init() {
        super.init()
        // The restore identifier lets iOS relaunch the app in the background for Bluetooth events.
        central = CBCentralManager(delegate: self, queue: .main,
                                   options: [CBCentralManagerOptionRestoreIdentifierKey: "CarDashCentral"])
    }

    private static let effectPrefixes = ["R|", "EQ|", "BASS|", "WIDE|"]

    @discardableResult
    func send(_ payload: String) -> Bool {
        guard let peripheral, let dataCharacteristic, peripheral.state == .connected else { return false }
        pendingWrites.append(Self.effectPrefixes.contains { payload.hasPrefix($0) })
        peripheral.writeValue(Data(payload.utf8), for: dataCharacteristic, type: .withResponse)
        lastSent = Date()
        return true
    }

    /// Change the dash's audio effects. The UI keeps showing the confirmed state; it updates
    /// when the board reports back that it applied the change.
    func updateEffects(_ change: (inout AudioEffects) -> Void) {
        guard status == .connected, effectsAvailable, !effectsPending else { return }
        var target = effects
        change(&target)
        if !reverbAvailable { target.reverbOn = false }
        let commands = effects.commands(to: target)
        guard !commands.isEmpty else { return }
        effectsError = nil
        requestedEffects = target
        awaitedStatuses = commands.count
        effectsPending = true
        for command in commands where !send(command) {
            finishEffectsRequest(error: "The dash disconnected. Try again when connected.")
            return
        }
        let timeout = DispatchWorkItem { [weak self] in
            guard let self, self.effectsPending else { return }
            self.finishEffectsRequest(error: "Couldn’t confirm the change. Try again.")
            if let peripheral = self.peripheral, let characteristic = self.dataCharacteristic {
                peripheral.readValue(for: characteristic)
            }
        }
        effectsTimeout?.cancel()
        effectsTimeout = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + 5, execute: timeout)
    }

    private func finishEffectsRequest(error: String?) {
        effectsTimeout?.cancel()
        effectsTimeout = nil
        requestedEffects = nil
        awaitedStatuses = 0
        effectsPending = false
        effectsError = error
    }

    private func resetEffectsState() {
        finishEffectsRequest(error: nil)
        effects = AudioEffects()
        effectsAvailable = false
        reverbAvailable = false
        pendingWrites.removeAll()
    }

    /// Forget the remembered board and search again (e.g. after swapping boards).
    func forget() {
        resetEffectsState()
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
        peripheral = nil
        dataCharacteristic = nil
        UserDefaults.standard.removeObject(forKey: Self.savedIDKey)
        connect()
    }

    private func connect() {
        guard central.state == .poweredOn else {
            status = .bluetoothOff
            return
        }
        if let peripheral {
            central.connect(peripheral)   // never times out: completes whenever the board appears
            status = .waiting
        } else if let saved = UserDefaults.standard.string(forKey: Self.savedIDKey),
                  let id = UUID(uuidString: saved),
                  let known = central.retrievePeripherals(withIdentifiers: [id]).first {
            adopt(known)
            central.connect(known)
            status = .waiting
        } else {
            central.scanForPeripherals(withServices: [Self.serviceUUID])
            status = .searching
        }
    }

    private func adopt(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
    }
}

extension DashLink: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state != .poweredOn { resetEffectsState() }
        connect()
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        if let restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            adopt(restored)
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        central.stopScan()
        adopt(peripheral)
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.savedIDKey)
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        dataCharacteristic = nil
        resetEffectsState()
        connect()   // re-arm the pending connection for the next time the car starts
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        connect()
    }
}

extension DashLink: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else { return }
        peripheral.discoverCharacteristics([Self.dataUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard let characteristic = service.characteristics?.first(where: { $0.uuid == Self.dataUUID }) else {
            return
        }
        dataCharacteristic = characteristic
        status = .connected
        if characteristic.properties.contains(.read) && characteristic.properties.contains(.notify) {
            peripheral.setNotifyValue(true, for: characteristic)
            peripheral.readValue(for: characteristic)
        }
        onReady?()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == Self.dataUUID, error == nil,
              let data = characteristic.value, let value = String(data: data, encoding: .utf8) else { return }
        guard let status = AudioEffects.parse(value) else {
            // A notification can be cut short if the link negotiated a small packet size;
            // a read always returns the whole value.
            if value.hasPrefix("FX|") { peripheral.readValue(for: characteristic) }
            return
        }
        effects = status.effects
        reverbAvailable = status.reverbAvailable
        effectsAvailable = true
        guard effectsPending else { return }
        awaitedStatuses -= 1
        if awaitedStatuses <= 0 {
            finishEffectsRequest(error: requestedEffects == effects ? nil : "The dash couldn’t apply that change. Try again.")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == Self.dataUUID, !pendingWrites.isEmpty else { return }
        let wasEffect = pendingWrites.removeFirst()
        if wasEffect, error != nil, effectsPending {
            finishEffectsRequest(error: "The change wasn’t sent. Try again.")
        }
    }
}
