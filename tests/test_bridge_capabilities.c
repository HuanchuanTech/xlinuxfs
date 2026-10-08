#include <lkl.h>
#include "lkl_fskit.h"
#include "lkl_errno.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned checks;
static int e;
#define EQ(expr, want) do { long long got_ = (long long)(expr), want_ = (long long)(want); checks++; \
    if (got_ != want_) { fprintf(stderr, "FAIL line %d: %s = %lld, wanted %lld (errno %d)\n", \
    __LINE__, #expr, got_, want_, e); exit(1); } } while (0)
#define CHECK(expr) EQ(!!(expr), 1)

static uint64_t create(lkfs_volume *v, const char *name)
{
    uint64_t ino = lkfs_create(v, LKFS_ROOT_INO, name, LKFS_TYPE_FILE, &e);
    CHECK(ino != 0);
    return ino;
}

static void put(lkfs_volume *v, uint64_t ino, const char *text)
{
    EQ(lkfs_write(v, ino, 0, text, strlen(text), &e), strlen(text));
}

static void expect_text(lkfs_volume *v, uint64_t ino, const char *text)
{
    char data[100] = {0};
    EQ(lkfs_read(v, ino, 0, data, sizeof(data), &e), strlen(text));
    CHECK(strcmp(data, text) == 0);
}

static int native_file(const char *name)
{
    int fd = (int)lkl_sys_open("/mnt", LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
    CHECK(fd >= 0);
    char entries[1024], path[512] = {0};
    long n = lkl_sys_getdents64(fd, (struct lkl_linux_dirent64 *)entries, sizeof(entries));
    CHECK(n > 0);
    for (long off = 0; off < n;) {
        struct lkl_linux_dirent64 *entry = (void *)(entries + off);
        if (entry->d_name[0] != '.') { snprintf(path, sizeof(path), "/mnt/%s/%s", entry->d_name, name); break; }
        off += entry->d_reclen;
    }
    lkl_sys_close(fd);
    CHECK(path[0]);
    fd = (int)lkl_sys_open(path, LKL_O_RDONLY, 0);
    CHECK(fd >= 0);
    return fd;
}

static void writable_tests(lkfs_volume *v)
{
    uint64_t ino = create(v, "caps-persistent");
    lkfs_attr_t before, after;
    EQ(lkfs_getattr(v, ino, &before), 0);
    CHECK(before.has_btime);
    put(v, ino, "persistent");
    EQ(lkfs_chmod(v, ino, 0600), 0);
    EQ(lkfs_set_times(v, ino, 1234567890, 123, 1234567891, 456), 0);
    EQ(lkfs_getattr(v, ino, &after), 0);
    EQ(after.btime_sec, before.btime_sec); EQ(after.btime_nsec, before.btime_nsec);
    EQ(after.mtime_sec, 1234567890); EQ(after.atime_nsec, 456);

    const unsigned char binary[] = {0, 0xff, 1, 0, 0x80};
    char buffer[65536];
    EQ(lkfs_get_xattr(v, ino, "com.apple.test", buffer, sizeof(buffer)), -ENOATTR);
    EQ(lkfs_set_xattr(v, ino, "com.apple.test", binary, sizeof(binary), LKFS_XATTR_CREATE), 0);
    EQ(lkfs_set_xattr(v, ino, "com.apple.test", binary, sizeof(binary), LKFS_XATTR_CREATE), -EEXIST);
    EQ(lkfs_set_xattr(v, ino, "absent", binary, sizeof(binary), LKFS_XATTR_REPLACE), -ENOATTR);
    EQ(lkfs_get_xattr(v, ino, "com.apple.test", NULL, 0), sizeof(binary));
    EQ(lkfs_get_xattr(v, ino, "com.apple.test", buffer, 2), -ERANGE);
    EQ(lkfs_get_xattr(v, ino, "com.apple.test", buffer, sizeof(buffer)), sizeof(binary));
    CHECK(memcmp(buffer, binary, sizeof(binary)) == 0);
    EQ(lkfs_set_xattr(v, ino, "empty", NULL, 0, LKFS_XATTR_SET), 0);
    EQ(lkfs_get_xattr(v, ino, "empty", buffer, sizeof(buffer)), 0);
    EQ(lkfs_set_xattr(v, ino, "empty", binary, sizeof(binary), LKFS_XATTR_REPLACE), 0);
    EQ(lkfs_set_xattr(v, ino, "empty", NULL, 0, LKFS_XATTR_DELETE), 0);
    EQ(lkfs_set_xattr(v, ino, "empty", NULL, 0, LKFS_XATTR_DELETE), -ENOATTR);
    EQ(lkfs_set_xattr(v, ino, "com.huanchuan.xlinuxfs.hidden", "1", 1, 0), -EPERM);
    EQ(lkfs_get_xattr(v, ino, "com.huanchuan.xlinuxfs.hidden", NULL, 0), -EPERM);
    EQ(lkfs_set_xattr(v, ino, "too-large", buffer, sizeof(buffer) + 1, 0), -E2BIG);
    EQ(lkfs_set_xattr(v, ino, "", NULL, 0, 0), -EINVAL);
    char long_name[252]; memset(long_name, 'n', sizeof(long_name)); long_name[251] = 0;
    EQ(lkfs_set_xattr(v, ino, long_name, NULL, 0, 0), -ENAMETOOLONG);
    EQ(lkfs_set_xattr(v, ino, "raw-\xff", binary, sizeof(binary), 0), 0);

    int native = native_file("caps-persistent");
    EQ(lkl_sys_fgetxattr(native, "user.com.apple.test", buffer, sizeof(buffer)), sizeof(binary));
    EQ(lkl_sys_fsetxattr(native, "trusted.caps-test", "x", 1, 0), 0);
    EQ(lkfs_set_xattr(v, ino, "security.selinux", "x", 1, 0), 0);
    EQ(lkl_sys_fgetxattr(native, "user.security.selinux", buffer, sizeof(buffer)), 1);
    EQ(lkl_sys_fgetxattr(native, "security.selinux", buffer, sizeof(buffer)), -LKL_ENODATA);
    lkl_sys_close(native);

    EQ(lkfs_set_extra_time(v, ino, true, 1100000000, 987654321), 0);
    EQ(lkfs_set_extra_time(v, ino, false, 1200000000, 123456789), 0);
    EQ(lkfs_set_extra_time(v, ino, true, 0, 1000000000), -EINVAL);
    EQ(lkfs_set_flags(v, ino, UF_HIDDEN | UF_NODUMP), 0);
    EQ(lkfs_getattr(v, ino, &after), 0);
    EQ(after.flags, UF_HIDDEN | UF_NODUMP);
    EQ(after.btime_sec, 1100000000); EQ(after.btime_nsec, 987654321);
    CHECK(after.has_backup_time); EQ(after.backup_sec, 1200000000);
    int64_t n = lkfs_list_xattrs(v, ino, buffer, sizeof(buffer));
    CHECK(n > 0);
    EQ(lkfs_list_xattrs(v, ino, NULL, 0), n);
    EQ(lkfs_list_xattrs(v, ino, buffer, 1), -ERANGE);
    for (int64_t pos = 0; pos < n; pos += strlen(buffer + pos) + 1) {
        CHECK(strncmp(buffer + pos, "com.huanchuan.xlinuxfs.", 22) != 0);
        CHECK(strcmp(buffer + pos, "trusted.caps-test") != 0);
    }
    EQ(lkfs_set_flags(v, ino, UF_IMMUTABLE | UF_HIDDEN), 0);
    EQ(lkfs_write(v, ino, 0, "x", 1, &e), -1); EQ(e, EPERM);
    EQ(lkfs_set_xattr(v, ino, "blocked", "x", 1, 0), -EPERM);
    EQ(lkfs_set_flags(v, ino, UF_APPEND), 0);
    EQ(lkfs_getattr(v, ino, &after), 0); EQ(after.flags, UF_APPEND);
    EQ(lkfs_truncate(v, ino, 0), -EPERM);
    EQ(lkfs_open_item(v, ino, LKFS_OPEN_WRITE), 0);
    EQ(lkfs_write(v, ino, 0, "!", 1, &e), -1); EQ(e, EPERM);
    EQ(lkfs_write(v, ino, 10, "!", 1, &e), 1);
    expect_text(v, ino, "persistent!");
    EQ(lkfs_set_flags(v, ino, UF_HIDDEN | UF_NODUMP), 0);
    EQ(lkfs_write(v, ino, 0, "p", 1, &e), 1);
    EQ(lkfs_getattr(v, ino, &after), 0); EQ(after.size, 11);
    EQ(lkfs_truncate(v, ino, 10), 0);
    EQ(lkfs_set_times(v, ino, 1234567890, 123, 1234567891, 456), 0);
    EQ(lkfs_close_item(v, ino, 0), 0);
    EQ(lkfs_set_flags(v, ino, SF_ARCHIVED), -ENOTSUP);

    uint64_t link = lkfs_symlink(v, LKFS_ROOT_INO, "caps-symlink", "caps-persistent", &e);
    CHECK(link);
    EQ(lkfs_set_xattr(v, link, "com.apple.test", "bad", 3, 0), -ENOTSUP);
    EQ(lkfs_chmod(v, link, 0777), -ENOTSUP);
    EQ(lkfs_set_times(v, link, 1300000000, 0, INT64_MIN, 0), 0);
    EQ(lkfs_getattr(v, ino, &after), 0); EQ(after.mtime_sec, 1234567890);
    EQ(lkfs_remove(v, LKFS_ROOT_INO, "caps-symlink"), 0);
    EQ(lkfs_readlink(v, link, buffer, sizeof(buffer)), 0); CHECK(strcmp(buffer, "caps-persistent") == 0);
    lkfs_reclaim_item(v, link);

    uint64_t old = create(v, "caps-unlink"); put(v, old, "old");
    EQ(lkfs_open_item(v, old, LKFS_OPEN_READ), 0);
    EQ(lkfs_open_item(v, old, LKFS_OPEN_WRITE), 0);
    EQ(lkfs_remove(v, LKFS_ROOT_INO, "caps-unlink"), 0);
    uint64_t replacement = create(v, "caps-unlink"); CHECK(replacement != old); put(v, replacement, "new");
    expect_text(v, old, "old"); put(v, old, "OLD");
    EQ(lkfs_truncate(v, old, 2), 0); expect_text(v, old, "OL");
    EQ(lkfs_set_xattr(v, old, "orphan", "x", 1, 0), 0);
    EQ(lkfs_getattr(v, old, &after), 0); EQ(after.nlink, 0);
    EQ(lkfs_close_item(v, old, LKFS_OPEN_READ), 0); expect_text(v, old, "OL");
    EQ(lkfs_close_item(v, old, 0), 0);
    EQ(lkfs_get_xattr(v, old, "orphan", buffer, sizeof(buffer)), 1);
    lkfs_reclaim_item(v, old);
    EQ(lkfs_getattr(v, old, &after), -ENOENT); expect_text(v, replacement, "new");

    uint64_t source = create(v, "caps-source"), target = create(v, "caps-target");
    put(v, source, "source"); put(v, target, "target");
    EQ(lkfs_open_item(v, target, LKFS_OPEN_READ | LKFS_OPEN_WRITE), 0);
    EQ(lkfs_rename(v, LKFS_ROOT_INO, "caps-source", LKFS_ROOT_INO, "caps-target"), 0);
    expect_text(v, target, "target"); expect_text(v, source, "source");
    EQ(lkfs_lookup(v, LKFS_ROOT_INO, "caps-target", &e), source);
    EQ(lkfs_rename(v, LKFS_ROOT_INO, "caps-target", LKFS_ROOT_INO, "caps-target"), 0);
    expect_text(v, source, "source");
    EQ(lkfs_link(v, source, LKFS_ROOT_INO, "caps-hardlink", &e), 0);
    EQ(lkfs_rename(v, LKFS_ROOT_INO, "caps-target", LKFS_ROOT_INO, "caps-hardlink"), 0);
    EQ(lkfs_remove(v, LKFS_ROOT_INO, "caps-hardlink"), 0); expect_text(v, source, "source");

    uint64_t dir = lkfs_create(v, LKFS_ROOT_INO, "caps-dir", LKFS_TYPE_DIR, &e);
    CHECK(dir);
    uint64_t child = lkfs_create(v, dir, "child", LKFS_TYPE_FILE, &e); CHECK(child);
    put(v, child, "nested");
    EQ(lkfs_set_xattr(v, dir, "directory", binary, sizeof(binary), 0), 0);
    EQ(lkfs_rename(v, LKFS_ROOT_INO, "caps-dir", LKFS_ROOT_INO, "caps-moved-dir"), 0);
    expect_text(v, child, "nested");
    EQ(lkfs_lookup(v, dir, "child", &e), child);
    EQ(lkfs_get_xattr(v, dir, "directory", buffer, sizeof(buffer)), sizeof(binary));

    uint64_t held[1200];
    for (unsigned i = 0; i < 1200; i++) {
        char name[32]; snprintf(name, sizeof(name), "caps-held-%u", i);
        held[i] = create(v, name);
    }
    for (unsigned i = 0; i < 1200; i++) {
        char name[32]; snprintf(name, sizeof(name), "caps-held-%u", i);
        EQ(lkfs_remove(v, LKFS_ROOT_INO, name), 0);
        lkfs_reclaim_item(v, held[i]);
    }
    for (unsigned i = 0; i < 1200; i++) {
        uint64_t recycled = create(v, "caps-recycle");
        EQ(lkfs_open_item(v, recycled, LKFS_OPEN_READ | LKFS_OPEN_WRITE), 0);
        EQ(lkfs_remove(v, LKFS_ROOT_INO, "caps-recycle"), 0);
        EQ(lkfs_close_item(v, recycled, 0), 0);
        lkfs_reclaim_item(v, recycled);
    }

    uint64_t sparse = create(v, "caps-sparse");
    EQ(lkfs_truncate(v, sparse, 4 * 1024 * 1024), 0);
    EQ(lkfs_write(v, sparse, 2 * 1024 * 1024, "x", 1, &e), 1);
    EQ(lkfs_sync(v), 0);
    EQ(lkfs_seek_region(v, sparse, 0, true), 0);
    n = lkfs_seek_region(v, sparse, 0, false); CHECK(n > 0 && n <= 2 * 1024 * 1024);
    EQ(lkfs_seek_region(v, sparse, 4 * 1024 * 1024, true), 4 * 1024 * 1024);
    EQ(lkfs_seek_region(v, sparse, 4 * 1024 * 1024, false), -ENXIO);
    EQ(lkfs_seek_region(v, sparse, -1, false), -EINVAL);
    EQ(lkfs_getattr(v, sparse, &after), 0); CHECK(after.alloc_size < after.size);

    uint64_t allocation = create(v, "caps-allocation");
    EQ(lkfs_preallocate(v, allocation, 0, 65536, LKFS_PREALLOC_CONTIGUOUS), -ENOTSUP);
    EQ(lkfs_preallocate(v, allocation, 0, 65536, LKFS_PREALLOC_ALL), -ENOTSUP);
    EQ(lkfs_preallocate(v, allocation, 999, 65536, LKFS_PREALLOC_FROM_EOF | LKFS_PREALLOC_PERSIST), 65536);
    EQ(lkfs_close_item(v, allocation, 0), 0);
    EQ(lkfs_getattr(v, allocation, &before), 0); EQ(before.size, 0); CHECK(before.alloc_size >= 65536);
    EQ(lkfs_preallocate(v, allocation, 0, 65536, LKFS_PREALLOC_FROM_EOF), -ENOTSUP);
    EQ(lkfs_close_item(v, allocation, 0), 0);
    EQ(lkfs_getattr(v, allocation, &after), 0); EQ(after.alloc_size, before.alloc_size);
    uint64_t temporary = create(v, "caps-temporary-allocation");
    EQ(lkfs_preallocate(v, temporary, 0, 65536, LKFS_PREALLOC_FROM_EOF), 65536);
    EQ(lkfs_preallocate(v, temporary, 0, 65536, LKFS_PREALLOC_FROM_EOF), 65536);
    put(v, temporary, "keep");
    EQ(lkfs_close_item(v, temporary, 0), 0); expect_text(v, temporary, "keep");
    EQ(lkfs_getattr(v, temporary, &after), 0); CHECK(after.alloc_size < 65536);
    EQ(lkfs_set_volume_name(v, "XLinux-Test"), 0);
    memset(buffer, 'x', 256); buffer[256] = 0;
    EQ(lkfs_set_volume_name(v, buffer), -ENAMETOOLONG);
    EQ(lkfs_set_volume_name(v, "bad/name"), -EINVAL);
}

static void readonly_tests(lkfs_volume *v)
{
    uint64_t ino = lkfs_lookup(v, LKFS_ROOT_INO, "caps-persistent", &e);
    CHECK(ino);
    expect_text(v, ino, "persistent");
    lkfs_attr_t attrs;
    EQ(lkfs_getattr(v, ino, &attrs), 0);
    EQ(attrs.btime_sec, 1100000000); EQ(attrs.btime_nsec, 987654321);
    EQ(attrs.backup_sec, 1200000000); EQ(attrs.backup_nsec, 123456789);
    EQ(attrs.flags, UF_HIDDEN | UF_NODUMP);
    char value[32]; EQ(lkfs_get_xattr(v, ino, "com.apple.test", value, sizeof(value)), 5);
    EQ(lkfs_set_xattr(v, ino, "test", "x", 1, 0), -EROFS);
    EQ(lkfs_set_xattr(v, ino, "com.apple.test", NULL, 0, LKFS_XATTR_DELETE), -EROFS);
    EQ(lkfs_set_flags(v, ino, 0), -EROFS);
    EQ(lkfs_set_extra_time(v, ino, true, 0, 0), -EROFS);
    EQ(lkfs_set_times(v, ino, 0, 0, 0, 0), -EROFS);
    EQ(lkfs_set_volume_name(v, "readonly"), -EROFS);
    EQ(lkfs_preallocate(v, ino, 0, 65536, 0), -EROFS);
    EQ(lkfs_open_item(v, ino, LKFS_OPEN_WRITE), -EROFS);
    EQ(lkfs_open_item(v, ino, LKFS_OPEN_READ), 0);
    EQ(lkfs_close_item(v, ino, 0), 0);
    lkfs_statfs_t stats; EQ(lkfs_statfs(v, &stats), 0); CHECK(strcmp(stats.volume_name, "XLinux-Test") == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    EQ(lkfs_host_errno(LKL_ENODATA), ENOATTR);
    EQ(lkfs_host_errno(LKL_EOPNOTSUPP), ENOTSUP);
    EQ(lkfs_host_errno(LKL_EBADMSG), EBADMSG);
    EQ(lkfs_host_errno(LKL_ENOTEMPTY), ENOTEMPTY);
    bool writable = strcmp(argv[2], "rw") == 0;
    void *backend = lkfs_backend_from_file(argv[1], writable, &e); CHECK(backend);
    lkfs_volume *v = lkfs_mount(backend, !writable, &e); CHECK(v);
    lkfs_statfs_t stats; EQ(lkfs_statfs(v, &stats), 0);
    if (writable) writable_tests(v); else readonly_tests(v);
    EQ(lkfs_umount(v), 0);
    lkfs_backend_free(backend);
    printf("PASS %s %s: %u capability checks\n", stats.fs_type, argv[2], checks);
    return 0;
}
