# Architecture and code structure

xlinuxfs is a macOS host app paired with an FSKit file-system extension. The
host app discovers resources, presents status and diagnostics, and guides the
user through actions that require a Terminal command. The extension owns the
filesystem implementation used by macOS.

## Components

```text
xlinuxfs.app                         SwiftUI control panel and app services
├── AppModel                          device/image state and user preferences
├── DiskArbitrationMonitor            Linux-volume discovery
├── MountService / ImageMountService  attach, mount, eject, and image workflows
└── ExtensionStatus                   installation, enablement, and diagnostics

lklfuse.appex                         FSKit file-system extension
├── lklfuse.swift                      FSUnaryFileSystem entry point
├── lklfuseFileSystem.swift            probe, load, activate, unload
├── lklfuseVolume.swift                FSVolume operations and capabilities
├── lklfuseItem.swift                  FSItem identity and lifecycle
└── bridge/
    ├── lkl_fskit.c / lkl_fskit.h      FSKit-facing filesystem operations
    ├── lkl_blk_ops.c                  block I/O backend
    └── linux_device_fskit.m           FSBlockDeviceResource integration

vendor/lkl                           pinned Linux Kernel Library submodule
scripts/build-liblkl.sh               shared library build entry point
```

## Resource and mount flow

1. `DiskArbitrationMonitor` observes physical media and identifies Linux
   filesystem candidates from the resource metadata and content hints.
2. The app presents devices and disk images in the control panel. Depending on
   the macOS version and sandbox permissions, it either requests a mount or
   provides a copyable `hdiutil` command.
3. FSKit calls `probeResource` before activation. The extension reads the
   filesystem superblock and recognizes ext2/3/4, XFS, or Btrfs without
   modifying the resource.
4. The extension loads the resource through the block backend. Activation
   creates an in-process LKL block device and mounts it with the selected
   read-only or read/write options.
5. `lklfuseVolume` translates FSKit volume and item operations into `lkfs_*`
   bridge calls. The bridge invokes Linux system calls against the LKL mount.
6. Unmount flushes and unmounts the LKL volume before the resource is released.

## Filesystem engine

The extension uses the Linux Kernel Library (LKL) as its filesystem engine. The
Darwin build produces a universal native `liblkl.a`; the selected Linux
ext2/3/4, XFS, and Btrfs drivers handle on-disk structures, journaling, and
filesystem operations. The xlinuxfs bridge supplies the host block device and
maps Linux results to host errno values expected by FSKit.

The bridge also owns the macOS-facing policies for persistent item IDs, open
and close lifetime, native and private extended attributes, timestamps, BSD
flags, volume labels, preallocation, and sparse-region queries. Their exact
support and limitations are documented in [Filesystem capabilities](filesystem-capabilities.md).

## App and extension boundary

The app is sandboxed and does not directly become the filesystem driver. It
shares only the settings and resource workflow needed by the extension. The
FSKit extension is separately signed and must be authorized by a provisioning
profile with the File System Module capability. The extension reads the
per-scenario read-only preference again during activation so a setting change
does not remain stale after resource probing.

## Source ownership

- App UI, resource discovery, mount workflows, diagnostics, and localization:
  `xlinuxfs/`
- FSKit implementation and macOS-facing bridge: `lklfuse/`
- LKL source, Darwin configuration, and library build inputs: `vendor/lkl/`
- Reproducible build and validation entry points: `scripts/` and `tests/`
