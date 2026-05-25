// EspBoatApp — app entry point.
//
// SwiftUI @main pattern. Owns the long-lived BusState observable that
// the four tabs all subscribe to; lifecycle hooks bring up the UDP
// listener + heartbeat publisher when we enter the foreground and tear
// them down when we go to the background (v1 is foreground-only;
// background WiFi sockets are killed by iOS aggressively).

import SwiftUI

@main
struct EspBoatApp: App {
    @StateObject private var bus = BusState()

    var body: some Scene {
        WindowGroup {
            RootTabView()
                .environmentObject(bus)
                .onAppear { bus.start() }
                .onDisappear { bus.stop() }
        }
    }
}
