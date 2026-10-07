#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
fixtures="${1:?Usage: bash tests/run_system_mount.sh image-directory}"
mkdir -p "$root/_tmp"
work="$(mktemp -d "$root/_tmp/system-mount.XXXXXX")"
whole=""
cleanup() {
    if [[ -n "$whole" ]]; then hdiutil detach "$whole" || return; fi
    if [[ -f "$work/fixture.img" ]]; then rm "$work/fixture.img"; fi
}
trap cleanup EXIT
xcrun swiftc "$root/tests/attached_devices.swift" -o "$work/attached-devices"
xcrun swiftc "$root/tests/verify_system_mount.swift" -o "$work/verify"
for fs in ext4 xfs btrfs; do
    cp -c "$fixtures/$fs-mbr.img" "$work/fixture.img"
    before="$(shasum -a 256 "$work/fixture.img")"
    hdiutil attach -nomount -readonly -plist "$work/fixture.img" > "$work/$fs-attach.plist"
    devices="$("$work/attached-devices" "$work/$fs-attach.plist")"
    whole="${devices%%$'\n'*}"
    partition="${devices##*$'\n'}"
    hdiutil mountvol "$partition"
    diskutil info -plist "$partition" > "$work/$fs-ro.plist"
    "$work/verify" "$work/$fs-ro.plist"
    hdiutil detach "$whole"
    whole=""
    test "$before" = "$(shasum -a 256 "$work/fixture.img")"
    rm "$work/fixture.img"
    printf 'PASS %s: mountvol, expected driver/mode, unchanged RO image hash\n' "$fs"
done
printf 'Logs: %s\n' "$work"
