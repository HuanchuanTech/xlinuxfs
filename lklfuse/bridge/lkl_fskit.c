/*
 * lkl_fskit.c - the lkfs_* FSKit bridge implemented over the in-process LKL
 * kernel (POSIX syscalls). FSItems pin inodes independently of directory names;
 * a path cache also serves entries discovered by readdir(). The real Linux
 * st_ino is the reported FSItem id, except the root which is LKFS_ROOT_INO.
 *
 * The LKL kernel is a process-global singleton: started once, then each volume
 * adds its own virtio-blk disk (backed by an lkfs backend) and mounts it under
 * /mnt/<id>. Filesystem type is detected from the superblock magic so neither
 * probe nor mount needs to guess.
 */

#include <lkl.h>
#include <lkl_host.h>
#include "lkl_fskit.h"
#include "lkl_errno.h"
#include "lkl_fiemap.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

extern struct lkl_dev_blk_ops lkl_fskit_blk_ops;   /* lkl_blk_ops.c */

/* ------------------------------------------------------------------ */
/* Process-global LKL kernel (started once).                           */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_started;
static int g_start_error;

static int ensure_kernel(void)
{
    int ret = 0;
    pthread_mutex_lock(&g_lock);
    if (!g_started) {
        if (lkl_init(&lkl_host_ops) < 0) { ret = -ENOMEM; goto out; }
        ret = lkl_start_kernel("mem=64M");
        if (ret) { ret = -EIO; goto out; }
        g_started = 1;
        lkl_sys_mkdir("/proc", 0555);
        long rc = lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
        if (rc && rc != -LKL_EBUSY) g_start_error = (int)lkfs_host_result(rc);
        /* FSKit caches more items than Linux's default 1024-descriptor limit.
         * These are LKL descriptors, not host process descriptors. */
        struct lkl_rlimit limit;
        if (!g_start_error) {
            rc = lkl_sys_getrlimit(LKL_RLIMIT_NOFILE, &limit);
            if (!rc && limit.rlim_cur < 65536) {
                limit.rlim_cur = 65536;
                if (limit.rlim_max < limit.rlim_cur) limit.rlim_max = limit.rlim_cur;
                rc = lkl_sys_setrlimit(LKL_RLIMIT_NOFILE, &limit);
            }
            g_start_error = (int)lkfs_host_result(rc);
        }
    }
    ret = g_start_error;
out:
    pthread_mutex_unlock(&g_lock);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Superblock probe (no LKL needed) — fstype + label from magic.       */
/* ------------------------------------------------------------------ */
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Fills fstype ("ext4"/"xfs"/"btrfs"), label, and the 16-byte native fs UUID
 * (uuid_out may be NULL); returns 1 if recognized. */
static int detect_fs(void *backend, char *fstype, size_t fcap, char *label, size_t lcap, uint8_t *uuid_out)
{
    uint8_t buf[4096];
    if (label && lcap) label[0] = 0;
    if (uuid_out) memset(uuid_out, 0, 16);

    /* ext2/3/4: magic 0xEF53 at byte 1024+56; label at 1024+120 (16 bytes); s_uuid at 1024+0x68. */
    if (lkfs_block_pread(backend, buf, 1024, 256) == 256 && rd_le16(buf + 56) == 0xEF53) {
        snprintf(fstype, fcap, "ext4");
        if (label && lcap) { memcpy(label, buf + 120, 16 < lcap ? 16 : lcap - 1); label[16 < lcap ? 16 : lcap - 1] = 0; }
        if (uuid_out) memcpy(uuid_out, buf + 0x68, 16);
        return 1;
    }
    /* XFS: magic "XFSB" at byte 0; sb_uuid at 0x20; label sb_fname at 0x6c (12 bytes). */
    if (lkfs_block_pread(backend, buf, 0, 512) == 512 && memcmp(buf, "XFSB", 4) == 0) {
        snprintf(fstype, fcap, "xfs");
        if (label && lcap) { size_t n = 12 < lcap - 1 ? 12 : lcap - 1; memcpy(label, buf + 0x6c, n); label[n] = 0; }
        if (uuid_out) memcpy(uuid_out, buf + 0x20, 16);
        return 1;
    }
    /* Btrfs: magic "_BHRfS_M" at byte 0x10040; fsid at 0x10000+0x20; label at 0x10000+0x12b (256). */
    if (lkfs_block_pread(backend, buf, 0x10000, 4096) == 4096 && memcmp(buf + 0x40, "_BHRfS_M", 8) == 0) {
        snprintf(fstype, fcap, "btrfs");
        if (label && lcap) { strncpy(label, (char *)buf + 0x12b, lcap - 1); label[lcap - 1] = 0; }
        if (uuid_out) memcpy(uuid_out, buf + 0x20, 16);
        return 1;
    }
    return 0;
}

int lkfs_probe(void *backend, char *name_out, size_t name_cap, char *fstype_out, size_t fstype_cap, uint8_t uuid_out[16])
{
    char fstype[16];
    char label[260];
    if (!detect_fs(backend, fstype, sizeof(fstype), label, sizeof(label), uuid_out))
        return 0;
    if (name_out && name_cap) { strncpy(name_out, label, name_cap - 1); name_out[name_cap - 1] = 0; }
    if (fstype_out && fstype_cap) { strncpy(fstype_out, fstype, fstype_cap - 1); fstype_out[fstype_cap - 1] = 0; }
    return 1;
}

/* ------------------------------------------------------------------ */
/* ino -> path map (per volume).                                       */
/* ------------------------------------------------------------------ */
#define IMAP_BUCKETS 2048
struct ino_node { uint64_t ino; char *path; struct ino_node *next; };
struct held_inode {
    uint64_t ino;
    int pin_fd;
    int io_fd;
    unsigned modes;
    int trim_preallocation;
    struct held_inode *next;
};

struct lkfs_volume {
    char mountpoint[64];
    char fstype[16];
    char label[256];
    uint8_t uuid[16];
    unsigned int disk_id;
    struct lkl_disk disk;   /* as populated by lkl_disk_add; .dev needed by lkl_disk_remove */
    void *backend;
    int read_only;
    struct ino_node *buckets[IMAP_BUCKETS];
    struct held_inode *held[IMAP_BUCKETS];
};

static int trim_preallocation(lkfs_volume *v, struct held_inode *item);

static struct held_inode *held_find(lkfs_volume *v, uint64_t ino)
{
    for (struct held_inode *p = v->held[ino % IMAP_BUCKETS]; p; p = p->next)
        if (p->ino == ino) return p;
    return NULL;
}

static int hold_inode(lkfs_volume *v, uint64_t ino, const char *path)
{
    if (held_find(v, ino)) return 0;
    long fd = lkl_sys_open(path, LKL_O_PATH | LKL_O_NOFOLLOW, 0);
    if (fd < 0) return (int)lkfs_host_result(fd);
    struct held_inode *p = calloc(1, sizeof(*p));
    if (!p) { lkl_sys_close((unsigned)fd); return -ENOMEM; }
    p->ino = ino;
    p->pin_fd = (int)fd;
    p->io_fd = -1;
    p->next = v->held[ino % IMAP_BUCKETS];
    v->held[ino % IMAP_BUCKETS] = p;
    return 0;
}

static void release_held(lkfs_volume *v, struct held_inode *p)
{
    (void)trim_preallocation(v, p);
    if (p->io_fd >= 0) lkl_sys_close((unsigned)p->io_fd);
    lkl_sys_close((unsigned)p->pin_fd);
    free(p);
}

static void held_free(lkfs_volume *v)
{
    for (int i = 0; i < IMAP_BUCKETS; i++) {
        while (v->held[i]) {
            struct held_inode *p = v->held[i];
            /* Keep the entry discoverable while trimming its allocation. */
            struct held_inode *next = p->next;
            release_held(v, p);
            v->held[i] = next;
        }
    }
}

static struct ino_node *map_find(lkfs_volume *v, uint64_t ino)
{
    struct ino_node *n = v->buckets[ino % IMAP_BUCKETS];
    for (; n; n = n->next)
        if (n->ino == ino) return n;
    return NULL;
}

static const char *map_get(lkfs_volume *v, uint64_t ino)
{
    struct ino_node *n = map_find(v, ino);
    return n ? n->path : NULL;
}

/* Record an ino->path mapping. An inode may be reachable by several names (hard
 * links), so dedup on (ino, path) rather than ino: every distinct path gets a node,
 * and removing one name (map_forget_subtree) leaves the inode's other names in place,
 * so a held handle still resolves to a surviving link. */
static void map_put(lkfs_volume *v, uint64_t ino, const char *path)
{
    for (struct ino_node *it = v->buckets[ino % IMAP_BUCKETS]; it; it = it->next)
        if (it->ino == ino && strcmp(it->path, path) == 0) return;
    struct ino_node *n = malloc(sizeof(*n));
    if (!n) return;
    n->ino = ino;
    n->path = strdup(path);
    if (!n->path) { free(n); return; }
    n->next = v->buckets[ino % IMAP_BUCKETS];
    v->buckets[ino % IMAP_BUCKETS] = n;
}

static void map_free(lkfs_volume *v)
{
    for (int i = 0; i < IMAP_BUCKETS; i++) {
        struct ino_node *n = v->buckets[i];
        while (n) { struct ino_node *x = n->next; free(n->path); free(n); n = x; }
        v->buckets[i] = NULL;
    }
}

/* Remove the cached entry for `prefix` and anything under `prefix/`. Used after
 * a remove (unlink/rmdir) and to drop an overwritten rename target, so a stale
 * held inode can't keep resolving to — or be confused with — a recreated path. */
static void map_forget_subtree(lkfs_volume *v, const char *prefix)
{
    size_t plen = strlen(prefix);
    for (int i = 0; i < IMAP_BUCKETS; i++) {
        struct ino_node **pp = &v->buckets[i];
        while (*pp) {
            const char *p = (*pp)->path;
            if (strcmp(p, prefix) == 0 || (strncmp(p, prefix, plen) == 0 && p[plen] == '/')) {
                struct ino_node *dead = *pp;
                *pp = dead->next;
                free(dead->path);
                free(dead);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

/* After renaming old_prefix -> new_prefix, re-point every cached path that is
 * old_prefix itself or lives under old_prefix/ so a held FSItem (keyed by ino)
 * stays resolvable. The bucket is keyed by ino, not path, so mutating the path
 * in place keeps the node in its bucket. */
static void map_rebase(lkfs_volume *v, const char *old_prefix, const char *new_prefix)
{
    size_t oldlen = strlen(old_prefix);
    for (int i = 0; i < IMAP_BUCKETS; i++) {
        for (struct ino_node *n = v->buckets[i]; n; n = n->next) {
            char *np = NULL;
            if (strcmp(n->path, old_prefix) == 0) {
                np = strdup(new_prefix);
            } else if (strncmp(n->path, old_prefix, oldlen) == 0 && n->path[oldlen] == '/') {
                const char *tail = n->path + oldlen;   /* includes the leading '/' */
                size_t need = strlen(new_prefix) + strlen(tail) + 1;
                np = malloc(need);
                if (np) snprintf(np, need, "%s%s", new_prefix, tail);
            }
            if (np) { free(n->path); n->path = np; }
        }
    }
}

/* Max path length we build/store; Linux PATH_MAX. */
#define LKFS_PATH_MAX 4096

/* Build "<dir path>/<name>" into out. Returns 0, or -1 if it would truncate. */
static int join(char *out, size_t cap, const char *dir, const char *name)
{
    int n = (strcmp(dir, "/") == 0) ? snprintf(out, cap, "/%s", name)
                                    : snprintf(out, cap, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Map a held ino to its path, or NULL. */
static const char *ino_path(lkfs_volume *v, uint64_t ino)
{
    if (ino == LKFS_ROOT_INO) return v->mountpoint;
    return map_get(v, ino);
}

static int inode_stat(lkfs_volume *v, uint64_t ino, struct lkl_stat *st)
{
    struct held_inode *p = held_find(v, ino);
    if (p) return (int)lkfs_host_result(lkl_sys_fstat(p->pin_fd, st));
    const char *path = ino_path(v, ino);
    return path ? (int)lkfs_host_result(lkl_sys_lstat(path, st)) : -ENOENT;
}

static int inode_statx(lkfs_volume *v, uint64_t ino, struct lkl_statx *st)
{
    struct held_inode *p = held_find(v, ino);
    const char *path = p ? "" : ino_path(v, ino);
    if (!path) return -ENOENT;
    memset(st, 0, sizeof(*st));
    return (int)lkfs_host_result(lkl_sys_statx(p ? p->pin_fd : LKL_AT_FDCWD, path,
        LKL_AT_SYMLINK_NOFOLLOW | (p ? LKL_AT_EMPTY_PATH : 0), LKL_STATX_BASIC_STATS | LKL_STATX_BTIME, st));
}

/* procfs magic links reopen the pinned inode, even after its last name is gone.
 * Never turn symlink metadata operations into operations on the target. */
static int open_inode(lkfs_volume *v, uint64_t ino, int flags)
{
    struct lkl_stat st;
    int rc = inode_stat(v, ino, &st);
    if (rc) return rc;
    if (!LKL_S_ISREG(st.st_mode) && !LKL_S_ISDIR(st.st_mode)) return -ENOTSUP;
    if (flags & (LKL_O_WRONLY | LKL_O_RDWR)) {
        struct lkl_statx extended;
        rc = inode_statx(v, ino, &extended);
        if (rc) return rc;
        if (extended.stx_attributes & extended.stx_attributes_mask & LKL_STATX_ATTR_APPEND)
            flags |= LKL_O_APPEND;
    }
    struct held_inode *p = held_find(v, ino);
    char proc[64];
    const char *path = ino_path(v, ino);
    if (p) { snprintf(proc, sizeof(proc), "/proc/self/fd/%d", p->pin_fd); path = proc; }
    if (!path) return -ENOENT;
    return (int)lkfs_host_result(lkl_sys_open(path, flags | (p ? 0 : LKL_O_NOFOLLOW), 0));
}

int lkfs_open_item(lkfs_volume *v, uint64_t ino, unsigned modes)
{
    if (!modes || (modes & ~(unsigned)(LKFS_OPEN_READ | LKFS_OPEN_WRITE))) return -EINVAL;
    if (v->read_only && (modes & LKFS_OPEN_WRITE)) return -EROFS;
    struct held_inode *p = held_find(v, ino);
    if (!p) {
        const char *path = ino_path(v, ino);
        if (!path) return -ENOENT;
        int rc = hold_inode(v, ino, path);
        if (rc) return rc;
        p = held_find(v, ino);
    }
    modes |= p->modes;
    if (p->io_fd >= 0 && p->modes == modes) return 0;
    int flags = modes == LKFS_OPEN_READ ? LKL_O_RDONLY :
                modes == LKFS_OPEN_WRITE ? LKL_O_WRONLY : LKL_O_RDWR;
    int fd = open_inode(v, ino, flags);
    if (fd < 0) return fd;
    if (p->io_fd >= 0) lkl_sys_close((unsigned)p->io_fd);
    p->io_fd = fd;
    p->modes = modes;
    return 0;
}

int lkfs_close_item(lkfs_volume *v, uint64_t ino, unsigned keeping_modes)
{
    struct held_inode *p = held_find(v, ino);
    if (!p) return 0;
    if (keeping_modes & ~p->modes) return -EINVAL;
    p->modes = keeping_modes;
    if (keeping_modes) return 0;
    int rc = trim_preallocation(v, p);
    if (p->io_fd >= 0) { lkl_sys_close((unsigned)p->io_fd); p->io_fd = -1; }
    return rc;
}

void lkfs_reclaim_item(lkfs_volume *v, uint64_t ino)
{
    struct held_inode **pp = &v->held[ino % IMAP_BUCKETS];
    while (*pp) {
        if ((*pp)->ino == ino) {
            struct held_inode *dead = *pp;
            struct held_inode *next = dead->next;
            release_held(v, dead);
            *pp = next;
            break;
        }
        pp = &(*pp)->next;
    }
    struct ino_node **np = &v->buckets[ino % IMAP_BUCKETS];
    while (*np) {
        if ((*np)->ino == ino) {
            struct ino_node *dead = *np;
            *np = dead->next;
            free(dead->path); free(dead);
        } else np = &(*np)->next;
    }
}

/* Reported ino for a real st_ino (root collapses to LKFS_ROOT_INO). */
static uint64_t report_ino(lkfs_volume *v, uint64_t real, const char *path)
{
    if (strcmp(path, v->mountpoint) == 0) return LKFS_ROOT_INO;
    return real;
}

/* ------------------------------------------------------------------ */
/* mount / umount / statfs / sync                                      */
/* ------------------------------------------------------------------ */
/* Remove a disk (added via lkl_disk_add) under the kernel lock. */
static void disk_remove_locked(struct lkl_disk disk)
{
    pthread_mutex_lock(&g_lock);
    lkl_disk_remove(disk);
    pthread_mutex_unlock(&g_lock);
}

lkfs_volume *lkfs_mount(void *backend, bool read_only, int *out_errno)
{
    char fstype[16], label[260];
    uint8_t uuid[16];
    if (!detect_fs(backend, fstype, sizeof(fstype), label, sizeof(label), uuid)) {
        if (out_errno) *out_errno = EINVAL;
        return NULL;
    }
    int e = ensure_kernel();
    if (e) { if (out_errno) *out_errno = -e; return NULL; }

    struct lkl_disk disk;
    memset(&disk, 0, sizeof(disk));
    disk.handle = backend;
    disk.ops = &lkl_fskit_blk_ops;

    pthread_mutex_lock(&g_lock);
    int disk_id = lkl_disk_add(&disk);
    pthread_mutex_unlock(&g_lock);
    if (disk_id < 0) { if (out_errno) *out_errno = ENXIO; return NULL; }

    lkfs_volume *v = calloc(1, sizeof(*v));
    if (!v) { disk_remove_locked(disk); if (out_errno) *out_errno = ENOMEM; return NULL; }
    v->backend = backend;
    v->disk_id = (unsigned int)disk_id;
    v->disk = disk;
    v->read_only = read_only ? 1 : 0;
    strncpy(v->fstype, fstype, sizeof(v->fstype) - 1);
    strncpy(v->label, label, sizeof(v->label) - 1);
    memcpy(v->uuid, uuid, 16);

    /* A read-only mount must not replay a dirty journal/log: recovery needs to write,
     * which fails outright on read-only media (the reproduced "recovery required on
     * readonly filesystem" -> superblock/journal write I/O errors). Skip recovery so the
     * mount is truly write-free — XFS takes `norecovery`, ext2/3/4 take `noload`; both give
     * a last-checkpoint read-only view. A clean fs is unaffected, and a read-write mount
     * still recovers normally. Btrfs also needs nologreplay: MS_RDONLY alone
     * does not suppress tree-log recovery. */
    const char *data = NULL;
    if (read_only) {
        if (strcmp(fstype, "xfs") == 0) data = "norecovery";
        else if (strcmp(fstype, "ext4") == 0) data = "noload";
        else if (strcmp(fstype, "btrfs") == 0) data = "nologreplay";
    }
    long rc = lkl_mount_dev(v->disk_id, 0, fstype,
                            read_only ? LKL_MS_RDONLY : 0, data,
                            v->mountpoint, sizeof(v->mountpoint));
    if (rc) {
        disk_remove_locked(disk);
        free(v);
        if (out_errno) *out_errno = lkfs_host_errno((int)-rc);
        return NULL;
    }
    map_put(v, LKFS_ROOT_INO, v->mountpoint);
    return v;
}

/* Tears down a volume. Returns 0 only when the LKL disk was actually removed — in
 * which case `v` is freed and the caller MUST now release the backend. On any
 * failure it returns a positive errno, does NOT remove the disk, and does NOT free
 * `v`: the disk stays registered and keeps referencing the backend via disk.handle,
 * so the caller must keep BOTH the handle and the backend alive (freeing the backend
 * would dangle; removing the disk would surprise-abort the fs). */
int lkfs_umount(lkfs_volume *v)
{
    if (!v) return 0;
    held_free(v);
    pthread_mutex_lock(&g_lock);
    /* Commit dirty data, then unmount. */
    lkl_sys_sync();
    long rc = lkl_umount_dev(v->disk_id, 0, 0, 2000);
    if (rc != 0) {                       /* still mounted — keep the disk + backend */
        pthread_mutex_unlock(&g_lock);
        return lkfs_host_errno((int)-rc);
    }
    /* umount() detaches the mount, but LKL's cooperative kernel runs the superblock
     * teardown (jbd2 commit + ext4_put_super writing the clean flag, then releasing
     * the block device) as DEFERRED work. Removing the disk before that finishes is
     * a surprise removal that aborts the journal and leaves the fs needing recovery.
     * Wait DETERMINISTICALLY rather than by a fixed sleep: a block device still held
     * by the fs fails O_EXCL open with EBUSY; O_EXCL succeeds only once the fs has
     * released it — i.e. teardown is complete and the fs is marked clean. Poll that
     * (yielding 1ms so the kernel drains), bounded. The mountpoint is "/mnt/%08x"
     * where %08x is the block dev id. */
    unsigned int dev = (unsigned int)strtoul(v->mountpoint + 5, NULL, 16);
    char devp[16];
    snprintf(devp, sizeof(devp), "/dev/%08x", dev);
    lkl_sys_mknod(devp, LKL_S_IFBLK | 0600, (dev_t)dev);
    struct __lkl__kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
    int released = 0;
    for (int i = 0; i < 2000; i++) {     /* bounded ~2s; normally ready in ~15ms */
        long fd = lkl_sys_open(devp, LKL_O_RDONLY | LKL_O_EXCL, 0);
        if (fd >= 0) { lkl_sys_close((unsigned)fd); released = 1; break; }
        lkl_sys_nanosleep(&ts, NULL);
    }
    lkl_sys_unlink(devp);
    if (!released) {                     /* bdev still held — do NOT surprise-remove */
        pthread_mutex_unlock(&g_lock);
        return EBUSY;
    }
    lkl_disk_remove(v->disk);
    pthread_mutex_unlock(&g_lock);
    map_free(v);
    free(v);
    return 0;
}

int lkfs_sync(lkfs_volume *v)
{
    if (!v) return -EINVAL;
    lkl_sys_sync();
    return 0;
}

int lkfs_statfs(lkfs_volume *v, lkfs_statfs_t *out)
{
    if (!v || !out) return -EINVAL;
    struct lkl_statfs st;
    memset(out, 0, sizeof(*out));
    long rc = lkl_sys_statfs(v->mountpoint, &st);
    if (rc) return (int)lkfs_host_result(rc);
    out->block_size = (uint32_t)st.f_bsize;
    out->total_blocks = (uint64_t)st.f_blocks;
    out->free_blocks = (uint64_t)st.f_bfree;
    out->total_files = (uint64_t)st.f_files;
    out->free_files = (uint64_t)st.f_ffree;
    out->read_only = v->read_only != 0;
    strncpy(out->fs_type, v->fstype, sizeof(out->fs_type) - 1);
    strncpy(out->volume_name, v->label, sizeof(out->volume_name) - 1);
    memcpy(out->uuid, v->uuid, 16);
    return 0;
}

int lkfs_set_volume_name(lkfs_volume *v, const char *name)
{
    if (v->read_only) return -EROFS;
    if (!name || !*name || strchr(name, '/')) return -EINVAL;
    size_t limit = strcmp(v->fstype, "ext4") == 0 ? 16 : strcmp(v->fstype, "xfs") == 0 ? 12 : 255;
    if (strlen(name) > limit) return -ENAMETOOLONG;
    char label[LKL_FSLABEL_MAX] = {0};
    memcpy(label, name, strlen(name));
    int fd = open_inode(v, LKFS_ROOT_INO, LKL_O_RDONLY | LKL_O_DIRECTORY);
    if (fd < 0) return fd;
    long rc = lkl_sys_ioctl((unsigned)fd, LKL_FS_IOC_SETFSLABEL, (unsigned long)label);
    lkl_sys_close((unsigned)fd);
    if (rc) return (int)lkfs_host_result(rc);
    memcpy(v->label, label, sizeof(v->label));
    return 0;
}

/* Physical EOF includes unwritten extents beyond st_size; st_blocks alone
 * cannot locate it on sparse files. Query FIEMAP in bounded batches. */
static int allocated_end(int fd, uint64_t *end)
{
    struct fiemap *map = calloc(1, sizeof(*map) + 16 * sizeof(struct fiemap_extent));
    if (!map) return -ENOMEM;
    uint64_t cursor = 0;
    int error = 0;
    for (;;) {
        memset(map, 0, sizeof(*map) + 16 * sizeof(struct fiemap_extent));
        map->fm_start = cursor;
        map->fm_length = UINT64_MAX - cursor;
        map->fm_flags = LKFS_FIEMAP_SYNC;
        map->fm_extent_count = 16;
        long rc = lkl_sys_ioctl((unsigned)fd, LKL_FS_IOC_FIEMAP, (unsigned long)map);
        if (rc) { error = (int)lkfs_host_result(rc); break; }
        if (!map->fm_mapped_extents) break;
        if (map->fm_mapped_extents > 16) { error = -EIO; break; }
        struct fiemap_extent *last = &map->fm_extents[map->fm_mapped_extents - 1];
        if (last->fe_length > UINT64_MAX - last->fe_logical ||
            last->fe_logical + last->fe_length <= cursor) { error = -EIO; break; }
        cursor = last->fe_logical + last->fe_length;
        if ((last->fe_flags & LKFS_FIEMAP_LAST) || cursor == UINT64_MAX) break;
    }
    free(map);
    if (!error) *end = cursor;
    return error;
}

static int trim_preallocation(lkfs_volume *v, struct held_inode *p)
{
    if (!p->trim_preallocation || v->read_only) return 0;
    int fd = open_inode(v, p->ino, LKL_O_WRONLY);
    if (fd < 0) return fd;
    struct lkl_stat st;
    int error = (int)lkfs_host_result(lkl_sys_fstat(fd, &st));
    /* hole-punch may ignore extents past EOF. Truncating to the current size
     * drops only our temporary tail; preallocate rejects pre-existing tails. */
    if (!error) error = (int)lkfs_host_result(lkl_sys_ftruncate((unsigned)fd, st.st_size));
    lkl_sys_close((unsigned)fd);
    if (!error) p->trim_preallocation = 0;
    return error;
}

int64_t lkfs_preallocate(lkfs_volume *v, uint64_t ino, int64_t offset, int64_t length, unsigned flags)
{
    if (v->read_only) return -EROFS;
    if (length < 0 || (!(flags & LKFS_PREALLOC_FROM_EOF) && offset < 0)) return -EINVAL;
    if (flags & ~(unsigned)(LKFS_PREALLOC_CONTIGUOUS | LKFS_PREALLOC_ALL | LKFS_PREALLOC_PERSIST | LKFS_PREALLOC_FROM_EOF)) return -EINVAL;
    /* fallocate does not promise contiguous extents or rollback on ENOSPC. */
    if (flags & (LKFS_PREALLOC_CONTIGUOUS | LKFS_PREALLOC_ALL)) return -ENOTSUP;
    if (!length) return 0;
    int rc = lkfs_open_item(v, ino, LKFS_OPEN_WRITE);
    if (rc) return rc;
    struct held_inode *p = held_find(v, ino);
    uint64_t old_end;
    rc = allocated_end(p->io_fd, &old_end);
    if (rc) return rc;
    if (flags & LKFS_PREALLOC_FROM_EOF) {
        if (old_end > INT64_MAX) return -EFBIG;
        offset = (int64_t)old_end;
    }
    if (offset > INT64_MAX - length) return -EFBIG;
    if (!(flags & LKFS_PREALLOC_PERSIST)) {
        struct lkl_stat st;
        rc = (int)lkfs_host_result(lkl_sys_fstat(p->io_fd, &st));
        if (rc) return rc;
        uint64_t block = st.st_blksize > 0 ? (uint64_t)st.st_blksize : 4096;
        uint64_t rounded_size = ((uint64_t)st.st_size + block - 1) / block * block;
        if (offset < st.st_size || (!p->trim_preallocation && old_end > rounded_size)) return -ENOTSUP;
    }
    long result = lkl_sys_fallocate(p->io_fd, LKFS_FALLOC_KEEP_SIZE, offset, length);
    /* Even an unsuccessful fallocate may have reserved some extents. */
    if (!(flags & LKFS_PREALLOC_PERSIST)) p->trim_preallocation = 1;
    /* A subsequent persistent request promotes the entire reserved tail. */
    else if (!result) p->trim_preallocation = 0;
    return result < 0 ? lkfs_host_result(result) : length;
}

int64_t lkfs_seek_region(lkfs_volume *v, uint64_t ino, int64_t offset, bool hole)
{
    if (offset < 0) return -EINVAL;
    int fd = open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) return fd;
    struct lkl_stat st;
    long rc = lkl_sys_fstat(fd, &st);
    if (!rc && !LKL_S_ISREG(st.st_mode)) rc = -LKL_EINVAL;
    if (!rc) rc = hole && offset == st.st_size ? offset :
                  lkl_sys_lseek((unsigned)fd, offset, hole ? LKL_SEEK_HOLE : LKL_SEEK_DATA);
    lkl_sys_close((unsigned)fd);
    return lkfs_host_result(rc);
}

/* ------------------------------------------------------------------ */
/* attr helpers                                                        */
/* ------------------------------------------------------------------ */
static uint32_t mode_to_type(uint32_t mode)
{
    switch (mode & LKL_S_IFMT) {
    case LKL_S_IFDIR:  return LKFS_TYPE_DIR;
    case LKL_S_IFREG:  return LKFS_TYPE_FILE;
    case LKL_S_IFLNK:  return LKFS_TYPE_SYMLINK;
    case LKL_S_IFIFO:  return LKFS_TYPE_FIFO;
    case LKL_S_IFCHR:  return LKFS_TYPE_CHARDEV;
    case LKL_S_IFBLK:  return LKFS_TYPE_BLOCKDEV;
    case LKL_S_IFSOCK: return LKFS_TYPE_SOCKET;
    default:           return LKFS_TYPE_UNKNOWN;
    }
}

static uint32_t dtype_to_type(unsigned char d_type)
{
    switch (d_type) {
    case 4:  return LKFS_TYPE_DIR;      /* DT_DIR */
    case 8:  return LKFS_TYPE_FILE;     /* DT_REG */
    case 10: return LKFS_TYPE_SYMLINK;  /* DT_LNK */
    case 1:  return LKFS_TYPE_FIFO;     /* DT_FIFO */
    case 2:  return LKFS_TYPE_CHARDEV;  /* DT_CHR */
    case 6:  return LKFS_TYPE_BLOCKDEV; /* DT_BLK */
    case 12: return LKFS_TYPE_SOCKET;   /* DT_SOCK */
    default: return LKFS_TYPE_UNKNOWN;
    }
}

#define PRIVATE_XATTR "user.com.huanchuan.xlinuxfs."
#define BIRTH_XATTR PRIVATE_XATTR "birthtime"
#define BACKUP_XATTR PRIVATE_XATTR "backuptime"
#define HIDDEN_XATTR PRIVATE_XATTR "hidden"

static int read_time_xattr(int fd, const char *name, int64_t *sec, int64_t *nsec, uint8_t *present)
{
    uint8_t value[12];
    long n = lkl_sys_fgetxattr(fd, name, value, sizeof(value));
    if (n == -LKL_ENODATA || n == -LKL_EOPNOTSUPP) return 0;
    if (n < 0) return (int)lkfs_host_result(n);
    if (n != sizeof(value)) return -EIO;
    uint64_t s = 0;
    uint32_t ns = 0;
    for (unsigned i = 0; i < 8; i++) s |= (uint64_t)value[i] << (i * 8);
    for (unsigned i = 0; i < 4; i++) ns |= (uint32_t)value[8 + i] << (i * 8);
    if (ns >= 1000000000) return -EIO;
    *sec = (int64_t)s; *nsec = ns; *present = 1;
    return 0;
}

static int read_hidden(int fd, bool *hidden)
{
    uint8_t value = 0;
    long n = lkl_sys_fgetxattr(fd, HIDDEN_XATTR, &value, sizeof(value));
    *hidden = false;
    if (n == -LKL_ENODATA || n == -LKL_EOPNOTSUPP) return 0;
    if (n < 0) return (int)lkfs_host_result(n);
    if (n != 1 || value > 1) return -EIO;
    *hidden = value != 0;
    return 0;
}

static int write_hidden(int fd, bool hidden)
{
    uint8_t value = 1;
    long rc = hidden ? lkl_sys_fsetxattr(fd, HIDDEN_XATTR, &value, 1, 0)
                     : lkl_sys_fremovexattr(fd, HIDDEN_XATTR);
    if (!hidden && rc == -LKL_ENODATA) return 0;
    return (int)lkfs_host_result(rc);
}

int lkfs_getattr(lkfs_volume *v, uint64_t ino, lkfs_attr_t *out)
{
    struct lkl_statx st;
    int rc = inode_statx(v, ino, &st);
    if (rc) return rc;
    memset(out, 0, sizeof(*out));
    out->ino = ino;
    out->type = mode_to_type(st.stx_mode);
    out->mode = st.stx_mode & 07777;
    out->nlink = st.stx_nlink;
    out->size = st.stx_size;
    out->alloc_size = st.stx_blocks * 512;
    out->mtime_sec = st.stx_mtime.tv_sec; out->mtime_nsec = st.stx_mtime.tv_nsec;
    out->atime_sec = st.stx_atime.tv_sec; out->atime_nsec = st.stx_atime.tv_nsec;
    out->ctime_sec = st.stx_ctime.tv_sec; out->ctime_nsec = st.stx_ctime.tv_nsec;
    out->has_btime = (st.stx_mask & LKL_STATX_BTIME) != 0;
    if (out->has_btime) {
        out->btime_sec = st.stx_btime.tv_sec; out->btime_nsec = st.stx_btime.tv_nsec;
    }
    uint64_t flags = st.stx_attributes & st.stx_attributes_mask;
    if (flags & LKL_STATX_ATTR_IMMUTABLE) out->flags |= UF_IMMUTABLE;
    if (flags & LKL_STATX_ATTR_APPEND) out->flags |= UF_APPEND;
    if (flags & LKL_STATX_ATTR_NODUMP) out->flags |= UF_NODUMP;
    if (out->type != LKFS_TYPE_FILE && out->type != LKFS_TYPE_DIR) return 0;
    int fd = open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) return fd;
    int error = read_time_xattr(fd, BIRTH_XATTR, &out->btime_sec, &out->btime_nsec, &out->has_btime);
    if (!error) error = read_time_xattr(fd, BACKUP_XATTR, &out->backup_sec, &out->backup_nsec, &out->has_backup_time);
    bool hidden = false;
    if (!error) error = read_hidden(fd, &hidden);
    if (hidden) out->flags |= UF_HIDDEN;
    lkl_sys_close((unsigned)fd);
    return error;
}

int lkfs_set_extra_time(lkfs_volume *v, uint64_t ino, bool birth_time, int64_t sec, int64_t nsec)
{
    if (v->read_only) return -EROFS;
    if (nsec < 0 || nsec >= 1000000000) return -EINVAL;
    int fd = open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) return fd;
    uint8_t value[12];
    for (unsigned i = 0; i < 8; i++) value[i] = (uint8_t)((uint64_t)sec >> (i * 8));
    for (unsigned i = 0; i < 4; i++) value[8 + i] = (uint8_t)((uint64_t)nsec >> (i * 8));
    long rc = lkl_sys_fsetxattr(fd, birth_time ? BIRTH_XATTR : BACKUP_XATTR, value, sizeof(value), 0);
    lkl_sys_close((unsigned)fd);
    return (int)lkfs_host_result(rc);
}

int lkfs_set_flags(lkfs_volume *v, uint64_t ino, uint32_t flags)
{
    if (v->read_only) return -EROFS;
    if (flags & ~(uint32_t)(UF_HIDDEN | UF_IMMUTABLE | UF_APPEND | UF_NODUMP)) return -ENOTSUP;
    int fd = open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) return fd;
    int32_t before = 0;
    int rc = (int)lkfs_host_result(lkl_sys_ioctl((unsigned)fd, LKL_FS_IOC_GETFLAGS, (unsigned long)&before));
    bool hidden_before = false;
    if (!rc) rc = read_hidden(fd, &hidden_before);
    if (!rc) {
        int32_t after = before & ~(LKL_FS_IMMUTABLE_FL | LKL_FS_APPEND_FL | LKL_FS_NODUMP_FL);
        if (flags & UF_IMMUTABLE) after |= LKL_FS_IMMUTABLE_FL;
        if (flags & UF_APPEND) after |= LKL_FS_APPEND_FL;
        if (flags & UF_NODUMP) after |= LKL_FS_NODUMP_FL;
        bool hidden = (flags & UF_HIDDEN) != 0;
        /* Clear old write restrictions before changing the private hidden bit,
         * then apply the final native flags. Roll back on an intermediate error. */
        int32_t unlocked = before & ~(LKL_FS_IMMUTABLE_FL | LKL_FS_APPEND_FL);
        bool unlock = hidden != hidden_before && unlocked != before;
        if (unlock) rc = (int)lkfs_host_result(lkl_sys_ioctl((unsigned)fd, LKL_FS_IOC_SETFLAGS, (unsigned long)&unlocked));
        if (!rc && hidden != hidden_before) rc = write_hidden(fd, hidden);
        if (!rc && (after != before || unlock))
            rc = (int)lkfs_host_result(lkl_sys_ioctl((unsigned)fd, LKL_FS_IOC_SETFLAGS, (unsigned long)&after));
        if (rc) {
            if (hidden != hidden_before) (void)write_hidden(fd, hidden_before);
            if (unlock) (void)lkl_sys_ioctl((unsigned)fd, LKL_FS_IOC_SETFLAGS, (unsigned long)&before);
        }
        struct held_inode *p = held_find(v, ino);
        if (!rc && p && p->io_fd >= 0 && ((before ^ after) & LKL_FS_APPEND_FL)) {
            lkl_sys_close((unsigned)p->io_fd);
            p->io_fd = -1;
        }
    }
    lkl_sys_close((unsigned)fd);
    return rc;
}

static int xattr_name(const char *name, char linux_name[256])
{
    if (!name || !*name) return -EINVAL;
    size_t length = strlen(name);
    if (length > 250) return -ENAMETOOLONG;
    if (strncmp(name, &PRIVATE_XATTR[5], strlen(PRIVATE_XATTR) - 5) == 0) return -EPERM;
    memcpy(linux_name, "user.", 5);
    memcpy(linux_name + 5, name, length + 1);
    return 0;
}

int64_t lkfs_get_xattr(lkfs_volume *v, uint64_t ino, const char *name, void *buffer, size_t size)
{
    char mapped[256];
    int rc = xattr_name(name, mapped);
    if (rc) return rc;
    if (size && !buffer) return -EINVAL;
    int fd = open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) return fd;
    long n = lkl_sys_fgetxattr(fd, mapped, buffer, size);
    lkl_sys_close((unsigned)fd);
    return lkfs_host_result(n);
}

int lkfs_set_xattr(lkfs_volume *v, uint64_t ino, const char *name,
                   const void *value, size_t size, unsigned policy)
{
    if (v->read_only) return -EROFS;
    if (policy > LKFS_XATTR_DELETE || (size && !value)) return -EINVAL;
    if (size > LKFS_XATTR_MAX) return -E2BIG;
    char mapped[256];
    int rc = xattr_name(name, mapped);
    if (rc) return rc;
    int fd = open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) return fd;
    long result = policy == LKFS_XATTR_DELETE ? lkl_sys_fremovexattr(fd, mapped) :
        lkl_sys_fsetxattr(fd, mapped, value, size, (int)policy);
    lkl_sys_close((unsigned)fd);
    return (int)lkfs_host_result(result);
}

int64_t lkfs_list_xattrs(lkfs_volume *v, uint64_t ino, char *buffer, size_t size)
{
    if (size && !buffer) return -EINVAL;
    int fd = open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) return fd;
    char *names = malloc(LKFS_XATTR_MAX);
    if (!names) { lkl_sys_close((unsigned)fd); return -ENOMEM; }
    long n = lkl_sys_flistxattr(fd, names, LKFS_XATTR_MAX);
    lkl_sys_close((unsigned)fd);
    if (n < 0) { free(names); return lkfs_host_result(n); }
    size_t used = 0;
    for (size_t pos = 0; pos < (size_t)n;) {
        size_t length = strnlen(names + pos, (size_t)n - pos);
        if (length == (size_t)n - pos) { free(names); return -EIO; }
        const char *name = names + pos;
        if (length > 5 && strncmp(name, "user.", 5) == 0 &&
            strncmp(name, PRIVATE_XATTR, strlen(PRIVATE_XATTR)) != 0) {
            size_t bytes = length - 5 + 1;
            if (buffer && used + bytes > size) { free(names); return -ERANGE; }
            if (buffer) memcpy(buffer + used, name + 5, bytes);
            used += bytes;
        }
        pos += length + 1;
    }
    free(names);
    return (int64_t)used;
}

uint64_t lkfs_lookup(lkfs_volume *v, uint64_t dir_ino, const char *name, int *out_errno)
{
    const char *dir = ino_path(v, dir_ino);
    if (!dir) { if (out_errno) *out_errno = ENOENT; return 0; }
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) { if (out_errno) *out_errno = ENAMETOOLONG; return 0; }
    struct lkl_stat st;
    long rc = lkl_sys_lstat(path, &st);
    if (rc) { if (out_errno) *out_errno = lkfs_host_errno((int)-rc); return 0; }
    uint64_t ino = report_ino(v, (uint64_t)st.st_ino, path);
    int held = hold_inode(v, ino, path);
    if (held) { if (out_errno) *out_errno = -held; return 0; }
    map_put(v, ino, path);
    return ino;
}

/* ------------------------------------------------------------------ */
/* readdir (getdents64) — struct lkl_linux_dirent64 comes from lkl headers */
/* ------------------------------------------------------------------ */
int lkfs_readdir(lkfs_volume *v, uint64_t dir_ino, int64_t start_cookie, void *ctx, lkfs_dir_cb cb)
{
    const char *dir = ino_path(v, dir_ino);
    int fd = open_inode(v, dir_ino, LKL_O_RDONLY | LKL_O_DIRECTORY);
    if (fd < 0) return (int)fd;

    char buf[8192];
    int64_t index = 0;       /* 0-based position among non-dot entries */
    int rc = 0;
    for (;;) {
        long n = lkl_sys_getdents64((unsigned int)fd, (struct lkl_linux_dirent64 *)buf, sizeof(buf));
        if (n < 0) { rc = (int)lkfs_host_result(n); break; }
        if (n == 0) break;
        for (long off = 0; off < n; ) {
            struct lkl_linux_dirent64 *d = (struct lkl_linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
                continue;
            if (index++ < start_cookie)
                continue;
            char child[LKFS_PATH_MAX];
            uint64_t ino = d->d_ino;
            if (dir) {
                if (join(child, sizeof(child), dir, d->d_name)) { rc = -ENAMETOOLONG; goto done; }
                ino = report_ino(v, d->d_ino, child);
                map_put(v, ino, child);
            }
            if (cb(ctx, d->d_name, ino, dtype_to_type(d->d_type), index) != 0)
                goto done;
        }
    }
done:
    lkl_sys_close((unsigned int)fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* read / write                                                        */
/* ------------------------------------------------------------------ */
int64_t lkfs_read(lkfs_volume *v, uint64_t ino, int64_t offset, void *buf, int64_t len, int *out_errno)
{
    if (offset < 0 || len < 0) { if (out_errno) *out_errno = EINVAL; return -1; }
    struct held_inode *p = held_find(v, ino);
    bool cached = p && p->io_fd >= 0 && (p->modes & LKFS_OPEN_READ);
    int fd = cached ? p->io_fd : open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) { if (out_errno) *out_errno = -fd; return -1; }
    long n = lkl_sys_pread64((unsigned)fd, buf, (size_t)len, offset);
    if (!cached) lkl_sys_close((unsigned)fd);
    if (n < 0) { if (out_errno) *out_errno = lkfs_host_errno((int)-n); return -1; }
    return n;
}

int64_t lkfs_write(lkfs_volume *v, uint64_t ino, int64_t offset, const void *buf, int64_t len, int *out_errno)
{
    if (v->read_only) { if (out_errno) *out_errno = EROFS; return -1; }
    if (offset < 0 || len < 0) { if (out_errno) *out_errno = EINVAL; return -1; }
    struct lkl_statx st;
    int rc = inode_statx(v, ino, &st);
    if (rc) { if (out_errno) *out_errno = -rc; return -1; }
    bool append_only = (st.stx_attributes & st.stx_attributes_mask & LKL_STATX_ATTR_APPEND) != 0;
    if (append_only && (uint64_t)offset != st.stx_size) { if (out_errno) *out_errno = EPERM; return -1; }
    struct held_inode *p = held_find(v, ino);
    bool cached = !append_only && p && p->io_fd >= 0 && (p->modes & LKFS_OPEN_WRITE);
    int fd = cached ? p->io_fd : open_inode(v, ino, LKL_O_WRONLY);
    if (fd < 0) { if (out_errno) *out_errno = -fd; return -1; }
    long n = lkl_sys_pwrite64((unsigned)fd, buf, (size_t)len, offset);
    if (!cached) lkl_sys_close((unsigned)fd);
    if (n < 0) { if (out_errno) *out_errno = lkfs_host_errno((int)-n); return -1; }
    return n;
}

/* ------------------------------------------------------------------ */
/* create / remove / truncate / set_times / rename / readlink         */
/* ------------------------------------------------------------------ */
uint64_t lkfs_create(lkfs_volume *v, uint64_t dir_ino, const char *name, uint32_t type, int *out_errno)
{
    if (v->read_only) { if (out_errno) *out_errno = EROFS; return 0; }
    if (type != LKFS_TYPE_FILE && type != LKFS_TYPE_DIR) { if (out_errno) *out_errno = ENOTSUP; return 0; }
    const char *dir = ino_path(v, dir_ino);
    if (!dir) { if (out_errno) *out_errno = ENOENT; return 0; }
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) { if (out_errno) *out_errno = ENAMETOOLONG; return 0; }
    long rc;
    if (type == LKFS_TYPE_DIR) {
        rc = lkl_sys_mkdir(path, 0755);
    } else {
        long fd = lkl_sys_open(path, LKL_O_CREAT | LKL_O_EXCL | LKL_O_WRONLY, 0644);
        if (fd >= 0) { lkl_sys_close((unsigned int)fd); rc = 0; } else rc = fd;
    }
    if (rc) { if (out_errno) *out_errno = lkfs_host_errno((int)-rc); return 0; }
    struct lkl_stat st;
    if (lkl_sys_lstat(path, &st)) { if (out_errno) *out_errno = EIO; return 0; }
    uint64_t ino = report_ino(v, (uint64_t)st.st_ino, path);
    int held = hold_inode(v, ino, path);
    if (held) {
        if (type == LKFS_TYPE_DIR) lkl_sys_rmdir(path); else lkl_sys_unlink(path);
        if (out_errno) *out_errno = -held;
        return 0;
    }
    map_put(v, ino, path);
    return ino;
}

uint64_t lkfs_symlink(lkfs_volume *v, uint64_t dir_ino, const char *name, const char *target, int *out_errno)
{
    if (v->read_only) { if (out_errno) *out_errno = EROFS; return 0; }
    const char *dir = ino_path(v, dir_ino);
    if (!dir) { if (out_errno) *out_errno = ENOENT; return 0; }
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) { if (out_errno) *out_errno = ENAMETOOLONG; return 0; }
    long rc = lkl_sys_symlink(target, path);   /* symlink(target, linkpath) */
    if (rc) { if (out_errno) *out_errno = lkfs_host_errno((int)-rc); return 0; }
    struct lkl_stat st;
    if (lkl_sys_lstat(path, &st)) { if (out_errno) *out_errno = EIO; return 0; }
    uint64_t ino = report_ino(v, (uint64_t)st.st_ino, path);
    int held = hold_inode(v, ino, path);
    if (held) { lkl_sys_unlink(path); if (out_errno) *out_errno = -held; return 0; }
    map_put(v, ino, path);
    return ino;
}

/* Hard link: give the inode behind `target_ino` an additional name `name` in dir_ino.
 * The new path is recorded for the (same) inode so a held handle survives unlinking
 * either name. ext/XFS/Btrfs support hard links; the kernel refuses them on dirs. */
int lkfs_link(lkfs_volume *v, uint64_t target_ino, uint64_t dir_ino, const char *name, int *out_errno)
{
    if (v->read_only) { if (out_errno) *out_errno = EROFS; return -1; }
    const char *target = ino_path(v, target_ino);
    const char *dir = ino_path(v, dir_ino);
    if (!target || !dir) { if (out_errno) *out_errno = ENOENT; return -1; }
    /* FSKit expects ENOTSUP when hard links aren't supported for the object's type (a dir);
       the kernel would otherwise return EPERM for link(2) on a directory. */
    struct lkl_stat st;
    if (lkl_sys_lstat(target, &st) == 0 && LKL_S_ISDIR(st.st_mode)) {
        if (out_errno) *out_errno = ENOTSUP; return -1;
    }
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) { if (out_errno) *out_errno = ENAMETOOLONG; return -1; }
    long rc = lkl_sys_link(target, path);   /* link(oldpath=target, newpath=path) */
    if (rc) { if (out_errno) *out_errno = lkfs_host_errno((int)-rc); return -1; }
    map_put(v, target_ino, path);
    return 0;
}

int lkfs_remove(lkfs_volume *v, uint64_t dir_ino, const char *name)
{
    if (v->read_only) return -EROFS;
    const char *dir = ino_path(v, dir_ino);
    if (!dir) return -ENOENT;
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) return -ENAMETOOLONG;
    struct lkl_stat st;
    long lrc = lkl_sys_lstat(path, &st);
    if (lrc) return (int)lkfs_host_result(lrc);
    long rc = LKL_S_ISDIR(st.st_mode) ? lkl_sys_rmdir(path) : lkl_sys_unlink(path);
    if (rc == 0) map_forget_subtree(v, path);
    return (int)lkfs_host_result(rc);
}

int lkfs_truncate(lkfs_volume *v, uint64_t ino, uint64_t size)
{
    if (v->read_only) return -EROFS;
    if (size > INT64_MAX) return -EFBIG;
    int fd = open_inode(v, ino, LKL_O_WRONLY);
    if (fd < 0) return fd;
    long rc = lkl_sys_ftruncate((unsigned)fd, (long)size);
    lkl_sys_close((unsigned)fd);
    return (int)lkfs_host_result(rc);
}

/* Apply the POSIX permission bits requested by create / setAttributes (mode is
 * masked to the 07777 perm/setid/sticky bits; the file type is untouched). */
int lkfs_chmod(lkfs_volume *v, uint64_t ino, uint32_t mode)
{
    if (v->read_only) return -EROFS;
    int fd = open_inode(v, ino, LKL_O_RDONLY);
    if (fd < 0) return fd;
    long rc = lkl_sys_fchmod((unsigned)fd, mode & 07777);
    lkl_sys_close((unsigned)fd);
    return (int)lkfs_host_result(rc);
}

int lkfs_set_times(lkfs_volume *v, uint64_t ino,
                   int64_t mtime_sec, int64_t mtime_nsec,
                   int64_t atime_sec, int64_t atime_nsec)
{
    if (v->read_only) return -EROFS;
    struct held_inode *p = held_find(v, ino);
    const char *path = p ? "" : ino_path(v, ino);
    if (!path) return -ENOENT;
    const long UTIME_OMIT_ = ((1L << 30) - 2);   /* skip this timestamp */
    struct __lkl__kernel_timespec ts[2];
    /* [0]=atime [1]=mtime */
    if (atime_sec == INT64_MIN) { ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT_; }
    else { ts[0].tv_sec = atime_sec; ts[0].tv_nsec = atime_nsec; }
    if (mtime_sec == INT64_MIN) { ts[1].tv_sec = 0; ts[1].tv_nsec = UTIME_OMIT_; }
    else { ts[1].tv_sec = mtime_sec; ts[1].tv_nsec = mtime_nsec; }
    return (int)lkfs_host_result(lkl_sys_utimensat(p ? p->pin_fd : LKL_AT_FDCWD, path, ts,
                                                  LKL_AT_SYMLINK_NOFOLLOW | (p ? LKL_AT_EMPTY_PATH : 0)));
}

int lkfs_rename(lkfs_volume *v, uint64_t src_dir, const char *src_name,
                uint64_t dst_dir, const char *dst_name)
{
    if (v->read_only) return -EROFS;
    const char *sd = ino_path(v, src_dir), *dd = ino_path(v, dst_dir);
    if (!sd || !dd) return -ENOENT;
    char sp[LKFS_PATH_MAX], dp[LKFS_PATH_MAX];
    if (join(sp, sizeof(sp), sd, src_name)) return -ENAMETOOLONG;
    if (join(dp, sizeof(dp), dd, dst_name)) return -ENAMETOOLONG;
    struct lkl_stat source, destination;
    if (!lkl_sys_lstat(sp, &source) && !lkl_sys_lstat(dp, &destination) &&
        source.st_ino == destination.st_ino) return 0;
    long rc = lkl_sys_rename(sp, dp);
    if (rc == 0) {
        /* The moved inode (and any cached descendants) now live at dp; re-point
         * their cached paths so a held FSItem stays resolvable. Drop any stale
         * entry (or subtree) for an overwritten destination first. */
        map_forget_subtree(v, dp);
        map_rebase(v, sp, dp);
    }
    return (int)lkfs_host_result(rc);
}

int lkfs_readlink(lkfs_volume *v, uint64_t ino, char *buf, size_t cap)
{
    if (!buf || !cap || cap > INT_MAX) return -EINVAL;
    struct held_inode *p = held_find(v, ino);
    const char *path = p ? "" : ino_path(v, ino);
    if (!path) return -ENOENT;
    long n = lkl_sys_readlinkat(p ? p->pin_fd : LKL_AT_FDCWD, path, buf, (int)cap);
    if (n < 0) return (int)lkfs_host_result(n);
    if ((size_t)n >= cap) return -ENAMETOOLONG;  /* target needs >= cap bytes, no room for the NUL */
    buf[n] = 0;
    return 0;
}
