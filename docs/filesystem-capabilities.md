# Filesystem capabilities

The Swift FSKit handlers delegate to `lklfuse/bridge/lkl_fskit.c`. All bridge
errors use host errno numbers, not Linux's numerically different constants.
This includes mapping a missing Linux xattr (`ENODATA`) to macOS `ENOATTR`.

## Item lifetime

Lookup and creation pin the inode with an LKL `O_PATH | O_NOFOLLOW` descriptor.
Directory enumeration alone does not pin every entry. An FSItem therefore keeps
its identity after unlink, rename, or replacement by another inode at the same
path. Data descriptors follow FSKit's open/close modes; the identity descriptor
lasts until reclaim. Metadata and subsequent I/O reopen the pinned inode using
LKL's procfs, never the host's `/proc` or a host filesystem mount.

The LKL descriptor limit is raised to 65536 to accommodate FSKit's item cache;
the host process resource limits are unchanged. Reclaim closes descriptors and
removes cached paths. Unmount closes remaining descriptors before flushing and
unmounting the LKL volume. Open-but-unlinked files and overwritten rename
targets remain distinct from newly created files at their former names.

## Extended attributes

`FSVolume.XattrOperations` supplies get, set, list, create-only, replace-only,
and delete operations on macOS 15.4 and later. Binary and empty values are
preserved; an empty value is not a request to delete an attribute.

The mapping is bijective: macOS name `N` corresponds to Linux `user.N`.
For example, `com.apple.FinderInfo` becomes `user.com.apple.FinderInfo`.
Listing strips one `user.` prefix. Names are byte sequences, including names
which are not valid UTF-8. Names may occupy at most 250 bytes before the prefix.
Values are bounded by the Linux 64 KiB API limit; individual filesystems and
image features can impose smaller per-value or aggregate storage limits.
Underlying storage errors are returned rather than truncating attributes.

Linux `security.*`, `trusted.*`, and `system.*` attributes, including POSIX ACLs,
are not exposed through this mapping. Setting a macOS attribute named
`security.selinux` creates `user.security.selinux`, not a security label.
The private namespace described below is also excluded from public xattr
listing and access. Linux user xattrs on symlinks and special files are not
supported by this bridge; requests fail without following a symlink's target.

There is no migration of pre-existing AppleDouble (`._`) sidecars. These tests
do not establish how Finder chooses between native attributes and old sidecars.

## Timestamps and flags

Creation time comes from `statx(STATX_BTIME)` when the filesystem reports it.
If absent, the birth-time attribute is left invalid, not substituted with ctime.
Explicit macOS creation/backup time changes use the following private xattrs:

| Linux name | Encoding |
| --- | --- |
| `user.com.huanchuan.xlinuxfs.birthtime` | 8-byte signed seconds followed by 4-byte nanoseconds, little-endian |
| `user.com.huanchuan.xlinuxfs.backuptime` | Same 12-byte encoding |
| `user.com.huanchuan.xlinuxfs.hidden` | One byte, 1 for hidden; normally removed for visible |

Creation-time overrides take precedence in xlinuxfs, but do not rewrite Linux's
native birth time. Native Linux tools continue to see the original birth time.
Malformed private values are reported as I/O errors rather than misinterpreted.
The whole `user.com.huanchuan.xlinuxfs.*` prefix is reserved.

`UF_IMMUTABLE`, `UF_APPEND`, and `UF_NODUMP` map to Linux inode flags through
`FS_IOC_GETFLAGS/SETFLAGS`, preserving unrelated Linux flags. `UF_HIDDEN` uses
the private attribute above. Other BSD flags return `ENOTSUP`. Linux continues
to enforce its native immutable/append restrictions. Append-only writes must
start at the current EOF and use an append descriptor; other offsets fail.
Changing the append bit invalidates cached data descriptors so clearing the
flag restores ordinary positional writes.

Creation and attribute updates report their consumed fields to FSKit. Mode,
size, access/modify time, creation/backup time, and supported flags are handled.
The existing ignored-ownership policy remains: UID/GID changes and ACL mapping
are not implemented. Added time is not synthesized. Symlink time changes use
no-follow semantics; private time overrides and flags are unsupported on them.

## Volume names

`FSVolume.RenameOperations` uses `FS_IOC_SETFSLABEL`, never raw superblock writes.
Limits are checked in bytes: 16 for ext, 12 for XFS, and 255 for Btrfs. Empty
names and names containing `/` are rejected; overlong names are not truncated.
The volume UUID stays unchanged. Successful changes update the reported name
and persist across unmount/remount.

## Allocation and sparse regions

`FSVolume.PreallocateOperations` uses `fallocate(KEEP_SIZE)` without extending
logical file size. `fromEOF` locates physical EOF through FIEMAP, including
unwritten extents beyond logical EOF; it does not assume `st_blocks` is an offset.

- Persistent allocations remain after close.
- Temporary allocations are supported only beyond logical EOF and when there
  is no pre-existing reserved tail to preserve. Final close/reclaim truncates
  to the current logical size, retaining data written since allocation.
- Mixing a new temporary reservation with a pre-existing persistent tail, or
  temporarily filling holes inside the file, returns `ENOTSUP` before allocating.
- A successful persistent request promotes pending temporary tail reservations
  to persistent reservations.
- `contiguous` and `all` return `ENOTSUP`: Linux fallocate does not guarantee
  physical contiguity or all-or-nothing allocation on failure. No allocation is
  attempted for these flags. Other failures may leave partial reserved space,
  as allowed for a non-`all` operation.

On macOS 27, `FSVolume.SeekRegionHandler` exposes `SEEK_DATA` and `SEEK_HOLE`.
The older deployment target remains 15.4; this conformance is availability
guarded. Normal sparse-file creation remains available on older versions.

## Verification and boundaries

Run against unmounted raw fixture images; the scripts only modify APFS clones:

```bash
bash tests/run_bridge_capabilities.sh /path/to/test-images
bash tests/run_volume_capabilities.sh /path/to/test-images
bash tests/run_bridge_platforms.sh /path/to/test-images
```

The capability runners exercise ext4, XFS, and Btrfs on arm64 and x86_64
(Rosetta on the test host). They cover persistence, unchanged read-only image
hashes, xattr namespaces/policies, timestamp stability, flags, labels, sparse
queries, allocation cleanup, unlink/rename identity, and more than 1024 held
items followed by reclamation. The Swift runner invokes production handlers
directly, including async cross-thread item access and consumed attributes.

Direct handler tests do not mount a newly installed extension through the
system, drive Finder, or prove behavior on a macOS 15/26 runtime. The tests do
not register extensions, alter enablement preferences, or replace `/Applications`.
Check/repair remains a compatibility no-op; formatting remains unsupported.
Kernel-offloaded I/O, macOS 27 cache negotiation, ACL translation, and full
Linux multi-user ownership semantics are outside this change.
