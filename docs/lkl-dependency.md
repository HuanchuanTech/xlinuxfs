# LKL dependency workflow

## Ownership

- `vendor/lkl`: pinned Git submodule from `https://github.com/HuanchuanTech/lkl`.
  Owns Darwin kernel/host changes and `tools/lkl/darwin/` build inputs.
- `scripts/build-liblkl.sh`: shared entry point for Xcode, CLI and bridge tests.
- `scripts/prebuild-lkl.py`: serializes builds and checks the local build cache.
- `libs/liblkl.a`, `libs/include/`, `libs/lkl-build.json`: ignored outputs.
- `lklfuse/bridge/`: FSKit integration, owned by this application.

## Initial fork publication

The changes are committed on `darwin-fskit` in the original sibling checkout.
The individual port/build commits retain their original author identities;
local paths in the bring-up notes use generic examples. The submodule pins
these commits and its recorded remote points to the fork. After creating it:

```sh
git -C ../lkl push https://github.com/HuanchuanTech/lkl darwin-fskit
```

Push the LKL commits before publishing the parent submodule pointer. Until then,
new machines cannot fetch the pinned commit from GitHub. No push is performed
by the build scripts.

## Build

```sh
git submodule update --init --recursive
git config --local xlinuxfs.lklClang /path/to/llvm19/bin/clang
bash scripts/build-liblkl.sh
xcodebuild -project xlinuxfs.xcodeproj -scheme xlinuxfs \
  -configuration Release ONLY_ACTIVE_ARCH=NO build
```

Requires upstream LLVM 19, Python 3.9+, and Xcode. The wrapper also searches
`clang-19` on PATH and the standard Homebrew `llvm@19` locations. The local Git
setting above also works when opening Xcode from Finder; it is not committed.
`LKL_CLANG` (or `CLANG`) can override it for a single invocation.
The extension's prebuild phase invokes the same script; script sandboxing is
disabled for that target so it can read the submodule and execute LLVM.

## Update

Make and commit engine changes inside `vendor/lkl`, build, then run:

```sh
bash tests/run_bridge_platforms.sh /path/to/test-images
git -C vendor/lkl push origin HEAD:darwin-fskit
git add vendor/lkl
```

Commit the updated gitlink together with any required bridge changes. Normal
builds use the pinned commit, never `git submodule update --remote`. Configuration
changes require reviewing the whole fixed profile, as documented in LKL.

## Case-sensitive paths

Linux contains case-only filename pairs in netfilter and litmus tests. On the
default case-insensitive APFS, Git may show these paths as modified immediately
after clone. They are not port changes and are not used by this filesystem
profile. Do not stage them. Use case-sensitive storage for general Linux
development; this fixed Darwin build is tested on ordinary APFS.

## Filesystem-only profile

The Darwin profile now compiles 714 kernel units instead of 1047. Kconfig
disables networking, FAT/VFAT and NLS, PCI/NVMe, input/HID/serio and TTY;
the object list and both kernel/public headers are regenerated together.
It retains ext2/3/4, XFS, Btrfs, their existing ACL/xattr and compression/checksum
support, virtio block I/O, procfs and sysfs. Both architectures and `-O2` remain
unchanged. Tracing is retained because the LKL architecture selects it.

The exporter no longer requires historical `.o.macho` files to choose objects.
Its explicit Darwin replacements and scheduler ordering are documented in
`vendor/lkl/tools/lkl/darwin/README.md`. Run the profile checks before building:

```sh
python3 vendor/lkl/tools/lkl/darwin/test_profile.py
bash scripts/build-liblkl.sh
bash tests/run_bridge_platforms.sh /path/to/test-images
bash tests/run_bridge_capabilities.sh /path/to/test-images
bash tests/run_volume_capabilities.sh /path/to/test-images
```

The host send/recv convenience wrappers use the syscall dispatcher directly,
so the host headers also compile without generated socket syscall helpers;
the filesystem-only kernel returns `ENOSYS` for networking operations.

## Validation (2026-09-28)

The submodule was cloned from committed Git objects, without the original
checkout's ignored build artifacts. Both architectures compiled from all 1047
profile units. Bridge tests passed for arm64 and x86_64 (Rosetta), each with
ext4, XFS and Btrfs: unchanged image hash under read-only access, write/rename/
readback, and persistence after a fresh read-only mount. Xcode Release built
the app and extension as universal binaries using the new prebuild phase and
exported headers (`CODE_SIGNING_ALLOWED=NO`). No installation was performed.
Repeated CLI and Xcode builds both reused the verified library/header cache;
SDK symlink and versioned paths resolve to the same cache identity.

Local verification logs are `_tmp/lkl-submodule-tests.log` and
`_tmp/lkl-submodule-xcode.log`. The original measurement-only ELF format
override was removed; the pre-cleanup diff is kept locally at
`_tmp/lkl-local-before-cleanup.patch`. Original compiled bring-up artifacts
remain in the sibling checkout and are not needed for this build.
