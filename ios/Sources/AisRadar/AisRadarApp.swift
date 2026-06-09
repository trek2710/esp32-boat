import SwiftUI

@main
struct AisRadarApp: App {
    @StateObject private var model = RadarModel()
    @State private var central: BleCentral?
    @State private var location: LocationPublisher?

    var body: some Scene {
        WindowGroup {
            RootView(model: model)
                .onAppear {
                    if central == nil {
                        let c = BleCentral(model: model)
                        let loc = LocationPublisher()
                        loc.onLocation = { [weak c] data in c?.writeHostGps(data) }
                        loc.start()
                        model.onWriteSettings = { [weak c] data in c?.writeSettings(data) }
                        central = c
                        location = loc
                    }
                }
        }
    }
}
