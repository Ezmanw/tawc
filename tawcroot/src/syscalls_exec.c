/* SIGSYS dispatch handlers for `execve` / `execveat`.
 *
 * Wires the guest's exec syscalls into exec_handler.h's prepare
 * (build an exec_state memfd, under exec_lock) + commit (re-exec
 * tawcroot with --exec-child, after unlock) pair.
 *
 * The handlers themselves are small adapters: they copy the path out
 * of guest memory via usercopy (so a wild guest pointer returns -EFAULT
 * instead of faulting the supervisor) and hand the guest's argv/envp
 * arrays to prepare, which reads them the same way.
 *
 * argv/envp limits are the kernel's (exec_handler.h); the path is
 * copied with the kernel's PATH_MAX rule (ENAMETOOLONG at 4096 bytes).
 *
 *   - execveat's AT_SYMLINK_NOFOLLOW flag is intentionally not emulated
 *     yet; callers get -ENOSYS rather than a silently-followed symlink.
 *     See notes/tawcroot/status.md §"Accepted syscall-fidelity divergences".
 */

#include <stddef.h>
#include <stdint.h>

#include "arch.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "exec_handler.h"
#include "io.h"
#include "path.h"
#include "raw_sys.h"
#include "rescue.h"
#include "syscalls_exec.h"
#include "tawc_uapi.h"
#include "usercopy.h"

/* Exec path buffers. The kernel's getname() rejects a path of
 * PATH_MAX bytes or more; a dirfd-joined path can legally be longer. */
#define EXEC_PATH_MAX 4096
#define RESOLVED_MAX  (16 * 1024)

/* The exec staging state (path_buf, guest_path/resolved below and
 * exec_handler.c's reader and bind/shm export buffers) is `static` —
 * it doesn't fit the handler stack budget. SIGSYS handlers run concurrently on
 * multiple guest threads (sa_mask only masks the trapping thread), and
 * a CLONE_VM child exec'ing while a sibling execs shares this address
 * space, so two concurrent execs could interleave writes into these
 * buffers. Serialize the collect→serialize phase with a spinlock.
 *
 * The lock MUST be released before the execveat commit point — hence
 * the prepare/commit split in exec_handler.h. An earlier revision held
 * it across the execveat ("the winner never unlocks, the address space
 * is replaced anyway"), which is wrong for posix_spawn children:
 * CLONE_VM|CLONE_VFORK shares the parent's memory, so the lock the
 * exec'ing child left set persisted in the parent — and in every later
 * fork snapshot — wedging the family's next exec in this spin forever
 * (Firefox: glxtest posix_spawn leaked the lock, the startup-crash
 * relaunch fork+execve then hung). commit() touches no shared statics,
 * so running it unlocked is safe. */
static volatile int g_exec_lock;

static void exec_lock(void)
{
	while (__atomic_test_and_set(&g_exec_lock, __ATOMIC_ACQUIRE)) {
		/* spin — exec is rare and terminal */
	}
}

static void exec_unlock(void)
{
	__atomic_clear(&g_exec_lock, __ATOMIC_RELEASE);
}

static long do_exec(const void *guest_path,
                    char *const *guest_argv, char *const *guest_envp,
                    struct tawcroot_exec_prep *prep)
{
	if (!guest_path) return TAWC_EFAULT;

	static char path_buf[EXEC_PATH_MAX];
	long pn = tawc_copy_string_from_guest(path_buf, sizeof path_buf,
	                                      (const char *)guest_path);
	if (pn < 0) return pn;
	return tawcroot_exec_handler_prepare(path_buf,
	                                     (const char *const *)guest_argv,
	                                     (const char *const *)guest_envp,
	                                     prep);
}

/* execve(path, argv, envp). Both x86_64 (NR 59) and aarch64 (NR 221)
 * have this syscall. The earlier "aarch64 has no execve, glibc uses
 * execveat" claim was wrong — aarch64 does have execve(2), and glibc
 * uses it for plain execve() without dirfd. Validated empirically on
 * Android 14 / kernel 5.4: bash's `exec /bin/true` goes through NR 221
 * (execve), not NR 281 (execveat), so untrapped execve was getting
 * killed by Android's stacked filter while we sat there waiting for
 * an execveat trap that would never come. */
static long handle_execve(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	struct tawcroot_exec_prep prep;
	exec_lock();
	long r = do_exec((const void *)args->a, (char *const *)args->b,
	                 (char *const *)args->c, &prep);
	exec_unlock();
	if (r < 0) return r;
	/* commit() returns only on failure; on success it execveats away —
	 * past the rescue wrapper, which would then never put back a mode
	 * it widened to get the binary open (a 4111 sudo). Restore first;
	 * no-op when nothing was widened. */
	tawcroot_rescue_restore();
	return tawcroot_exec_handler_commit(&prep);
}

/* execveat(dirfd, path, argv, envp, flags). Handles the common fexecve(3)
 * shape: execveat(fd, "", argv, envp, AT_EMPTY_PATH), plus dirfd-relative
 * non-empty paths that can be reverse-translated into the rootfs view. */
static long execveat_locked(const tawcroot_syscall_args *args,
                            struct tawcroot_exec_prep *prep)
{
	int dirfd = (int)args->a;
	int flags = (int)args->e;

	if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) return TAWC_EINVAL;
	if (flags & AT_SYMLINK_NOFOLLOW) return TAWC_ENOSYS;
	if (dirfd == AT_FDCWD)
		return do_exec((const void *)args->b, (char *const *)args->c,
		               (char *const *)args->d, prep);

	static char guest_path[EXEC_PATH_MAX];
	long pn = tawc_copy_string_from_guest(guest_path, sizeof guest_path,
	                                      (const char *)args->b);
	if (pn < 0) return pn;

	const char *const *argv = (const char *const *)args->c;
	const char *const *envp = (const char *const *)args->d;
	if (guest_path[0] == '/')
		return tawcroot_exec_handler_prepare(guest_path, argv, envp, prep);

	if (guest_path[0] == 0 && !(flags & AT_EMPTY_PATH))
		return TAWC_ENOENT;

	static char resolved[RESOLVED_MAX];
	long rn = tawcroot_fd_to_guest_abs(dirfd, resolved, sizeof resolved);
	if (rn < 0) return rn;

	if (guest_path[0] != 0) {
		size_t len = (size_t)rn;
		long ar = 0;
		if (len == 0) return TAWC_EINVAL;
		if (resolved[len - 1] != '/')
			ar = tawc_str_append(resolved, sizeof resolved,
			                     &len, "/");
		if (!ar) ar = tawc_str_append(resolved, sizeof resolved,
		                              &len, guest_path);
		if (ar < 0) return ar;
	}

	return tawcroot_exec_handler_prepare(resolved, argv, envp, prep);
}

static long handle_execveat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	struct tawcroot_exec_prep prep;
	exec_lock();
	long r = execveat_locked(args, &prep);
	exec_unlock();
	if (r < 0) return r;
	/* See handle_execve. */
	tawcroot_rescue_restore();
	return tawcroot_exec_handler_commit(&prep);
}

void tawcroot_exec_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_execve,   handle_execve);
	tawcroot_dispatch_install(TAWC_SYS_execveat, handle_execveat);
}
