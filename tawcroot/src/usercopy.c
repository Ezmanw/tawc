/* See include/usercopy.h. */

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

#include "errno_neg.h"
#include "tawc_string.h"
#include "usercopy.h"
#include "raw_sys.h"
#include "sysnr.h"

int tawc_usercopy_works = 0;

/* The process_vm_{read,write}v helpers need the *current* task id to
 * address our own VM. We can't cache it: after `fork(2)` the
 * child inherits parent globals but lives in a different process,
 * with a fresh tid that doesn't match the parent's. Same with
 * thread creation. Recompute on every call — gettid is a cheap
 * vDSO-eligible syscall (~50ns through our raw stub) and the
 * SIGSYS handler is already on the slow path. (Discovered when
 * bash's fork+execve dance returned -EFAULT through our exec
 * handler — child's process_vm_readv was reading parent VM.) */
static long current_task_id(void)
{
	long t = TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	return t > 0 ? t : 0;
}

long tawc_usercopy_init(void)
{
	/* Probe: copy 1 byte from a known-good local address into a local
	 * buffer through process_vm_readv. If the kernel honors it, mark
	 * the helpers usable. */
	char src = 'x';
	char dst = 0;
	struct iovec liov = { .iov_base = &dst, .iov_len = 1 };
	struct iovec riov = { .iov_base = &src, .iov_len = 1 };
	long rv = TAWC_RAW(TAWC_SYS_process_vm_readv, current_task_id(),
			   (long)&liov, 1, (long)&riov, 1, 0);
	if (rv == 1 && dst == 'x') {
		tawc_usercopy_works = 1;
		return 0;
	}
	return rv < 0 ? rv : TAWC_ENOSYS;
}

/* Read up to `len` bytes from a guest address. Returns bytes read or
 * -errno.
 *
 * If process_vm_readv was unreachable at init (probe failed) we return
 * -EFAULT loudly rather than fall back to a bare memcpy. The probe must
 * succeed on every target we ship to (Linux 3.2+ has the syscall; SELinux
 * lets a process readv itself); a probe failure is a deployment problem,
 * not a runtime fallback condition. (Review finding B7.) */
static long readv_guest(void *dst, size_t len, const void *src)
{
	if (!tawc_usercopy_works) return TAWC_EFAULT;
	struct iovec liov = { .iov_base = dst,        .iov_len = len };
	struct iovec riov = { .iov_base = (void *)src, .iov_len = len };
	long rv = TAWC_RAW(TAWC_SYS_process_vm_readv, current_task_id(),
			   (long)&liov, 1, (long)&riov, 1, 0);
	return rv;
}

/* Bytes from `p` to the end of its 4 KiB page. A readable byte means a
 * readable page (also on 16 KiB-page kernels, whose pages are whole
 * multiples of this), so reads bounded this way never fail part-way. */
static size_t to_page_end(const void *p)
{
	return TAWC_GUEST_PAGE - ((uintptr_t)p & (TAWC_GUEST_PAGE - 1));
}

long tawc_copy_string_from_guest(char *dst, size_t cap,
				 const char *guest_src)
{
	if (!dst || cap == 0) return TAWC_EFAULT;
	if (!guest_src) return TAWC_EFAULT;

	/* One read per page: over-reading past the NUL within the same
	 * page is safe, crossing into the next page is not. */
	size_t off = 0;
	while (off < cap) {
		size_t want = cap - off;
		size_t page = to_page_end(guest_src + off);
		if (want > page) want = page;
		long got = readv_guest(dst + off, want, guest_src + off);
		if (got <= 0) return TAWC_EFAULT;
		for (long i = 0; i < got; i++) {
			if (dst[off + i] == 0) {
				return (long)(off + i);  /* len excl NUL */
			}
		}
		off += (size_t)got;
	}
	return TAWC_ENAMETOOLONG;
}

/* Point the cache at the page holding `p`. */
static long reader_fill(struct tawc_guest_reader *r, const void *p)
{
	uintptr_t base = (uintptr_t)p & ~(uintptr_t)(TAWC_GUEST_PAGE - 1);
	r->len = 0;
	long got = readv_guest(r->page, TAWC_GUEST_PAGE, (const void *)base);
	if (got <= 0) return TAWC_EFAULT;
	r->base = base;
	r->len  = (size_t)got;
	return 0;
}

/* Cached bytes available at `p` (0 = miss). */
static size_t reader_avail(const struct tawc_guest_reader *r, uintptr_t p)
{
	if (p < r->base || p - r->base >= r->len) return 0;
	return r->len - (p - r->base);
}

long tawc_guest_read(struct tawc_guest_reader *r, void *dst, size_t n,
		     const void *guest_src)
{
	if (!guest_src) return TAWC_EFAULT;
	unsigned char *d = (unsigned char *)dst;
	uintptr_t p = (uintptr_t)guest_src;
	while (n > 0) {
		size_t av = reader_avail(r, p);
		if (av == 0) {
			if (reader_fill(r, (const void *)p) < 0) return TAWC_EFAULT;
			av = reader_avail(r, p);
		}
		if (av > n) av = n;
		memcpy(d, r->page + (p - r->base), av);
		d += av; p += av; n -= av;
	}
	return 0;
}

long tawc_guest_read_string(struct tawc_guest_reader *r, char *dst,
			    size_t cap, const char *guest_src)
{
	if (!guest_src) return TAWC_EFAULT;
	uintptr_t p = (uintptr_t)guest_src;
	size_t off = 0;
	while (off < cap) {
		size_t av = reader_avail(r, p);
		if (av == 0) {
			if (reader_fill(r, (const void *)p) < 0) return TAWC_EFAULT;
			av = reader_avail(r, p);
		}
		if (av > cap - off) av = cap - off;
		const unsigned char *src = r->page + (p - r->base);
		for (size_t i = 0; i < av; i++) {
			dst[off + i] = (char)src[i];
			if (src[i] == 0) return (long)(off + i);
		}
		off += av; p += av;
	}
	return TAWC_ENAMETOOLONG;
}

long tawc_copy_from_guest(void *dst, size_t n, const void *guest_src)
{
	if (!dst || !guest_src) return TAWC_EFAULT;
	long got = readv_guest(dst, n, guest_src);
	if (got < 0) return TAWC_EFAULT;
	if ((size_t)got != n) return TAWC_EFAULT;
	return 0;
}

long tawc_copy_to_guest(void *guest_dst, const void *src, size_t n)
{
	/* Even under the shared-VM model (guest manual-loaded into our own
	 * address space), a bare deref is unsafe: the guest may pass a NULL
	 * or unmapped output pointer and the kernel's contract is to return
	 * -EFAULT, not deliver SIGSEGV synchronously into our SIGSYS handler.
	 * Route through process_vm_writev against our own task id — the kernel
	 * validates the destination and reports -EFAULT cleanly. (Reviewed
	 * findings B1+B6.) */
	if (!guest_dst || !src) return TAWC_EFAULT;
	if (n == 0) return 0;
	if (!tawc_usercopy_works) return TAWC_EFAULT;
	struct iovec liov = { .iov_base = (void *)src,    .iov_len = n };
	struct iovec riov = { .iov_base = guest_dst,      .iov_len = n };
	long rv = TAWC_RAW(TAWC_SYS_process_vm_writev, current_task_id(),
			   (long)&liov, 1, (long)&riov, 1, 0);
	if (rv < 0)        return TAWC_EFAULT;
	if ((size_t)rv != n) return TAWC_EFAULT;
	return 0;
}
