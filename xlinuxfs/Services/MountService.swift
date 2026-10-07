//
//  MountService.swift
//  Mounts / unmounts Linux device volumes via DiskArbitration, and builds the copyable
//  Terminal commands the sandbox can't run itself: `hdiutil mountvol` (on macOS < 27 or when
//  the in-app mount is refused) and `hdiutil attach` / `detach` for disk images — macOS
//  can't attach a Linux image through Disk Utility, so attaching is a guided command.
//

import Foundation
import DiskArbitration
import FSKit
import IOKit

enum MountError: LocalizedError {
    case daUnavailable
    case diskNotFound(String)
    case dissented(status: DAReturn, message: String?)
    case cannotCreateMountPoint(String)
    case alreadyMounted
    case extensionUnavailable
    case unexpectedMount
    case accessModeMismatch

    var errorDescription: String? {
        switch self {
        case .daUnavailable: return "DiskArbitration is unavailable."
        case .diskNotFound(let b): return "Device \(b) was not found."
        case .dissented(let status, let message): return "Mount was refused: \(message ?? "status \(status)")"
        case .cannotCreateMountPoint(let p): return "Couldn't create the mount folder at \(p)."
        case .alreadyMounted:
            return String(localized: "This volume is already mounted. Eject it before changing its access mode.")
        case .extensionUnavailable:
            return String(localized: "Enable lklfuse in File System Extensions before mounting this volume.")
        case .unexpectedMount:
            return String(localized: "The system did not return an xlinuxfs mount. Refresh the volume list and check Diagnostics.")
        case .accessModeMismatch:
            return String(localized: "The volume mounted with a different access mode than requested. Check its current status before using it.")
        }
    }

    /// kDAReturnNotPrivileged — the sandbox isn't allowed to perform this mount, so the
    /// only path is a user-run Terminal command (every other failure is a real error).
    var isNotPrivileged: Bool {
        if case .dissented(let status, _) = self { return status == kDAReturnNotPrivileged }
        return false
    }
}

/// Result of a mount attempt.
enum MountOutcome {
    case mounted(URL)
    case needsCommand(String)
    case failed(String)
}

@MainActor
final class MountService {
    private let monitor: DiskArbitrationMonitor
    init(monitor: DiskArbitrationMonitor) { self.monitor = monitor }

    // MARK: mount
    //
    // Physical device mounts can still be denied by App Sandbox on macOS 27.
    // System auto-mounts remain independent of this manual entry point.

    func unifiedMount(_ device: LinuxDevice, readOnly: Bool) async -> MountOutcome {
        let command = Self.mountCommand(for: device)
        guard #available(macOS 27.0, *) else { return .needsCommand(command) }
        do {
            let modules = try await FSClient.shared.installedExtensions
            guard modules.contains(where: { $0.bundleIdentifier == ExtensionStatus.bundleID && $0.isEnabled }) else {
                throw MountError.extensionUnavailable
            }
            return .mounted(try await mount(device, readOnly: readOnly))
        }
        catch {
            return Self.mountFailure(error, for: device)
        }
    }

    static func mountFailure(_ error: Error, for device: LinuxDevice) -> MountOutcome {
        if let mountError = error as? MountError, mountError.isNotPrivileged,
           device.kind != .diskImage {
            return .needsCommand(mountCommand(for: device))
        }
        return .failed(ImageMountService.diagnosticMessage(error))
    }

    private func checkedDisk(_ device: LinuxDevice) throws -> DADisk {
        let session = monitor.sessionRef()
        guard let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, device.id) else {
            throw MountError.diskNotFound(device.id)
        }
        if let expectedID = device.registryEntryID {
            let media = DADiskCopyIOMedia(disk)
            guard media != IO_OBJECT_NULL else { throw MountError.diskNotFound(device.id) }
            defer { IOObjectRelease(media) }
            var actualID: UInt64 = 0
            guard IORegistryEntryGetRegistryEntryID(media, &actualID) == KERN_SUCCESS,
                  actualID == expectedID else { throw MountError.diskNotFound(device.id) }
        }
        return disk
    }

    private func mount(_ device: LinuxDevice, readOnly: Bool) async throws -> URL {
        let disk = try checkedDisk(device)
        guard let description = DADiskCopyDescription(disk) as? [String: Any] else {
            throw MountError.diskNotFound(device.id)
        }
        guard description[kDADiskDescriptionVolumePathKey as String] == nil else { throw MountError.alreadyMounted }
        let mediaWritable = description[kDADiskDescriptionMediaWritableKey as String] as? Bool ?? device.mediaWritable
        let effectiveReadOnly = readOnly || !mediaWritable
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            // Pass an explicit ro/rw so the extension can tell a deliberate in-app mount
            // from a system auto-mount (which carries no ro/rw and honors the app setting).
            let args = effectiveReadOnly ? ["rdonly"] : ["rw"]
            withMountArguments(args) { argv in
                let box = DACallbackBox { dissenter in
                    if let dissenter {
                        let status = DADissenterGetStatus(dissenter)
                        let msg = DADissenterGetStatusString(dissenter).map { $0 as String }
                        cont.resume(throwing: MountError.dissented(status: status, message: msg))
                    } else {
                        cont.resume(returning: ())
                    }
                }
                DADiskMountWithArguments(disk, nil, DADiskMountOptions(kDADiskMountOptionDefault),
                                         { _, dissenter, ctx in daInvokeBox(dissenter, ctx) },
                                         Unmanaged.passRetained(box).toOpaque(), argv)
            }
        }
        guard let desc = DADiskCopyDescription(disk) as? [String: Any],
              let url = desc[kDADiskDescriptionVolumePathKey as String] as? URL,
              let info = DiskArbitrationMonitor.mountInfo(url),
              info.fsType == DiskArbitrationMonitor.moduleFSType else {
            throw MountError.unexpectedMount
        }
        guard info.readOnly == effectiveReadOnly else { throw MountError.accessModeMismatch }
        return url
    }

    func unmount(_ device: LinuxDevice, force: Bool = false) async throws {
        let disk = try checkedDisk(device)
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            let box = DACallbackBox { dissenter in
                if let dissenter { cont.resume(throwing: MountError.dissented(status: DADissenterGetStatus(dissenter), message: nil)) }
                else { cont.resume(returning: ()) }
            }
            let opts: DADiskUnmountOptions = force ? DADiskUnmountOptions(kDADiskUnmountOptionForce) : DADiskUnmountOptions(kDADiskUnmountOptionDefault)
            DADiskUnmount(disk, opts, { _, dissenter, ctx in daInvokeBox(dissenter, ctx) },
                          Unmanaged.passRetained(box).toOpaque())
        }
    }

    // MARK: copyable commands

    /// `hdiutil mountvol` mounts a detected-but-unmounted volume via Disk Arbitration. For a
    /// third-party FSKit (Linux) volume this is the path that actually works: `diskutil mount`
    /// was tested ineffective for Linux filesystems, and `mount -F -t xlinuxfs` is denied by
    /// the extension's sandbox. `mountvol` has no read-only option, so the mount's read/write
    /// mode follows the attached media's writability + the app's per-scenario read-only Setting,
    /// which the extension applies on the resulting DA mount (same as system auto-mount).
    static func mountCommand(for device: LinuxDevice) -> String {
        "hdiutil mountvol \(shellQuote(device.devicePath))"
    }

    /// mountvol cannot pass ro/rw. Do not offer a command that would ignore the
    /// user's requested mode; changing either selection or Settings re-evaluates this.
    static func fallbackCommand(for device: LinuxDevice, readOnly: Bool, settingsReadOnly: Bool) -> String? {
        let mediaReadOnly = !device.mediaWritable
        guard (readOnly || mediaReadOnly) == (settingsReadOnly || mediaReadOnly) else { return nil }
        return mountCommand(for: device)
    }

    /// Attach a disk image as a device — the extension then auto-mounts any ext/XFS/Btrfs
    /// volume on it. Read-only by default (the attached media's writability flows through to
    /// the mount); pass readOnly: false to attach writable.
    static func attachCommand(_ imagePath: String, readOnly: Bool) -> String {
        let ro = readOnly ? " -readonly" : ""
        return "hdiutil attach\(ro) \(shellQuote(imagePath))"
    }

    /// Detach an attached image by its whole-disk node (e.g. "disk6").
    static func detachCommand(wholeDiskBSD: String) -> String {
        "hdiutil detach /dev/\(wholeDiskBSD)"
    }

    private static func shellQuote(_ path: String) -> String {
        "'" + path.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}

// MARK: - DiskArbitration callback bridging

final class DACallbackBox {
    let handler: (DADissenter?) -> Void
    init(_ handler: @escaping (DADissenter?) -> Void) { self.handler = handler }
}

/// Shared body for the inline DiskArbitration mount/unmount completion closures.
private func daInvokeBox(_ dissenter: DADissenter?, _ ctx: UnsafeMutableRawPointer?) {
    guard let ctx else { return }
    let box = Unmanaged<DACallbackBox>.fromOpaque(ctx).takeRetainedValue()
    box.handler(dissenter)
}

/// Build a NULL-terminated C array of CFString mount arguments and pass it to `body`.
private func withMountArguments<T>(_ args: [String], _ body: (UnsafeMutablePointer<Unmanaged<CFString>?>?) -> T) -> T {
    if args.isEmpty { return body(nil) }
    let cfs = args.map { $0 as CFString }
    let buf = UnsafeMutablePointer<Unmanaged<CFString>?>.allocate(capacity: cfs.count + 1)
    defer { buf.deallocate() }
    for (i, s) in cfs.enumerated() { buf[i] = Unmanaged.passUnretained(s) }
    buf[cfs.count] = nil
    return body(buf)
}
