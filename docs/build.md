# Build and development

## Requirements

- macOS 15.4 or later
- Xcode 27 or later
- LLVM 19 with a working `clang` executable
- Python 3.9 or later
- A provisioning profile that authorizes the FSKit File System Module capability for signed system-mount tests

The project builds both `arm64` and `x86_64`. The pinned `vendor/lkl`
submodule contains the Darwin build profile and must be initialized before the
first build.

## Initial checkout

```sh
git submodule update --init --recursive
export CLANG=/path/to/llvm19/bin/clang
```

The compiler can also be configured without putting a machine-specific path in
the repository:

```sh
git config --local xlinuxfs.lklClang /path/to/llvm19/bin/clang
```

The local Git setting is ignored by the project and is useful when launching
Xcode from Finder. `LKL_CLANG` or `CLANG` can override it for one command.

## Build the library and app

```sh
bash scripts/build-liblkl.sh
xcodebuild -project xlinuxfs.xcodeproj -scheme xlinuxfs \
  -configuration Release ONLY_ACTIVE_ARCH=NO build
```

The Xcode prebuild phase invokes the same library script automatically. Build
outputs are written to ignored paths under `libs/`; temporary work and logs
belong under `_tmp/`.

For a signed archive, use a locally authorized signing identity and profile:

```sh
xcodebuild -project xlinuxfs.xcodeproj -scheme xlinuxfs \
  -configuration Release \
  -archivePath _tmp/xlinuxfs.xcarchive \
  -destination 'generic/platform=macOS' \
  ONLY_ACTIVE_ARCH=NO archive
```

The FSKit entitlement is restricted. A successful unsigned build does not prove
that macOS will load or mount the extension; verify the signed embedded app and
extension before system-mount testing.

## Validation

Run the bridge and direct FSKit capability checks against disposable fixture
images. The runners cover ext4, XFS, and Btrfs; they should not be pointed at a
mounted production volume.

```sh
python3 vendor/lkl/tools/lkl/darwin/test_profile.py
bash tests/run_bridge_platforms.sh /path/to/test-images
bash tests/run_bridge_capabilities.sh /path/to/test-images
bash tests/run_volume_capabilities.sh /path/to/test-images
```

For signed macOS 27 integration tests, see
[Platform support and verification](platform-support.md). Those tests require
an installed, signed app, an enabled extension, and disposable copies of the
fixtures.

## Dependency updates

Make LKL engine changes inside `vendor/lkl`, commit them in that submodule, and
run the bridge and capability test matrix before updating the parent gitlink.
The full ownership, fork, cache, and update workflow is documented in
[LKL dependency workflow](lkl-dependency.md).

## Development notes

- Do not commit generated `libs/` outputs or temporary `_tmp/` artifacts.
- Keep signing identities, provisioning data, local paths, and diagnostic logs
  out of public documentation and commits.
- Test read-only behavior separately from read/write behavior. A read-only
  resource must never become writable because of a requested mount mode.
- The filesystem capability document records supported metadata operations and
  intentional `ENOTSUP` boundaries.
