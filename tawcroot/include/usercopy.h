/* Guarded guest-memory access.
 *
 * Even though the guest shares our address space (we manual-loaded it
 * into our own VM), guest pointers are still untrusted syscall args:
 * a guest may pass a NULL or wild pointer to openat(), and the kernel's
 * contract is to return -EFAULT. A bare deref from the SIGSYS handler
 * would deliver SIGSEGV synchronously and tear down the process — so
 * we route through process_vm_readv against our own task id. The kernel
 * validates the address and returns -EFAULT without raising a signal.
 *
 * Two flavors:
 *   tawc_copy_string_from_guest — copies a NUL-terminated string,
 *     bounded by `cap` bytes. Returns the length on success (excluding
 *     NUL) or -errno. -EFAULT for invalid pointers, -ENAMETOOLONG if
 *     the cap was exhausted before NUL.
 *   tawc_copy_from_guest — copies `n` raw bytes; returns 0 / -errno.
 *
 * If process_vm_readv was unreachable at init the helpers refuse
 * subsequent calls with -EFAULT. We do NOT fall back to a bare deref
 * — that would silently re-introduce the SIGSEGV-in-handler hazard.
 * The probe must succeed on every target we ship to.
 *
 * Async-signal-safe: the helpers issue raw syscalls through the stub
 * and use only stack-local iovecs.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

long tawc_copy_string_from_guest(char *dst, size_t cap,
				 const char *guest_src);
long tawc_copy_from_guest(void *dst, size_t n, const void *guest_src);
long tawc_copy_to_guest(void *guest_dst, const void *src, size_t n);

/* Page-cached guest reader for bulk copies (exec argv/envp walks):
 * reads a whole 4 KiB page per process_vm_readv and serves later reads
 * that land in the same page from the cache, so a long argv packed
 * into a few pages costs a few syscalls instead of one per string.
 * Caller owns the struct; zero `len` (or zero-init) to reset. Same
 * return contract as the copy helpers above. */
#define TAWC_GUEST_PAGE 4096
struct tawc_guest_reader {
	uintptr_t     base;
	size_t        len;
	unsigned char page[TAWC_GUEST_PAGE];
};
long tawc_guest_read(struct tawc_guest_reader *r, void *dst, size_t n,
		     const void *guest_src);
long tawc_guest_read_string(struct tawc_guest_reader *r, char *dst,
			    size_t cap, const char *guest_src);

/* Init: probe process_vm_readv against our own task id; sets a static flag
 * tawc_usercopy_works. Call once at startup before installing the
 * filter. Returns 0 on success / -errno. */
long tawc_usercopy_init(void);

extern int tawc_usercopy_works;
