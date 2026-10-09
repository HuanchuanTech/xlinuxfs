import Foundation

@main
struct PlatformCompatibilityTests {
    @MainActor
    static func main() async throws {
        let fm = FileManager.default
        let parent = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
        let directory = parent.appendingPathComponent("compat-\(UUID())", isDirectory: true)
        let app = directory.appendingPathComponent("Test.app", isDirectory: true)
        let contents = app.appendingPathComponent("Contents/Extensions/lklfuse.appex/Contents", isDirectory: true)
        try fm.createDirectory(at: contents.appendingPathComponent("MacOS"), withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: directory) }
        let executable = contents.appendingPathComponent("MacOS/lklfuse")
        try Data("#!/bin/sh\nexit 0\n".utf8).write(to: executable)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
        func writePlist(identifier: String, executable: String) throws {
            let data = try PropertyListSerialization.data(
                fromPropertyList: ["CFBundleIdentifier": identifier, "CFBundleExecutable": executable],
                format: .xml, options: 0)
            try data.write(to: contents.appendingPathComponent("Info.plist"))
        }
        try writePlist(identifier: ExtensionStatus.bundleID, executable: "lklfuse")
        precondition(ExtensionStatus.embeddedExtension(in: app) != nil)
        let status = ExtensionStatus()
        precondition(status.state == .unknown && status.isEnabled == nil && !status.isInstalled)
        precondition(status.registrationOK == nil)
        let states: [ExtensionStatus.State] = [.unknown, .bundled, .notInstalled, .disabled, .enabled]
        for state in states {
            for dismissed in [false, true] {
                for mounted in [false, true] {
                    let expected = state == .bundled && !dismissed && !mounted
                    precondition(state.shouldShowLegacyGuidance(dismissed: dismissed, hasMountedVolume: mounted) == expected)
                }
            }
        }
        print("PASS legacy guidance: all 20 state/dismissed/mounted combinations")
        if ExtensionStatus.usesBundledExtensionStatus {
            await status.refresh(appURL: app)
            precondition(status.state == .bundled && status.isEnabled == nil && status.registrationOK == nil)
            precondition(status.installedCount == 0 && status.queryError == nil)
            precondition(status.moduleURLs.isEmpty && !status.isInstalled)
            precondition(status.state.shouldShowLegacyGuidance(dismissed: false, hasMountedVolume: false))
            precondition(!status.state.shouldShowLegacyGuidance(dismissed: true, hasMountedVolume: false))
            await status.refresh(appURL: app)
            precondition(status.state == .bundled && status.isEnabled == nil && status.registrationOK == nil)
            precondition(status.moduleURLs.isEmpty && !status.isInstalled)
            precondition(!status.state.shouldShowLegacyGuidance(dismissed: true, hasMountedVolume: false))
            print("PASS macOS 15: bundled presence is separate from unknown enablement/registration")
        } else {
            print("SKIP macOS 15 runtime branch on this OS")
        }
        try writePlist(identifier: ExtensionStatus.bundleID, executable: "../lklfuse")
        precondition(ExtensionStatus.embeddedExtension(in: app) == nil)
        try writePlist(identifier: "com.example.other", executable: "lklfuse")
        precondition(ExtensionStatus.embeddedExtension(in: app) == nil)
        if ExtensionStatus.usesBundledExtensionStatus {
            await status.refresh(appURL: app)
            precondition(status.state == .notInstalled && status.bundledExtensionURL == nil)
            precondition(status.isEnabled == nil && status.registrationOK == nil)
            precondition(!status.state.shouldShowLegacyGuidance(dismissed: true, hasMountedVolume: false))
            print("PASS macOS 15: missing extension remains not installed after guidance dismissal")
        }
        print("PASS embedded extension identity and executable validation")

        let invalid = directory.appendingPathComponent("partitioned.img")
        var mbr = Data(repeating: 0, count: 512)
        mbr[510] = 0x55; mbr[511] = 0xaa
        try mbr.write(to: invalid)
        for url in [directory, invalid] {
            do {
                try ImageMountService.validateImage(url)
                preconditionFailure("Accepted a non-volume image")
            } catch ImageMountError.unsupportedImage { }
        }
        for path in CommandLine.arguments.dropFirst(2) {
            try ImageMountService.validateImage(URL(fileURLWithPath: path))
        }
        precondition(MountService.attachCommand("/tmp/a ' b.img", readOnly: true) == "hdiutil attach -readonly '/tmp/a '\\'' b.img'")
        precondition(!ExtensionStatus.supportsEnableWorkaround || ProcessInfo.processInfo.operatingSystemVersion.majorVersion == 26)
        print("PASS image validation, shell quoting, enable-workaround OS gate")
    }
}
