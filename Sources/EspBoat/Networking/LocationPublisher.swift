// Networking/LocationPublisher.swift — iOS-side real GPS source.
//
// Round 85 v1.6 step 4. CoreLocation gives us the iPhone's fix; we
// repackage it as PGN 129025 (Position, Rapid Update) and unicast it
// to the AP on every update. The AP fans it out via the existing
// virtual-bus relay; LCDs + the iOS BoatState mirror dispatch 129025
// into BoatState already (no consumer change needed).
//
// PGN 129026 (COG/SOG, Rapid Update) is reserved for v1.6 step 5 when
// the LCD + iOS dispatchers learn it; until then we only publish
// 129025 so the data path is symmetric across all peers.
//
// Lifecycle: started by BusState on app foreground, stopped on
// background. Foreground-only matches iOS's foreground-location grant
// (Settings → app → Location → "While Using"). Background location
// would need NSLocationAlwaysAndWhenInUseUsageDescription + a
// UIBackgroundModes "location" entitlement — out of scope for v1.

import Foundation
import CoreLocation
import Network

final class LocationPublisher: NSObject {
    /// Read-only — last fix we sent, for the Diagnostics tab.
    private(set) var lastFix: CLLocation? = nil
    private(set) var isAuthorized: Bool = false
    private(set) var didSendCount: Int = 0

    private let manager = CLLocationManager()
    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "LocationPublisher.udp")
    private var isReady = false
    private var onUpdate: () -> Void

    init(onUpdate: @escaping () -> Void = {}) {
        self.onUpdate = onUpdate
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyBest
        // 5 m filter — boats don't move sub-metre. Cuts wasted publishes
        // when stationary while still being smooth enough for the map.
        manager.distanceFilter = 5
    }

    func start() {
        stop()
        let host = NWEndpoint.Host(Bus.apHost)
        let port = NWEndpoint.Port(rawValue: Bus.busPort)!
        let params: NWParameters = .udp
        params.prohibitedInterfaceTypes = [.cellular]   // see HeartbeatPublisher
        let c = NWConnection(host: host, port: port, using: params)
        c.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready: self.isReady = true
            default:     self.isReady = false
            }
        }
        c.start(queue: queue)
        connection = c

        // Ask for permission + start updates. On first launch iOS shows
        // the "Allow esp32-boat to use your location?" prompt; before
        // that .authorizationStatus is .notDetermined and updates fire
        // nothing. After grant, locationManager:didUpdateLocations: starts.
        switch manager.authorizationStatus {
        case .notDetermined:
            manager.requestWhenInUseAuthorization()
        case .authorizedWhenInUse, .authorizedAlways:
            isAuthorized = true
            manager.startUpdatingLocation()
        default:
            isAuthorized = false
        }
    }

    func stop() {
        manager.stopUpdatingLocation()
        connection?.cancel(); connection = nil
        isReady = false
    }

    private func publish(_ loc: CLLocation) {
        guard let c = connection, isReady else { return }
        // Match src/NmeaBridge.cpp::publishPosition wire shape exactly.
        // Lat / lon in degrees; six decimal places (~10 cm resolution).
        let payload =
            "{\"pgn\":129025,\"src\":255,\"peer\":\"\(Bus.peerName)\","
            + "\"fields\":{"
            + String(format: "\"latitude\":%.6f,\"longitude\":%.6f",
                     loc.coordinate.latitude, loc.coordinate.longitude)
            + "}}"
        guard let data = payload.data(using: .utf8) else { return }
        c.send(content: data, completion: .contentProcessed { _ in })
        didSendCount += 1
    }
}

extension LocationPublisher: CLLocationManagerDelegate {
    func locationManagerDidChangeAuthorization(_ mgr: CLLocationManager) {
        switch mgr.authorizationStatus {
        case .authorizedWhenInUse, .authorizedAlways:
            isAuthorized = true
            mgr.startUpdatingLocation()
        default:
            isAuthorized = false
            mgr.stopUpdatingLocation()
        }
        DispatchQueue.main.async { self.onUpdate() }
    }

    func locationManager(_ mgr: CLLocationManager,
                         didUpdateLocations locations: [CLLocation]) {
        guard let loc = locations.last else { return }
        lastFix = loc
        publish(loc)
        DispatchQueue.main.async { self.onUpdate() }
    }

    func locationManager(_ mgr: CLLocationManager,
                         didFailWithError error: Error) {
        // Common during indoor / no-signal startup; harmless. Don't
        // spam the console.
        _ = error
    }
}
