/* exec_state — the binary blob the SIGSYS handler writes to a memfd
 * before re-execing tawcroot, and that --exec-child reads back to
 * resume the guest's `execve(2)` in the new tawcroot incarnation.
 *
 * =======================================================================
 *  WHY THIS EXISTS
 * =======================================================================
 *
 * `execve(2)` resets every signal handler to SIG_DFL on success — see
 * `man 2 execve`. Our SIGSYS handler does not survive a kernel exec.
 * If we let the guest's `execve` go through, the kernel-installed
 * seccomp filter is inherited (filters do persist) but our handler
 * is gone, so the guest's first path-bearing syscall delivers SIGSYS
 * with disposition SIG_DFL → the kernel kills the guest with code 31.
 *
 * Instead the handler intercepts `execve`, validates the binary,
 * writes its own state (guest path, argv, envp) into a non-CLOEXEC
 * memfd, then re-execs tawcroot itself with `--exec-child <fd>`.
 * The new tawcroot reads the memfd, reinstalls the SIGSYS handler
 * (the seccomp filter is already inherited), and uses the manual
 * loader to map the guest binary + jump into it. From the guest's
 * perspective this is indistinguishable from a normal execve.
 *
 * =======================================================================
 *  FORMAT
 * =======================================================================
 *
 * A fixed header (tawcroot_exec_state_header) followed by a string
 * pool. The pool starts with the guest path (offset 0), then the argv
 * strings back to back, then the envp strings back to back, then the
 * optional extras. argv and envp are stored as regions (offset + byte
 * length + count), not per-string offsets, so the header stays small
 * and there is no cap on argc/envc: readers rebuild the pointer arrays
 * by walking the NULs. Every string is NUL-terminated.
 *
 * Endianness: same-machine fd handoff, so host endianness throughout.
 * The writer and reader are the same binary (re-exec'd self), so there
 * is no back-compat: a version bump is enough.
 *
 * =======================================================================
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TAWCROOT_EXEC_STATE_MAGIC   0x4358454Cu  /* 'LEXC' = "Loader EXeC" */
/* Version 3: adds the /dev/shm name table (n_shm + shm_name_off[] +
 * shm_fd[]) so the in-handler memfd-backed `/dev/shm` survives the
 * --exec-child dance. The internal memfds are non-CLOEXEC and survive
 * the execveat; we ferry their (name, fd_int) pairs through here so
 * the new tawcroot incarnation can re-register them. v2 readers don't
 * exist outside the tree — both ends are the same binary — so we
 * don't maintain back-compat.
 * Version 4: adds the virtual identity snapshot (has_identity +
 * identity) so a guest that dropped privileges keeps its tracked
 * uid/gid across execve.
 * Version 5: adds the hardlink-emulation store path (store_host_off,
 * linkstore.h). Ferried explicitly — NOT re-derived from rootfs_host,
 * which after a guest chroot is the chrooted root, not the store's
 * sibling.
 * Version 6: adds per-bind read-only flags (bind_ro[]) and the
 * root-view RO bit (root_ro, set when the guest chrooted into an RO
 * bind dst) so RO enforcement survives a guest execve.
 * Version 7: adds the proctitle string (proctitle_off) — the space-
 * joined guest cmdline (plus slack for shebang expansion) that commit()
 * passes as argv[0] of the execveat-into-self, sizing the new process's
 * kernel arg region so the loader can later overwrite it in place with
 * the real NUL-joined argv (see proctitle.h). Without it every guest
 * process's /proc/<pid>/cmdline reads "tawcroot --exec-child <fd>" and
 * pgrep/pkill/ps can't identify anything by name.
 * Version 8: argv/envp become regions (no per-string offset arrays, no
 * argc/envc caps), and the proctitle string is replaced by
 * cmdline_bytes; commit() builds the title itself. */
#define TAWCROOT_EXEC_STATE_VERSION 8
/* Internal, not guest-facing: bind and /dev/shm tables keep small
 * fixed caps. */
#define TAWCROOT_EXEC_STATE_MAX_BINDS 16
#define TAWCROOT_EXEC_STATE_MAX_SHM   64

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t string_bytes;
	uint32_t path_off;
	uint32_t argc;
	uint32_t envc;
	/* argv / envp regions within strings: argc (envc) NUL-terminated
	 * strings back to back. bytes == 0 iff the count is 0. */
	uint32_t argv_off;
	uint32_t argv_bytes;
	uint32_t envp_off;
	uint32_t envp_bytes;
	/* NUL-joined byte length of the argv the loader will install (after
	 * shebang rewriting). commit() sizes the re-exec's kernel arg
	 * region to it (proctitle.h). 0 = unknown. */
	uint32_t cmdline_bytes;
	/* Phase 2g: tawcroot per-process state to re-establish in
	 * --exec-child. A 0 offset means "absent" (no rootfs view active /
	 * no guest_exe stash). Bind specs are paired src/dst pointers
	 * indexed by [0..n_binds). When n_binds == 0, the bind arrays are
	 * meaningless. */
	uint32_t rootfs_host_off;
	uint32_t guest_exe_off;
	uint32_t store_host_off;   /* hardlink-emulation store; 0 = absent */
	uint32_t n_binds;
	uint32_t bind_src_off[TAWCROOT_EXEC_STATE_MAX_BINDS];
	uint32_t bind_dst_off[TAWCROOT_EXEC_STATE_MAX_BINDS];
	/* v6: per-bind read-only flags (parallel to bind_*_off) and the
	 * root-view RO bit (path.h tawcroot_root_ro). */
	uint8_t  bind_ro[TAWCROOT_EXEC_STATE_MAX_BINDS];
	uint32_t root_ro;
	/* /dev/shm name table. fd numbers are inherited verbatim because
	 * the internal memfds are non-CLOEXEC and survive execveat. */
	uint32_t n_shm;
	uint32_t shm_name_off[TAWCROOT_EXEC_STATE_MAX_SHM];
	uint32_t shm_fd[TAWCROOT_EXEC_STATE_MAX_SHM];
	/* Virtual identity snapshot (identity.c). Fixed-size value, not
	 * strings — embedded directly. has_identity == 0 means "absent"
	 * (restore keeps the register-time root defaults). */
	uint32_t      has_identity;
	tawc_identity identity;
} tawcroot_exec_state_header;

/* Result of unpacking. Pointers reference the caller's buffer
 * (typically the mmap'd memfd) and are valid as long as it is.
 * argv/envp are regions; tawcroot_exec_state_split builds pointer
 * arrays from them. */
typedef struct {
	const char  *path;
	uint32_t     argc;
	uint32_t     envc;
	const char  *argv_strings;   /* argc strings back to back */
	size_t       argv_bytes;
	const char  *envp_strings;   /* envc strings back to back */
	size_t       envp_bytes;
	uint32_t     cmdline_bytes;

	/* Phase 2g: per-process state. NULL when absent (--exec-via-handler
	 * test path doesn't carry a rootfs view; production -r mode does). */
	const char  *rootfs_host;       /* may be NULL */
	const char  *guest_exe;         /* may be NULL */
	const char  *store_host;        /* may be NULL */
	uint32_t     n_binds;
	const char  *bind_src[TAWCROOT_EXEC_STATE_MAX_BINDS];
	const char  *bind_dst[TAWCROOT_EXEC_STATE_MAX_BINDS];
	unsigned char bind_ro[TAWCROOT_EXEC_STATE_MAX_BINDS];
	uint32_t     root_ro;
	uint32_t     n_shm;
	const char  *shm_name[TAWCROOT_EXEC_STATE_MAX_SHM];
	int          shm_fd[TAWCROOT_EXEC_STATE_MAX_SHM];
	int           has_identity;
	tawc_identity identity;      /* valid iff has_identity */
} tawcroot_exec_state;

/* Optional inputs for the writer — may all be NULL/0 to indicate "no
 * tawcroot per-process state to carry". */
typedef struct {
	const char        *rootfs_host;     /* may be NULL */
	const char        *guest_exe;       /* may be NULL */
	const char        *store_host;      /* may be NULL */
	uint32_t           n_binds;
	const char *const *bind_src;        /* size n_binds */
	const char *const *bind_dst;        /* size n_binds */
	const unsigned char *bind_ro;       /* size n_binds; NULL = all RW */
	uint32_t           root_ro;
	uint32_t           n_shm;
	const char *const *shm_name;        /* size n_shm */
	const int         *shm_fd;          /* size n_shm */
	const tawc_identity *identity;      /* may be NULL */
} tawcroot_exec_state_extras;

/* ---- Writer ----
 *
 * The exec handler collects guest strings straight into the memfd
 * mapping, so writing is split into steps over a caller-owned buffer
 * `buf` of `cap` bytes; `*len` is the bytes used so far (header
 * included). All steps return 0 or -ENOSPC when `cap` is too small
 * (nothing is written past `cap`; the caller grows and retries).
 *
 *   tawcroot_exec_state_begin   header + path; *len = header + path
 *   (append argc strings, then envc strings, at buf + *len — directly
 *    or via tawcroot_exec_state_append_str — and record them with
 *    tawcroot_exec_state_set_args)
 *   tawcroot_exec_state_finish  extras; sets string_bytes
 *
 * tawcroot_exec_state_write does all of it from pointer arrays. */
long tawcroot_exec_state_begin(void *buf, size_t cap, size_t *len,
                               const char *path);
long tawcroot_exec_state_append_str(void *buf, size_t cap, size_t *len,
                                    const char *s);
/* Record argv as the argc strings at [argv_off, argv_off+argv_bytes)
 * and envp as the envc strings right after them (offsets relative to
 * the start of `buf`). */
void tawcroot_exec_state_set_args(void *buf, size_t argv_off,
                                  uint32_t argc, size_t argv_bytes,
                                  uint32_t envc, size_t envp_bytes,
                                  uint32_t cmdline_bytes);
long tawcroot_exec_state_finish(void *buf, size_t cap, size_t *len,
                                const tawcroot_exec_state_extras *extras);

/* One-shot writer. argv/envp are NULL-terminated (argv[argc] == NULL).
 * Returns the total bytes written or -errno (-EINVAL bad shape,
 * -E2BIG too many binds/shm entries, -ENOSPC buf_cap too small). */
long tawcroot_exec_state_write(void *buf, size_t buf_cap,
                               const char *path,
                               int argc, const char *const *argv,
                               const char *const *envp,
                               const tawcroot_exec_state_extras *extras);

/* Exact bytes tawcroot_exec_state_write needs. */
size_t tawcroot_exec_state_estimate_bytes(const char *path,
                                          int argc, const char *const *argv,
                                          const char *const *envp,
                                          const tawcroot_exec_state_extras *extras);

/* Bytes tawcroot_exec_state_finish appends for `extras`. */
size_t tawcroot_exec_state_extras_bytes(const tawcroot_exec_state_extras *extras);

/* ---- Reader (--exec-child side) ----
 *
 * Parse a serialized exec_state out of `buf` (the mmap'd memfd
 * contents). Returns 0, or -EINVAL on bad magic / version / sizes /
 * offsets, a region not ending in NUL, or a count that doesn't match
 * the region's NULs. */
long tawcroot_exec_state_read(const void *buf, size_t buf_size,
                              tawcroot_exec_state *out);

/* Fill ptrs[0..n) with the n NUL-terminated strings at `region` and
 * set ptrs[n] = NULL. `region` must hold n strings (as validated by
 * the reader). */
void tawcroot_exec_state_split(const char *region, uint32_t n,
                               const char **ptrs);

#ifdef __cplusplus
}
#endif
