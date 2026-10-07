# Platform support

The deployment target is macOS **15.4**, the first supported FSKit release.
The app, extension, and `liblkl.a` contain both `arm64` and `x86_64` slices.
Build with Xcode 27 or newer; newer APIs remain guarded at runtime.

| System | Extension status | Disk/image mounting |
| --- | --- | --- |
| macOS 15.4+ | Validate the embedded extension; registration and enablement remain unknown in-app | Existing Disk Arbitration auto-mount; attach images with the displayed Terminal command |
| macOS 26 | Query FSClient; keep the optional enablement workaround only on 26 | Existing auto-mount and Terminal image attachment |
| macOS 27+ | Query FSClient; open the official File System Extensions settings pane | In-app raw image mounting and explicit device mounting, with RO/RW selection |

Linux volumes keep their existing `FSMediaTypes` matching. There is no NTFS
driver override, temporary `/Library/Filesystems` routing entry, privileged
helper, or macOS 15 remount script. The macOS 15 auto-mount expectation still
needs a run on that OS; the macOS 27 tests are not proof of macOS 15 behavior.

Direct image mounting accepts raw single-volume ext2/3/4, XFS, and Btrfs images.
Partitioned whole-disk images and compressed containers retain the Terminal
attachment flow. Direct mounts are discovered from the mount table after app
restart, including their actual read-only flag. Device mounts use public Disk
Arbitration and check the I/O Registry connection identity, actual driver, and
actual access mode before reporting success.

Physical-disk mounts can still be denied by App Sandbox on macOS 27
(`kDAReturnNotPrivileged`). The app then offers `hdiutil mountvol`, while image
permission errors and other failures remain errors. Unlike xntfs, this app does
not offer `diskutil mount`: on the test host it still rejected an ext4 partition
that `hdiutil mountvol` could mount through xlinuxfs.

`mountvol` cannot pass explicit RO/RW options. The sheet shows the scenario
setting used by a Terminal mount and withholds the command if it differs from
the selected mode. Changing the selection or Settings re-evaluates this check;
read-only media stays read-only. The extension reads the scenario preference
again at activation so a setting changed after resource loading is not stale.

The host app needs `com.apple.developer.fskit.mount`, user-selected read/write
access, and app-scoped bookmarks. The extension retains
`com.apple.developer.fskit.fsmodule`. Signing profiles must authorize the
corresponding capabilities.

## Shared extension lifecycle

Probe/load are read-only. Activation resolves explicit RO/RW options before
falling back to the shared image/device preference. A read-only resource never
becomes writable. Read-only mounts skip journal/log recovery: ext uses `noload`,
XFS uses `norecovery`, and Btrfs uses `nologreplay`. See the
[Btrfs mount-option documentation](https://btrfs.readthedocs.io/en/stable/Administration.html).

LKL completes its clean unmount in `FSVolume.unmount`, before FSKit deactivation
can revoke resource I/O. Waiting until `unloadResource` caused an observed XFS
regression: a successful write/rename/readback disappeared from the subsequent
read-only view. That test now passes. The backend stays retained until unload,
and a loaded volume can reopen its LKL mount. Failed unmounts retain the live
handle/backend for retry.

## Building the library

```bash
git submodule update --init --recursive
export CLANG=/path/to/llvm19/bin/clang
bash scripts/build-liblkl.sh
xcodebuild -project xlinuxfs.xcodeproj -scheme xlinuxfs \
  -configuration Release ONLY_ACTIVE_ARCH=NO build
```

The pinned `vendor/lkl` submodule owns the Darwin port and the build recipe.
Its `tools/lkl/darwin/build.py` compiles both architectures from source using
the versioned fixed-profile configuration, per-file flags, ordered source list,
and generated headers. No sibling checkout or previously compiled object is
required. See [LKL dependency workflow](lkl-dependency.md).

Xcode runs `scripts/build-liblkl.sh` before compiling the extension. The same
entry point is used by the bridge tests. It emits `libs/liblkl.a` and matching
`libs/include/` headers; both are ignored by Git. The old separately vendored
headers and architecture-specific parent build script are no longer needed.
The cache checks the LKL commit and tracked changes, build wrapper, compiler,
SDK/linker identity, archive digest, and public headers. `--force` rebuilds.
Build work and logs live under `_tmp/lkl-build/`; objects are removed on success.

## Verification

Test host: macOS 27.0 (26A428), Xcode 27.0 (27A266a), Apple Silicon.

- Signed universal Release build and strict app/extension signature validation.
- arm64 and x86_64 (Rosetta) bridge tests on ext4, XFS, and Btrfs: read-only hash
  preservation, denied writes, create/rename/readback, clean unmount, and
  persistence across a fresh read-only mount.
- Sandboxed production `AppModel`/services against `/Applications/xlinuxfs.app`:
  three raw formats, actual `statfs` type/mode, host-side writes, persisted data,
  duplicate/symlink rejection, model rediscovery, and unmount cleanup.
- MBR image partitions for all three formats pass the device-mount workflow,
  including stale connection rejection, RO hashes, RW persistence, and read-only
  media that stays read-only even when the app requests RW.
- System auto-mount after `hdiutil attach -readonly` passes for all three MBR
  images without an app mount call or filesystem-routing workaround. Directory
  reads, actual `xlinuxfs` mount type, RO flags, and unchanged hashes are checked.
- Compatibility tests build for `x86_64-apple-macos15.4`: embedded-extension
  validation, raw-image validation, shell quoting, and OS gating. The actual
  macOS 15 status-query branch is explicitly skipped on macOS 27.

No physical Intel Mac, macOS 15/26 runtime, USB media, or automated SwiftUI
interaction is covered by these results. Kernel code is not audited by this
change.

```bash
bash tests/run_bridge_platforms.sh /path/to/test-images
bash tests/run_system_mount.sh /path/to/test-images
bash tests/run_automount.sh /path/to/test-images
bash tests/run_macos27_images.sh /Applications/xlinuxfs.app \
  'Apple Development: your identity' /path/to/ext4-rawfs.img
bash tests/run_macos27_images.sh /Applications/xlinuxfs.app \
  'Apple Development: your identity' /path/to/ext4-mbr.img device
bash tests/run_macos27_images.sh /Applications/xlinuxfs.app \
  'Apple Development: your identity' /path/to/ext4-mbr.img device-ro
xcrun swiftc -parse-as-library -target x86_64-apple-macos15.4 \
  tests/test_platform_compat.swift xlinuxfs/Services/*.swift \
  xlinuxfs/Model/LinuxDevice.swift -o _tmp/test-platform-compat-x86_64
_tmp/test-platform-compat-x86_64 _tmp /path/to/ext4-rawfs.img
xcrun swiftc -parse-as-library -target x86_64-apple-macos15.4 \
  tests/test_mount_fallback.swift xlinuxfs/Services/*.swift \
  xlinuxfs/Model/LinuxDevice.swift -o _tmp/test-mount-fallback-x86_64
_tmp/test-mount-fallback-x86_64
```

The bridge runner expects `ext4-rawfs.img`, `xfs-rawfs.img`, and `btrfs-rawfs.img`
under the fixture directory. Integration runners use disposable copies; source
images must not already be mounted because copies preserve filesystem UUIDs.
Logs remain under `_tmp`. The app UI retains the user's extension enablement and
scenario preferences; the tests do not change them.

The system-mount test uses disposable, read-only MBR images and verifies the
actual driver, access mode, directory access, and unchanged image hashes for
ext4, XFS, and Btrfs. The unit test covers physical-disk permission error mapping,
non-permission errors, repeated access-mode changes, and read-only media. These
tests do not reproduce the permission denial on a physical USB disk.
