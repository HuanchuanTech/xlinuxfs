import Foundation
import DiskArbitration

@main
struct MountFallbackTests {
    @MainActor
    static func main() {
        var device = LinuxDevice(id: "disk99s1", volumeName: "Test", sizeBytes: 0,
                                 kind: .removable, contentHint: "Linux", isRemovable: true,
                                 devicePath: "/dev/disk99s1")
        let denied = MountError.dissented(status: DAReturn(kDAReturnNotPrivileged), message: nil)
        let expected = "hdiutil mountvol '/dev/disk99s1'"
        for kind: DeviceKind in [.removable, .fixed] {
            device.kind = kind
            guard case .needsCommand(let command) = MountService.mountFailure(denied, for: device) else {
                fatalError("A denied physical mount must offer a Terminal command")
            }
            precondition(command == expected)
        }
        device.kind = .diskImage
        guard case .failed = MountService.mountFailure(denied, for: device) else {
            fatalError("An image error must not be classified as a physical-disk sandbox denial")
        }
        device.kind = .removable
        let otherErrors: [Error] = [
            MountError.dissented(status: DAReturn(kDAReturnBusy), message: "Busy"),
            MountError.diskNotFound(device.id), MountError.alreadyMounted,
            MountError.extensionUnavailable, MountError.unexpectedMount, MountError.accessModeMismatch,
            POSIXError(.EACCES),
            NSError(domain: "Unrelated", code: Int(kDAReturnNotPrivileged)),
        ]
        for error in otherErrors {
            guard case .failed(let message) = MountService.mountFailure(error, for: device), !message.isEmpty else {
                fatalError("Only a physical DA permission denial may offer a fallback")
            }
        }
        print("PASS physical permission fallback; image, stale-device and other errors remain failures")

        for writable in [true, false] {
            device.mediaWritable = writable
            for setting in [false, true] {
                // Repeated toggles must not expose a command with a mismatching mode.
                for requested in [false, true, false, true] {
                    let command = MountService.fallbackCommand(for: device, readOnly: requested, settingsReadOnly: setting)
                    if !writable || requested == setting {
                        precondition(command == expected)
                    } else {
                        precondition(command == nil)
                    }
                }
            }
        }
        device.mediaWritable = true
        // An open Settings window can also change the mode while the mount sheet is open.
        for setting in [false, true, false] {
            let command = MountService.fallbackCommand(for: device, readOnly: true, settingsReadOnly: setting)
            precondition((command != nil) == setting)
        }
        device.devicePath = "/dev/a ' b"
        precondition(MountService.mountCommand(for: device) == "hdiutil mountvol '/dev/a '\\'' b'")
        print("PASS command mode gating, repeated toggles, Settings changes, read-only media and shell quoting")
    }
}
