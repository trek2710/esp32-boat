import SwiftUI

// Tab shell. One device (ais-radar) today → a Radar tab + a Settings tab.
// Structured so future ESP devices add their own tabs here.
struct RootView: View {
    @ObservedObject var model: RadarModel

    var body: some View {
        TabView {
            RadarTab(model: model)
                .tabItem { Label("Radar", systemImage: "dot.radiowaves.left.and.right") }
            LogTab(model: model)
                .tabItem { Label("Log", systemImage: "list.bullet.rectangle") }
            SettingsTab(model: model)
                .tabItem { Label("Settings", systemImage: "slider.horizontal.3") }
        }
        .preferredColorScheme(.dark)
    }
}
