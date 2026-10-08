#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
fixtures="${1:?Usage: bash tests/run_volume_capabilities.sh raw-image-directory}"
mkdir -p "$root/_tmp"
work="$(mktemp -d "$root/_tmp/volume-capabilities.XXXXXX")"
trap 'rm -f -- "$work/fixture.img" "$work/test-arm64" "$work/test-x86_64" "$work"/*.o' EXIT
bash "$root/scripts/build-liblkl.sh"
for arch in arm64 x86_64; do
    objects=()
    for source in cli-test/lkl_file_backend.c lklfuse/bridge/lkl_fskit.c lklfuse/bridge/lkl_blk_ops.c; do
        object="$work/$(basename "$source").o"
        xcrun clang -arch "$arch" -mmacosx-version-min=15.4 -O2 -DLKL_HOST_CONFIG_POSIX \
            -I"$root/lklfuse/bridge" -isystem "$root/libs/include" -c "$root/$source" -o "$object"
        objects+=("$object")
    done
    if ! xcrun swiftc -parse-as-library -target "$arch-apple-macos15.4" \
        -import-objc-header "$root/lklfuse/lklfuse-Bridging-Header.h" \
        "$root/lklfuse/lklfuseItem.swift" "$root/lklfuse/lklfuseVolume.swift" \
        "$root/tests/test_volume_capabilities.swift" "${objects[@]}" "$root/libs/liblkl.a" \
        -o "$work/test-$arch" > "$work/build-$arch.log" 2>&1; then
        cat "$work/build-$arch.log"
        exit 1
    fi
    for fs in ext4 xfs btrfs; do
        cp -c "$fixtures/$fs-rawfs.img" "$work/fixture.img"
        for mode in rw ro; do
            before="$(shasum -a 256 "$work/fixture.img")"
            if ! "$work/test-$arch" "$work/fixture.img" "$mode" > "$work/$arch-$fs-$mode.log" 2>&1; then
                tail -40 "$work/$arch-$fs-$mode.log"
                printf 'Logs: %s\n' "$work"
                exit 1
            fi
            if [[ "$mode" == ro ]]; then test "$before" = "$(shasum -a 256 "$work/fixture.img")"; fi
            printf '%s %s: ' "$arch" "$fs"
            tail -1 "$work/$arch-$fs-$mode.log"
        done
        rm "$work/fixture.img"
    done
done
printf 'Logs: %s\n' "$work"
