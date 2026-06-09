import CoreLocation
import Foundation

// Streams the phone's GPS (position + course + speed) to the device over BLE
// so the device can use it as own-ship before the LC76G is soldered. Wire
// format: BleHostGps in shared/ble/AisRadarBle.h.
final class LocationPublisher: NSObject, CLLocationManagerDelegate {
    private let manager = CLLocationManager()
    var onLocation: ((Data) -> Void)?
    private var lastData: Data?
    private var timer: Timer?

    override init() {
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyBest
        manager.distanceFilter = 5
    }

    func start() {
        switch manager.authorizationStatus {
        case .notDetermined: manager.requestWhenInUseAuthorization()
        case .authorizedWhenInUse, .authorizedAlways: manager.startUpdatingLocation()
        default: break
        }
        // Re-send the last fix every 2 s so the device's host-GPS never goes
        // stale (iOS throttles updates when stationary).
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            if let d = self?.lastData { self?.onLocation?(d) }
        }
    }

    func locationManagerDidChangeAuthorization(_ m: CLLocationManager) {
        if m.authorizationStatus == .authorizedWhenInUse || m.authorizationStatus == .authorizedAlways {
            m.startUpdatingLocation()
        }
    }

    func locationManager(_ m: CLLocationManager, didUpdateLocations locs: [CLLocation]) {
        guard let l = locs.last else { return }
        let cog: Double? = l.course >= 0 ? l.course : nil
        let sog: Double? = l.speed >= 0 ? l.speed * 1.943844 : nil   // m/s → kn
        let d = packHostGps(lat: l.coordinate.latitude, lon: l.coordinate.longitude,
                            cogDeg: cog, sogKn: sog)
        lastData = d
        onLocation?(d)
    }
}

func packHostGps(lat: Double, lon: Double, cogDeg: Double?, sogKn: Double?) -> Data {
    var d = Data()
    func i32(_ v: Int32) { var x = v.littleEndian; withUnsafeBytes(of: &x) { d.append(contentsOf: $0) } }
    func i16(_ v: Int16) { var x = v.littleEndian; withUnsafeBytes(of: &x) { d.append(contentsOf: $0) } }
    func clampI16(_ v: Double) -> Int16 { Int16(max(-32767, min(32767, v.rounded()))) }
    i32(Int32((lat * 1e7).rounded()))
    i32(Int32((lon * 1e7).rounded()))
    i16(cogDeg.map { clampI16($0 * 10) } ?? Int16.min)
    i16(sogKn.map { clampI16($0 * 10) } ?? Int16.min)
    return d
}
