/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Linux FIEMAP UAPI layout; not included in LKL's exported header subset. */
#ifndef LKL_FSKIT_FIEMAP_H
#define LKL_FSKIT_FIEMAP_H
#include <stdint.h>

struct fiemap_extent {
    uint64_t fe_logical, fe_physical, fe_length, fe_reserved64[2];
    uint32_t fe_flags, fe_reserved[3];
};
struct fiemap {
    uint64_t fm_start, fm_length;
    uint32_t fm_flags, fm_mapped_extents, fm_extent_count, fm_reserved;
    struct fiemap_extent fm_extents[];
};
#define LKFS_FIEMAP_SYNC 1
#define LKFS_FIEMAP_LAST 1
#define LKFS_FALLOC_KEEP_SIZE 1
_Static_assert(sizeof(struct fiemap) == 32, "FIEMAP header ABI");
_Static_assert(sizeof(struct fiemap_extent) == 56, "FIEMAP extent ABI");
#endif
