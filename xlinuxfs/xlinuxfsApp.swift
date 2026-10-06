//
//  xlinuxfsApp.swift
//  ext4 for macOS — reads/writes ext4 volumes via ext4 + FSKit.
//

import SwiftUI

@main
struct xlinuxfsApp: App {
    @State private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(model)
                .task { model.start() }
                .frame(minWidth: 720, minHeight: 460)
        }
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About xlinuxfs") {
                    AboutPanel.show()
                }
            }
            CommandGroup(replacing: .help) {
                Link("GitHub", destination: URL(string: "https://github.com/HuanchuanTech/xlinuxfs")!)
                Link("GitHub Issues", destination: URL(string: "https://github.com/HuanchuanTech/xlinuxfs/issues")!)
            }
        }

        Settings {
            SettingsView()
                .environment(model)
        }
    }
}
