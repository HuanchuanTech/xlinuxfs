//
//  MountSheet.swift
//  Manual (re)mount of a detected device/image volume. Mounting only ever targets /Volumes
//  (a sandboxed app/extension can't be granted an arbitrary folder; /Volumes is owned by
//  diskarbitrationd). On macOS 27 the app tries Disk Arbitration; older systems and denied
//  physical-disk mounts use a copyable `hdiutil mountvol` command. That command follows
//  the per-scenario Setting, so it is offered only when the requested access mode matches.
//

import SwiftUI

struct MountSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let device: LinuxDevice

    @State private var command: String?
    @State private var busy = false
    @State private var requestedReadOnly = true
    @State private var errorText: String?

    /// Explicit mode for in-app mounts, or the scenario setting on older systems.
    private var readOnly: Bool {
        if #available(macOS 27.0, *) { return requestedReadOnly || !device.mediaWritable }
        return defaultReadOnly
    }

    private var defaultReadOnly: Bool {
        !device.mediaWritable ? true
            : (device.kind == .diskImage ? model.settings.imageReadOnly : model.settings.deviceReadOnly)
    }

    private var copyableCommand: String? {
        guard command != nil else { return nil }
        return MountService.fallbackCommand(for: device, readOnly: readOnly, settingsReadOnly: defaultReadOnly)
    }

    private var accessNote: LocalizedStringKey {
        device.mediaWritable
            ? "Read-only follows your Settings for this kind of volume — change it in Settings before mounting."
            : "This volume is read-only (e.g. an image attached read-only) and can only be mounted read-only."
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Mount \(device.displayName)").font(.title2).bold()
            Text("Mounts under /Volumes.").font(.callout).foregroundStyle(.secondary)

            if #available(macOS 27.0, *) {
                Toggle("Mount read-only", isOn: $requestedReadOnly)
                    .disabled(busy || !device.mediaWritable)
            } else {
                LabeledContent("Access") { Text(readOnly ? "Read-only" : "Read/write") }
                Text(accessNote).font(.caption).foregroundStyle(.secondary)
            }

            if let errorText {
                Text(errorText).font(.callout).foregroundStyle(.red).textSelection(.enabled)
            }

            if command != nil {
                Divider()
                Text("A sandboxed app can't mount this here, so paste this into Terminal:")
                    .font(.callout)
                if #available(macOS 27.0, *) {
                    Text("Terminal mounting follows the read-only setting for this kind of volume in Settings.")
                        .font(.caption).foregroundStyle(.secondary)
                    LabeledContent("Terminal mount access") {
                        Text(defaultReadOnly ? "Read-only" : "Read/write")
                    }
                }
                if let copyableCommand {
                    CopyableCommand(command: copyableCommand)
                } else {
                    Text("The selected access mode does not match Settings. Change the selection or Settings before copying the mount command.")
                        .font(.callout)
                    SettingsLink { Label("Settings…", systemImage: "gearshape") }
                }
            }

            Spacer(minLength: 0)

            HStack {
                Spacer()
                Button("Close") { dismiss() }.disabled(busy)
                Button("Mount") { Task { await doMount() } }
                    .keyboardShortcut(.defaultAction)
                    .disabled(busy)
            }
        }
        .padding(20)
        .frame(width: 560)
        .frame(minHeight: command == nil ? 220 : 360)
        .fixedSize(horizontal: false, vertical: true)
        .interactiveDismissDisabled(busy)
        .onAppear { requestedReadOnly = defaultReadOnly }
    }

    private func doMount() async {
        guard !busy else { return }
        errorText = nil
        command = nil
        busy = true
        defer { busy = false }
        switch await model.mount(device, readOnly: readOnly) {
        case .mounted: dismiss()
        case .needsCommand(let cmd): command = cmd
        case .failed(let message): errorText = message
        }
    }
}
