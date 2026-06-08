import SwiftUI

@main
struct AisRadarApp: App {
    @StateObject private var model = RadarModel()
    @State private var central: BleCentral?

    var body: some Scene {
        WindowGroup {
            ContentView(model: model)
                .onAppear {
                    if central == nil { central = BleCentral(model: model) }
                }
        }
    }
}
