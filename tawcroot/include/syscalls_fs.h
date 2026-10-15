/* Filesystem syscall handlers — phase 1. See syscalls_fs.c. */

#pragma once

#include "path.h"

/* Register the path-bearing fs handler set in the dispatch table.
 * Called from tawcroot_dispatch_init. */
void tawcroot_fs_register(void);

/* Pure openat/openat2 flags→intent classifier for the read-only-bind
 * check (unit-tested; see path.h tawcroot_path_intent). Write iff the
 * accmode is not O_RDONLY (accmode 3 fails closed), or O_TRUNC, or
 * O_CREAT. O_PATH opens are reads regardless (kernel-faithful — an
 * O_PATH fd can't write; the kernel ignores the other flags).
 * O_APPEND alone is NOT write intent (the kernel allows
 * O_RDONLY|O_APPEND on an RO fs; append only matters on write-mode
 * fds, which RO binds never grant). */
tawcroot_path_intent tawcroot_openat_intent(int flags);

struct stat;
struct statx;

/* statx(2) with raw-syscall semantics on every kernel. statx is 4.11+
 * (many Android 11 phones run 4.9): the first ENOSYS latches
 * tawcroot_no_statx and from then on the call is newfstatat +
 * tawcroot_stat_to_statx — basic stats only, no btime, mount id or
 * attributes. The latch is per process image, so it is re-learned
 * after every exec. Use this, never a raw TAWC_SYS_statx. */
long tawc_statx(int dirfd, const char *path, int flags,
		unsigned int mask, struct statx *out);
extern int tawcroot_no_statx;

/* The kernel's cp_statx shape from a newfstatat result: struct zeroed,
 * stx_mask = STATX_BASIC_STATS, st_dev/st_rdev split into major/minor.
 * Pure; unit-tested. */
void tawcroot_stat_to_statx(const struct stat *st, struct statx *sx);

/* STATX_MNT_ID emulation for pre-5.8 kernels (see syscalls_stat.c for
 * the full story): when `req_mask` asks for a mount id and `sx` lacks
 * one, fill stx_mnt_id from /proc/self/fdinfo of the target at
 * (dirfd, path) — empty/NULL path means dirfd itself. Best-effort;
 * on failure `sx` is unchanged. Also used by the shm statx
 * synthesizers and the hosted parser test. */
void tawcroot_statx_fill_mnt_id(int dirfd, const char *path,
				unsigned int req_mask, struct statx *sx);
