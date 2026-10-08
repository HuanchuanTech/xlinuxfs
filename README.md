# xlinuxfs — Linux filesystems for macOS (FSKit)

> 📖 Also available in [Simplified Chinese](README_CN.md).

<a href="https://apps.apple.com/app/xlinuxfs/id6785355678"><img src="https://tools.applemediaservices.com/api/badges/download-on-the-app-store/black/en-us?size=250x83" alt="Download xlinuxfs on the App Store" height="44"></a>

A macOS app + **FSKit file-system extension** for mounting **ext2/3/4, XFS, and Btrfs**
volumes — no kernel extension, no macFUSE. Extracted from the
[xntfs](../xntfs) project's reusable FSKit scaffolding, with the NTFS engine (ntfs-3g) removed.

Supports Apple Silicon and Intel, with a minimum deployment target of macOS 15.4.
macOS 27 adds in-app raw image mounting and device mounting with RO/RW selection.
See [platform support and verification](docs/platform-support.md) for build
prerequisites, OS-specific behavior, and tested boundaries.
See [filesystem capabilities](docs/filesystem-capabilities.md) for metadata,
item lifetime, allocation, and their implementation limits.

> **Status: working.** The app (control panel, detection, diagnostics, guided command-line
> attach / detach / mount) and the **FSKit extension** are both implemented: `probeResource`
> identifies ext2/3/4, XFS and Btrfs from the superblock, and core volume operations (read/write,
> create, remove, rename, symlinks, attributes) is served by the real Linux drivers. The bridge is
> validated end-to-end by a CLI harness; OS-level mounting still needs the FSKit module authorized
> by a provisioning profile (and macOS 27 for in-app mounting — see [Build & provisioning](#build--provisioning)).
>
> **Engine: the real Linux kernel via LKL** (Linux Kernel Library), built as a native Mach-O
> `liblkl.a`, so ext2/3/4 + XFS + Btrfs are served by the actual in-kernel drivers (journaled,
> read-write) rather than a reimplementation. The macOS port of LKL (kernel → Mach-O) is tracked in
> `vendor/lkl/tools/lkl/darwin/README.md`; the host side (`lkl_host_ops` + a block backend over
> `FSBlockDeviceResource`, then `lkl_sys_mount`/`lkl_sys_*`) lives in the extension's `bridge/`.

```
xlinuxfs.app  (SwiftUI, App Sandbox — control panel)
 ├─ AppModel ── DiskArbitrationMonitor   detect/list Linux-fs volumes (detection only)
 │           └─ MountService             guided command-line attach / detach / mount
 └─ Contents/Extensions/lklfuse.appex     FSKit module — in-process LKL Linux engine
        lklfuse.swift            @main UnaryFileSystemExtension
        lklfuseFileSystem.swift  probe (superblock magic) / load / unload
        lklfuseVolume.swift      FSVolume handlers over the lkfs_* bridge
        lklfuseItem.swift        FSItem ↔ Linux inode number
        bridge/                  lkfs_* C bridge over the in-process LKL kernel (liblkl.a)
```

## What's here vs. what's TODO

- **App target `xlinuxfs/`** — control panel UI, `DiskArbitrationMonitor` (detects Linux media by
  the Linux-filesystem GPT GUID / content hints), `MountService` (guided command-line attach /
  detach / mount), `ExtensionStatus` diagnostics, settings, localization. Reused largely from xntfs.
- **Extension target `lklfuse/`** — an FSKit `FSUnaryFileSystem` / `FSVolume` / `FSItem`
  implementation over the in-process LKL kernel (`bridge/lkl_fskit.*`, `lkl_blk_ops.c`,
  `linux_device_fskit.m`):
  - `lklfuseFileSystem.probeResource` identifies ext2/3/4 (`0xEF53`), XFS (`XFSB`) and Btrfs
    (`_BHRfS_M`) from the superblock magic, and reports the label + native fs UUID.
  - `lklfuseFileSystem.loadResource` loads metadata read-only; activation then applies
    explicit RO/RW options or the per-scenario App-Group setting.
  - `lklfuseVolume` operations are served by the Linux drivers via `lkl_sys_*`.
- **`Info.plist` `FSMediaTypes`** — Linux-filesystem GPT GUID `0FC63DAF-…-3D69D8477DE4`, a `Linux`
  content hint, and partitionless whole-disk. **Verify the exact DA content hints for ext volumes
  on a real machine** (MBR disks may report `Linux`/`0x83` differently).

## Excluded from the source project

Everything ntfs-3g: the `ntfs-3g` submodule + `libntfs-3g.a`, the C/Obj-C bridge
(`bridge/ntfs_fskit.{c,h}`, `ntfs_device_fskit.m`), the Swift↔C bridging header, the
`build-libntfs.sh` script and `test_bridge.c`. The extension's build settings (`HAVE_CONFIG_H`,
ntfs-3g header search paths, the static-lib link, the Obj-C bridging header) were stripped from
the Xcode project.

## Build & provisioning

Initialize `vendor/lkl` with `git submodule update --init --recursive` and install
upstream LLVM 19 (or set `CLANG` to its executable). Xcode's prebuild phase builds
the universal library and matching headers automatically. See
[dependency and fork workflow](docs/lkl-dependency.md).

Open `xlinuxfs.xcodeproj` and build the `xlinuxfs` scheme. Targets use Xcode **synchronized folder
groups**, so files added under `xlinuxfs/` and `lklfuse/` are picked up automatically.

The extension declares the restricted entitlement `com.apple.developer.fskit.fsmodule`
(`lklfuse/lklfuse.entitlements`); macOS only loads it when that entitlement is authorized by a
provisioning profile (App ID with the **FSKit File System Module** capability for your team).
Enable the module under **System Settings → General → Login Items & Extensions → File System
Extensions**.

## Caveats

- Builds via `xcodebuild` (app + embedded `lklfuse.appex`); open `xlinuxfs.xcodeproj` in Xcode to
  confirm it loads there too. OS-level mounting can't be exercised in-app on macOS < 27 (FSKit
  Mounter is macOS-27-only); the bridge is validated by the CLI harness meanwhile.
- The app icon (`assets/xlinuxfs-icon.svg`, rasterized into `Assets.xcassets/AppIcon.appiconset`) is a Tux-on-a-drive design in the xntfs product family; re-render the sizes from the SVG with `rsvg-convert` if you edit it.
- Bundle IDs: app `com.huanchuan.xlinuxfs`, extension `com.huanchuan.xlinuxfs.lklfuse`; `FSShortName` =
  `xlinuxfs`. Change to your own identifiers as needed.

## License & privacy

This project embeds the Linux kernel and is distributed under the
**GNU General Public License, version 2** (see [LICENSE](LICENSE)).
See the [Privacy Policy](PRIVACY.md) for data handling information.
