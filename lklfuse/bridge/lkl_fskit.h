/*
 * lkl_fskit.h - Swift-facing C API bridging FSKit to the in-process LKL kernel.
 *
 * Opaque, ObjC/Swift-friendly surface (no LKL/kernel types leak through).
 * Uses LKL POSIX syscalls rather than a host mount or filesystem helper.
 *
 * Three translation units back this header:
 *   - lkl_fskit.c          : mount/probe + item ops over lkl_sys_* (pure C).
 *   - lkl_blk_ops.c        : struct lkl_dev_blk_ops backing virtio-blk by a backend.
 *   - linux_device_fskit.m : block I/O over FSBlockDeviceResource / image file (ObjC).
 *
 * The bridge is inode-keyed (FSKit identity is per-fileID). Looked-up items hold
 * an LKL O_PATH descriptor until reclaim; directory entries also cache paths.
 * The real Linux st_ino is the FSItem identifier (persistent IDs), except the root which is
 * always reported as LKFS_ROOT_INO to match FSItem.Identifier.rootDirectory.
 * Callers serialize operations per volume. All returned errno values use the
 * host's numbering, including ENOATTR for a missing extended attribute.
 */

#ifndef LKL_FSKIT_H
#define LKL_FSKIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Item types — values intentionally match FSKit's FSItemType enum. */
enum {
    LKFS_TYPE_UNKNOWN  = 0,
    LKFS_TYPE_FILE     = 1,
    LKFS_TYPE_DIR      = 2,
    LKFS_TYPE_SYMLINK  = 3,
    LKFS_TYPE_FIFO     = 4,
    LKFS_TYPE_CHARDEV  = 5,
    LKFS_TYPE_BLOCKDEV = 6,
    LKFS_TYPE_SOCKET   = 7,
};

/* Inode reported for the root directory (matches FSItem.Identifier.rootDirectory). */
#define LKFS_ROOT_INO 2ULL
/* Parent reported for the root directory (matches FSItem.Identifier.parentOfRoot). */
#define LKFS_PARENT_OF_ROOT 1ULL

typedef struct lkfs_volume lkfs_volume;

typedef struct {
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t total_files;
    uint64_t free_files;
    uint8_t  read_only;
    char     volume_name[260];   /* UTF-8, NUL-terminated */
    char     fs_type[16];        /* "ext4" / "xfs" / "btrfs" */
    uint8_t  uuid[16];           /* native fs UUID (ext s_uuid / XFS sb_uuid / Btrfs fsid); all-zero if none */
} lkfs_statfs_t;

typedef struct {
    uint64_t ino;
    uint64_t parent_ino;   /* parent dir in our numbering; 0 if unknown */
    uint32_t type;         /* LKFS_TYPE_* */
    uint32_t mode;         /* POSIX permission bits */
    uint32_t nlink;
    uint32_t flags;        /* host BSD flags */
    uint64_t size;
    uint64_t alloc_size;
    int64_t  mtime_sec;  int64_t mtime_nsec;
    int64_t  atime_sec;  int64_t atime_nsec;
    int64_t  ctime_sec;  int64_t ctime_nsec;
    int64_t  btime_sec;  int64_t btime_nsec;
    int64_t  backup_sec; int64_t backup_nsec;
    uint8_t  has_btime;
    uint8_t  has_backup_time;
} lkfs_attr_t;

#define LKFS_XATTR_MAX 65536
enum { LKFS_OPEN_READ = 1, LKFS_OPEN_WRITE = 2 };
enum { LKFS_XATTR_SET = 0, LKFS_XATTR_CREATE = 1, LKFS_XATTR_REPLACE = 2, LKFS_XATTR_DELETE = 3 };
enum { LKFS_PREALLOC_CONTIGUOUS = 2, LKFS_PREALLOC_ALL = 4,
       LKFS_PREALLOC_PERSIST = 8, LKFS_PREALLOC_FROM_EOF = 16 };

/* Directory enumeration callback. Return 0 to continue, non-zero to stop.
 * `cookie` is an opaque resume position for the *next* entry. */
typedef int (*lkfs_dir_cb)(void *ctx, const char *name_utf8,
                           uint64_t ino, uint32_t type, int64_t cookie);

/* --- lifecycle --- */
lkfs_volume *lkfs_mount(void *backend, bool read_only, int *out_errno);
int  lkfs_probe(void *backend, char *name_out, size_t name_cap, char *fstype_out, size_t fstype_cap, uint8_t uuid_out[16]);
/* Returns 0 if the disk was removed (caller must then free the backend); a positive
 * errno otherwise, in which case the disk + backend are still owned (do not free). */
int  lkfs_umount(lkfs_volume *v);
int  lkfs_sync(lkfs_volume *v);
int  lkfs_statfs(lkfs_volume *v, lkfs_statfs_t *out);
int  lkfs_set_volume_name(lkfs_volume *v, const char *name);

/* --- item operations (ino-keyed; root is LKFS_ROOT_INO) --- */
int      lkfs_getattr(lkfs_volume *v, uint64_t ino, lkfs_attr_t *out);
int      lkfs_open_item(lkfs_volume *v, uint64_t ino, unsigned modes);
int      lkfs_close_item(lkfs_volume *v, uint64_t ino, unsigned keeping_modes);
void     lkfs_reclaim_item(lkfs_volume *v, uint64_t ino);
uint64_t lkfs_lookup(lkfs_volume *v, uint64_t dir_ino, const char *name_utf8, int *out_errno);
int      lkfs_readdir(lkfs_volume *v, uint64_t dir_ino, int64_t start_cookie, void *ctx, lkfs_dir_cb cb);
int64_t  lkfs_read(lkfs_volume *v, uint64_t ino, int64_t offset, void *buf, int64_t len, int *out_errno);
int64_t  lkfs_write(lkfs_volume *v, uint64_t ino, int64_t offset, const void *buf, int64_t len, int *out_errno);
uint64_t lkfs_create(lkfs_volume *v, uint64_t dir_ino, const char *name_utf8, uint32_t type, int *out_errno);
uint64_t lkfs_symlink(lkfs_volume *v, uint64_t dir_ino, const char *name_utf8, const char *target, int *out_errno);
int      lkfs_link(lkfs_volume *v, uint64_t target_ino, uint64_t dir_ino, const char *name_utf8, int *out_errno);
int      lkfs_remove(lkfs_volume *v, uint64_t dir_ino, const char *name_utf8);
int      lkfs_truncate(lkfs_volume *v, uint64_t ino, uint64_t size);
int      lkfs_chmod(lkfs_volume *v, uint64_t ino, uint32_t mode);
int      lkfs_set_flags(lkfs_volume *v, uint64_t ino, uint32_t flags);
int      lkfs_set_extra_time(lkfs_volume *v, uint64_t ino, bool birth_time,
                             int64_t seconds, int64_t nanoseconds);
int      lkfs_set_times(lkfs_volume *v, uint64_t ino,
                        int64_t mtime_sec, int64_t mtime_nsec,
                        int64_t atime_sec, int64_t atime_nsec);
int      lkfs_rename(lkfs_volume *v, uint64_t src_dir, const char *src_name,
                     uint64_t dst_dir, const char *dst_name);
int      lkfs_readlink(lkfs_volume *v, uint64_t ino, char *buf, size_t cap);
/* Xattr names are macOS names; the bridge maps them to Linux user.* only.
 * get/list return the byte count or a negative host errno; a NULL buffer queries size.
 * list returns NUL-separated names. Empty values are distinct from deletion. */
int64_t  lkfs_get_xattr(lkfs_volume *v, uint64_t ino, const char *name, void *buffer, size_t size);
int64_t  lkfs_list_xattrs(lkfs_volume *v, uint64_t ino, char *buffer, size_t size);
int      lkfs_set_xattr(lkfs_volume *v, uint64_t ino, const char *name,
                       const void *value, size_t size, unsigned policy);
int64_t  lkfs_preallocate(lkfs_volume *v, uint64_t ino, int64_t offset, int64_t length, unsigned flags);
int64_t  lkfs_seek_region(lkfs_volume *v, uint64_t ino, int64_t offset, bool hole);

/* --- Backend (implemented in linux_device_fskit.m / lkl_file_backend.c) ---
 * Wraps either an FSBlockDeviceResource or an opened image file behind one I/O
 * interface. Create one, pass it to lkfs_probe / lkfs_mount, release after umount. */
void *lkfs_backend_from_block(void *block_resource, int allow_write);  /* __bridge FSBlockDeviceResource* */
void *lkfs_backend_from_file(const char *path, int writable, int *out_err);
void  lkfs_backend_free(void *backend);

int64_t  lkfs_block_pread(void *backend, void *buf, int64_t offset, int64_t count);
int64_t  lkfs_block_pwrite(void *backend, const void *buf, int64_t offset, int64_t count);
int      lkfs_block_sync(void *backend);
uint64_t lkfs_block_total_bytes(void *backend);
uint32_t lkfs_block_sector_size(void *backend);
int      lkfs_block_is_writable(void *backend);

#ifdef __cplusplus
}
#endif

#endif /* LKL_FSKIT_H */
