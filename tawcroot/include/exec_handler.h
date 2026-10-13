/* SIGSYS-handler-side execve interception.
 *
 * Catches the guest's `execve` / `execveat`, validates the binary,
 * writes the request into a memfd, and re-execs tawcroot itself with
 * `--exec-child <fd>`. From the guest's point of view the syscall
 * succeeds and a new program is running, exactly like a kernel-
 * driven execve — except the new tawcroot process inherits our
 * SIGSYS handler chain (we re-install it in `--exec-child` after
 * the kernel-execve resets handlers to SIG_DFL).
 *
 * Why we can't let the guest's execve go through:
 * - The kernel resets every signal handler to SIG_DFL on a successful
 *   exec (`man 2 execve`). The seccomp filter is preserved (filters
 *   ride along), so the new program runs with our trap set installed
 *   but no SIGSYS handler.
 * - The new program's first path-bearing syscall traps to SIGSYS
 *   with disposition SIG_DFL → kernel kills the program.
 *
 * This file's contract is two phases:
 *
 *   `tawcroot_exec_handler_prepare(path, argv, envp, &prep)`
 *     - path: the UNTRANSLATED guest path (already copied out of guest
 *       memory). prepare translates it for the probe, and the
 *       exec-child re-translates it after re-exec.
 *     - argv / envp: the guest's pointer arrays, still in guest memory
 *       (NULL = empty list). Read through usercopy.
 *     - Mirrors execve(2)'s checks and errno order: open + probe the
 *       file (ENOENT, EACCES, EISDIR, ...), then argv/envp against the
 *       kernel's limits (E2BIG, EFAULT), then the binary format
 *       (ENOEXEC, shebang interpreter errors, post-shebang E2BIG).
 *     - Collects the strings straight into a non-CLOEXEC memfd mapping
 *       that grows on demand, and fills `prep`.
 *     - Returns 0 or -errno. Stages through static buffers — callers
 *       hold syscalls_exec.c's exec_lock.
 *
 *   `tawcroot_exec_handler_commit(&prep)`
 *     - Opens /proc/self/exe, execveat-s self with
 *       `--exec-child <fd>`. Touches no shared statics — callers must
 *       have RELEASED exec_lock first (see below).
 *     - On success: never returns.
 *     - On failure: releases `prep` and returns -errno.
 *
 * Limits follow fs/exec.c: each string at most MAX_ARG_STRLEN (32
 * pages) including its NUL; the pointers ((max(argc,1) + envc) * 8)
 * plus every string (path, envp, argv) within
 * max(min(6 MiB, RLIMIT_STACK / 4), 128 KiB), re-checked after shebang
 * rewriting. No tawcroot-specific caps.
 *
 * prepare stages through a few static buffers (guest readers, bind/shm
 * export tables); a process-global spinlock in syscalls_exec.c
 * (exec_lock) serializes it so concurrent execs — or a CLONE_VM child
 * exec'ing while the parent execs — can't interleave.
 *
 * The phase split exists because the lock must be RELEASED before the
 * execveat commit point. A posix_spawn child (CLONE_VM|CLONE_VFORK)
 * shares the parent's address space; a lock still held when the child
 * execveats away is leaked set in the PARENT's memory (and via fork
 * snapshots into every later child), so the next exec anywhere in the
 * family spins forever. Firefox hit exactly this: glxtest is
 * posix_spawn'd, then the startup-crash relaunch fork+execve hung.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A prepared exec: the state memfd and prepare's mapping of it. */
struct tawcroot_exec_prep {
	int     mfd;
	void   *map;      /* MAP_SHARED, map_cap bytes */
	size_t  map_cap;
	size_t  len;      /* state bytes (= memfd size) */
};

/* Phase 1: validate the target and serialize the exec request. See the
 * module header. */
long tawcroot_exec_handler_prepare(const char *path,
                                   const char *const *argv,
                                   const char *const *envp,
                                   struct tawcroot_exec_prep *prep);

/* Phase 2: execveat self as `--exec-child <mfd>`. Never returns on
 * success. On failure releases `prep` and returns -errno (-ENOEXEC when
 * /proc/self/exe couldn't be opened, else the execveat errno). */
long tawcroot_exec_handler_commit(struct tawcroot_exec_prep *prep);

/* prepare + commit for callers outside the SIGSYS path (testhost
 * --exec-via-handler), with argv/envp in our own memory. Never returns
 * 0. */
long tawcroot_exec_handler_perform(const char *path,
                                   const char *const *argv,
                                   const char *const *envp);

#ifdef __cplusplus
}
#endif
