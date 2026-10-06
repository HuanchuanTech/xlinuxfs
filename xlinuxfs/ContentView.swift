//
//  ContentView.swift
//  Disk Arbitration devices and path-backed FSKit image mounts share the sidebar.
//

import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.scenePhase) private var scenePhase
    @State private var extStatus = ExtensionStatus()
    @State private var selection: LinuxDevice.ID?
    @State private var showAddImage = false
    @State private var showDiagnostics = false
    @State private var pendingAttach: PendingAttach?
    @State private var showError = false

    var body: some View {
        VStack(spacing: 0) {
            if extStatus.state == .disabled || extStatus.state == .notInstalled {
                ExtensionBanner(status: extStatus, onDiagnostics: { showDiagnostics = true })
            } else if extStatus.state == .bundled {
                HStack(spacing: 12) {
                    Label {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Extension status unavailable on macOS 15")
                                .font(.callout)
                            ExtensionEnablementInstructions()
                        }
                    } icon: {
                        Image(systemName: "info.circle")
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    Button("Open Settings…") { ExtensionStatus.openSettings() }
                    Button("Diagnostics…") { showDiagnostics = true }
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .overlay(alignment: .bottom) { Divider() }
            }
            mainContent
        }
        .task { await extStatus.refresh() }
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didMountNotification)) { _ in
            model.refreshDevices()
        }
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didUnmountNotification)) { _ in
            model.refreshDevices()
        }
        .onChange(of: scenePhase) { _, phase in
            if phase == .active {
                model.refreshDevices()
                Task { await extStatus.refresh() }
            }
        }
    }

    private var physicalDevices: [LinuxDevice] { model.devices.filter { $0.kind != .diskImage } }
    private var imageDevices: [LinuxDevice] { model.devices.filter { $0.kind == .diskImage } }

    private var selectedDevice: LinuxDevice? {
        guard let id = selection else { return nil }
        return model.devices.first { $0.id == id }
    }

    private var mainContent: some View {
        NavigationSplitView {
            List(selection: $selection) {
                Section("Devices") {
                    ForEach(physicalDevices) { device in
                        DeviceRow(device: device, byModule: device.mountedByModule, isSelected: selection == device.id).tag(device.id)
                    }
                }
                Section("Disk Images") {
                    ForEach(imageDevices) { device in
                        DeviceRow(device: device, byModule: device.mountedByModule, isSelected: selection == device.id).tag(device.id)
                    }
                    ForEach(model.mountedImages) { image in
                        HStack(spacing: 10) {
                            Image(systemName: "opticaldiscdrive.fill").font(.title2)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(image.name)
                                Text(image.readOnly ? LocalizedStringKey("Mounted · read-only") : "Mounted")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Text("xlinuxfs").font(.caption2).fontWeight(.semibold)
                        }
                        .padding(.vertical, 2)
                        .tag(image.id)
                    }
                }
            }
            .navigationSplitViewColumnWidth(min: 240, ideal: 280)
            .overlay {
                if model.devices.isEmpty && model.mountedImages.isEmpty {
                    ContentUnavailableView("No Linux volumes",
                                           systemImage: "externaldrive.badge.questionmark",
                                           description: Text("Plug in an ext, XFS, or Btrfs drive, or add a disk image."))
                }
            }
        } detail: {
            if let device = selectedDevice {
                DeviceDetailView(device: device)
            } else if let image = model.mountedImages.first(where: { $0.id == selection }) {
                ImageDetailView(image: image)
            } else {
                ContentUnavailableView("Select a volume", systemImage: "externaldrive")
            }
        }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button { showAddImage = true } label: {
                    Label("Add Disk Image…", systemImage: "plus")
                }
                .help("Add Disk Image…")
                .disabled(model.mountingImage)
            }
            ToolbarItem(placement: .primaryAction) {
                Button { showDiagnostics = true } label: {
                    Label("Diagnostics…", systemImage: "stethoscope")
                }
            }
        }
        .fileImporter(isPresented: $showAddImage, allowedContentTypes: Self.imageTypes) { result in
            switch result {
            case .success(let url): pendingAttach = PendingAttach(url: url)
            case .failure(let error): model.lastError = ImageMountService.diagnosticMessage(error)
            }
        }
        .sheet(isPresented: $showDiagnostics) {
            DiagnosticsView(status: extStatus)
        }
        .sheet(item: $pendingAttach) { p in
            if #available(macOS 27.0, *) {
                ImageMountSheet(source: p.url) { url in selection = "image:" + url.path }
            } else {
                AttachImageSheet(url: p.url)
            }
        }
        .onChange(of: model.lastError) { _, newValue in showError = (newValue != nil) }
        .alert("Operation failed", isPresented: $showError) {
            Button("OK") { model.clearError() }
        } message: {
            Text(model.lastError ?? "")
        }
    }

    private static var imageTypes: [UTType] {
        var types: [UTType] = [.diskImage, .data]
        for ext in ["img", "dd", "raw", "bin", "ext4", "ext3", "ext2", "xfs", "btrfs"] {
            if let u = UTType(filenameExtension: ext) { types.append(u) }
        }
        return types
    }

}

struct ExtensionBanner: View {
    let status: ExtensionStatus
    var onDiagnostics: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
                .font(.title3)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.callout).fontWeight(.semibold)
                ExtensionEnablementInstructions()
                Text("Can't enable it? Open Diagnostics to troubleshoot.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer(minLength: 8)
            Button("Diagnostics…") { onDiagnostics() }
            Button("Open Settings…") { ExtensionStatus.openSettings() }
                .buttonStyle(.borderedProminent)
            Button {
                Task { await status.refresh() }
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(.borderless)
            .help("Re-check")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Color.orange.opacity(0.12))
        .overlay(alignment: .bottom) { Divider() }
    }

    private var title: LocalizedStringKey {
        status.state == .notInstalled ? "Linux file-system extension not installed" : "Linux file-system extension not enabled"
    }
}

struct DeviceRow: View {
    let device: LinuxDevice
    var byModule: Bool = false
    var isSelected: Bool = false

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: icon)
                .font(.title2)
                .foregroundStyle(iconColor)
            VStack(alignment: .leading, spacing: 2) {
                Text(device.displayName).font(.body)
                Text("\(device.sizeBytes.humanSize) · \(Text(statusText))")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if device.state.isMounted {
                Text(byModule ? "xlinuxfs" : "System")
                    .font(.caption2).fontWeight(.semibold)
                    .padding(.horizontal, 6).padding(.vertical, 2)
                    .background(badgeColor.opacity(0.18))
                    .foregroundStyle(badgeColor)
                    .clipShape(Capsule())
            }
        }
        .padding(.vertical, 2)
    }

    // When the row is selected the List paints an accent background, so accent-colored
    // content (the icon, the badge) blends in and vanishes — use white instead.
    private var iconColor: Color {
        if isSelected { return .white }
        return device.state.isMounted ? .accentColor : .secondary
    }
    private var badgeColor: Color {
        if isSelected { return .white }
        return byModule ? .accentColor : .secondary
    }

    private var icon: String {
        switch device.kind {
        case .diskImage: return "opticaldiscdrive.fill"
        case .removable: return "externaldrive.fill"
        case .fixed: return "internaldrive.fill"
        }
    }

    private var statusText: LocalizedStringKey {
        switch device.state {
        case .unmounted: return "Not mounted"
        case .mounting: return "Mounting…"
        case .mounted: return device.readOnly ? "Mounted · read-only" : "Mounted"
        case .unmounting: return "Ejecting…"
        case .failed: return "Failed"
        }
    }
}

struct DeviceDetailView: View {
    @Environment(AppModel.self) private var model
    let device: LinuxDevice
    @State private var showMountSheet = false
    @State private var showDetach = false

    var body: some View {
        Form {
            Section("Volume") {
                LabeledContent("Name", value: device.displayName)
                LabeledContent("BSD device", value: device.devicePath)
                LabeledContent("Capacity", value: device.sizeBytes.humanSize)
                LabeledContent("Format", value: device.formatDisplay)
                if case .mounted(let url) = device.state {
                    LabeledContent("Mounted at", value: url.path)
                    LabeledContent("Access", value: device.readOnly ? "Read-only" : "Read/write")
                }
            }

            Section {
                if device.state.isMounted {
                    Button {
                        Task { await model.unmount(device) }
                    } label: { Label("Eject", systemImage: "eject.fill") }

                    if let url = device.state.mountPoint {
                        Button {
                            NSWorkspace.shared.activateFileViewerSelecting([url])
                        } label: { Label("Reveal in Finder", systemImage: "folder") }
                    }
                } else {
                    Button {
                        showMountSheet = true
                    } label: { Label("Mount…", systemImage: "play.fill") }
                    .disabled(device.state == .mounting || device.state == .unmounting)
                }
                if device.kind == .diskImage {
                    Button(role: .destructive) {
                        showDetach = true
                    } label: { Label("Detach Image…", systemImage: "eject.circle") }
                    .disabled(device.wholeDiskBSD.isEmpty || device.state == .mounting || device.state == .unmounting)
                }
            }

            if case .failed(let message) = device.state {
                Section { Text(message).foregroundStyle(.red) }
            }
        }
        .formStyle(.grouped)
        .navigationTitle(device.displayName)
        .sheet(isPresented: $showMountSheet) {
            MountSheet(device: device).environment(model)
        }
        .sheet(isPresented: $showDetach) {
            CommandSheet(title: "Detach Disk Image",
                         command: MountService.detachCommand(wholeDiskBSD: device.wholeDiskBSD))
        }
    }
}
