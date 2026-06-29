# xlinuxfs — Linux filesystems for macOS (FSKit + LKL)

> 📖 Also available in [Simplified Chinese](README_CN.md).

A macOS app + **FSKit file-system extension** that reads and writes **ext2/3/4, XFS, and Btrfs**
volumes using the **real Linux kernel** via [LKL](https://github.com/lkl/linux) (Linux Kernel
Library). No kernel extension, no macFUSE.

The `lklfuse` app-extension embeds the actual in-kernel Linux filesystem drivers (journaling and
the whole ext4 / XFS / Btrfs codebase) — built to a native Mach-O **`liblkl.a`** — behind a thin
FSKit `FSVolume` mapping layer. Each volume runs file operations through the `lkfs_*` C bridge over
`lkl_sys_*` syscalls, so the on-disk handling is the kernel's own, not a reimplementation.

Linux drives **auto-mount under `/Volumes` by the system** (DiskArbitration probes the extension's
registered `FSMediaTypes` — the GPT *Linux filesystem data* type, the MBR `Linux` (0x83) hint, and
partitionless whole disks — with no app process needed, exactly like the built-in exFAT/MSDOS
modules). The `xlinuxfs` app is a **control panel**: it lists Linux drives and attached images,
does manual mount/unmount and read-only remounts, and attaches/detaches disk images. The sandbox
can't run `hdiutil`/`mount`, so those are surfaced as **copyable Terminal commands** where the app
can't act directly.

```
xlinuxfs.app  (SwiftUI, App Sandbox — control panel; not a background agent)
 ├─ AppModel ── DiskArbitrationMonitor   detect/list Linux drives (detection only)
 │           └─ MountService             guided command-line attach / detach / mount
 └─ Contents/Extensions/lklfuse.appex    FSKit module — the in-process Linux engine (+ auto-mount)
        lklfuse.swift            @main UnaryFileSystemExtension
        lklfuseFileSystem.swift  probe (superblock magic) / load / unload
        lklfuseVolume.swift      every FSVolume operation → lkfs_* bridge
        bridge/lkl_fskit.c       lkfs_* over lkl_sys_* (mount, getattr, readdir, read, write, create…)
        bridge/lkl_blk_ops.c     virtio-blk backed by the FSKit backend
        bridge/linux_device_fskit.m  block I/O over FSBlockDeviceResource / image file
        + liblkl.a (arm64; the in-process Linux kernel, statically linked)
```

## Status

| Piece | State |
|-------|-------|
| `lklfuse` extension (ext/XFS/Btrfs engine via LKL) | ✅ builds; engine statically linked; FSKit conformances complete |
| `xlinuxfs` app (UI, monitor, mount service, settings) | ✅ builds |
| English + Simplified Chinese localization | ✅ `Localizable.xcstrings` (en, zh-Hans) |
| App icon | ✅ generated, full AppIcon set |
| End-to-end mount on a Mac | ⛔ requires the FSKit entitlement to be provisioned for your team — see below |

The engine itself is independently proven: the in-process kernel boots and mounts ext2/3/4
read-write, XFS and Btrfs read-only; mount / enumerate / read / write / create / rename / hard +
symbolic links are cross-checked by a CLI harness against real images. Only the OS *loading* of the
extension depends on code-signing / provisioning tied to your Apple Developer account.

## Features

1. **Localization (English + Simplified Chinese)** — `xlinuxfs/Localizable.xcstrings` (String
   Catalog). Add more languages by adding `localizations` entries.
2. **Auto-mount removable Linux media** — handled by the **system**, not the app: the extension's
   `Info.plist` registers `FSMediaTypes` for the GPT *Linux filesystem data* type (`0FC63DAF-…`),
   the MBR `Linux` content hint, and partitionless whole disks, so DiskArbitration auto-mounts a
   recognized ext/XFS/Btrfs volume under `/Volumes` using our module — with no app process running.
   The app only *detects/lists* drives for the UI.
3. **Read-only by scenario** — Settings has two per-scenario defaults shared with the extension
   through an App Group: **disk drives** default read/write, **disk images** default read-only. A
   read-only mount is genuinely write-free (the kernel mounts `MS_RDONLY` with ext `noload` / XFS
   `norecovery`, and the backend is opened `O_RDONLY`).
4. **Multiple drives at once** — devices are tracked by BSD name and mounted independently.
5. **Manual mount / unmount** — per-device *Mount…* and *Eject*. On macOS 27 the app mounts in-app
   via `FSClient`; on macOS 26 a sandboxed app can't mount a third-party FSKit volume, so it shows a
   copyable **`hdiutil mountvol /dev/diskXsY`** command (`diskutil mount` is ineffective for Linux
   filesystems).
6. **Disk images** — macOS can't attach a Linux disk image through Disk Utility, so *Add Disk
   Image…* picks a raw ext/XFS/Btrfs image and shows a copyable **`hdiutil attach [-readonly] <img>`**
   command (read-only by default); once attached, the volume **auto-mounts** like any drive.
   *Detach Image…* shows `hdiutil detach`.

## The engine — a real Linux kernel, in-process

`liblkl.a` is the Linux kernel built as a **native arm64 Mach-O object** via LKL. It boots once
inside the extension; each mounted volume attaches its backing store as a virtio-blk device and
mounts it with the kernel's own ext4 / XFS / Btrfs driver, so journaling and recovery are the real
thing. The kernel → Mach-O macOS port (boot + mount fixes) is tracked in the companion `lkl` tree's
`MACOS_PORT_NOTES.md`.

## Build

Open `xlinuxfs.xcodeproj` and build the `xlinuxfs` scheme. Targets use Xcode **synchronized folder
groups**, so files under `xlinuxfs/` and `lklfuse/` are picked up automatically.

The engine archive `libs/liblkl.a` is **not committed** (it's a build product); rebuild it with
`scripts/build-liblkl.sh`, which assembles it from the LKL tree's macOS build objects (the kernel
object comes from the LKL hybrid build in the companion `lkl` tree). **arm64 only.**

## Provisioning (required to actually run the extension)

The extension declares the **restricted** entitlement `com.apple.developer.fskit.fsmodule`
(`lklfuse/lklfuse.entitlements`). macOS (AMFI) refuses to load it unless that entitlement is
authorized by a provisioning profile — the team's App ID needs the **FSKit File System Module**
capability in the Apple Developer portal, and the app + extension App IDs need the **App Group**
(`<TeamID>.group.com.huanchuan.xlinuxfs`) so the read-only settings are shared with the extension.
Once provisioned, enable the module under **System Settings → General → Login Items & Extensions →
File System Extensions**.

## Sandbox & mounting

Verified on macOS 26.5 with the sandboxed build:

- **Auto-mount is the main path.** The system mounts a recognized Linux volume read/write (or
  read-only per your Settings) under `/Volumes` via the extension's `FSMediaTypes` — no app
  process, no mount entitlement.
- **A sandboxed app can't manually mount a third-party FSKit volume on macOS 26.** Manual mounting
  is in-app only on macOS 27 (`FSClient`, `/Volumes` only); before that it's a copyable
  `hdiutil mountvol` command.
- **`diskutil mount` is ineffective for Linux filesystems**, and `mount -F -t xlinuxfs …` is denied
  by the extension's sandbox — `hdiutil mountvol` (also routed through DiskArbitration) is the
  working CLI.
- **Custom-folder mounting is impossible** for a sandboxed FSKit volume — `/Volumes` only.
- **The app never shells out.** The sandbox blocks `Process`, so `hdiutil` is surfaced as copyable
  Terminal commands for you to run.

## License

`xlinuxfs` embeds the Linux kernel and its ext4 / XFS / Btrfs drivers, which are **GPL-2.0**, so the
project as a whole is distributed under the **GNU General Public License, version 2** (see
[`LICENSE`](LICENSE)).

## Layout

```
xlinuxfs/                   app target (SwiftUI)
  xlinuxfsApp.swift         @main App + Settings scene
  ContentView.swift         device/image list + detail; mount / attach / detach actions
  MountSheet.swift          manual mount (read-only shown, follows Settings)
  CommandSheet.swift        attach/detach sheets (AttachImageSheet: read-only default)
  CopyableCommand.swift     shared "copy this Terminal command" control
  DiagnosticsView.swift     extension-status diagnostics page
  SettingsView.swift        per-scenario read-only preferences
  Model/LinuxDevice.swift
  Services/                 AppModel, AppSettings, DiskArbitrationMonitor, MountService,
                            ExtensionStatus
  Localizable.xcstrings     en + zh-Hans
  Assets.xcassets/AppIcon   generated icon set
lklfuse/                    FSKit extension target
  lklfuse*.swift            @main + FSUnaryFileSystem + FSVolume + FSItem
  bridge/                   lkl_fskit.{h,c}, lkl_blk_ops.c, linux_device_fskit.m, lkl-include/
  Info.plist                FSShortName=xlinuxfs, FSMediaTypes, block resources
  lklfuse.entitlements      com.apple.developer.fskit.fsmodule + App Group + sandbox
libs/liblkl.a               the in-process Linux kernel (built by scripts/build-liblkl.sh)
scripts/build-liblkl.sh     assembles liblkl.a from the LKL macOS build
assets/xlinuxfs-icon.svg    icon source
```
