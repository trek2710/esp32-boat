// RootTabView — the four-tab shell from docs/IOS_APP.md.
//
// Each tab is wrapped in a NavigationStack so detail views (e.g. AIS
// target detail) can push without leaving the tab.

import SwiftUI

struct RootTabView: View {
    var body: some View {
        TabView {
            NavigationStack { BoatTab() }
                .tabItem { Label("Boat", systemImage: "sailboat") }

            NavigationStack { AisTab() }
                .tabItem { Label("AIS", systemImage: "dot.radiowaves.left.and.right") }

            NavigationStack { SettingsTab() }
                .tabItem { Label("Settings", systemImage: "slider.horizontal.3") }

            NavigationStack { DiagnosticsTab() }
                .tabItem { Label("Diag", systemImage: "stethoscope") }
        }
    }
}
