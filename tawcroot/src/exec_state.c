/* exec_state writer + reader. See include/exec_state.h.
 *
 * Pure data construction — no syscalls. Can be unit-tested from
 * cleat by feeding any bag of bytes through the round trip.
 */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "exec_state.h"
#include "tawc_string.h"

#define HDR sizeof(tawcroot_exec_state_header)

/* Optional strings use offset 0 as "absent": the path is always the
 * first string, at offset 0, so no other string can sit there. */

static size_t opt_len(const char *s)
{ return s ? strlen(s) + 1 : 0; }

size_t tawcroot_exec_state_extras_bytes(const tawcroot_exec_state_extras *ex)
{
	if (!ex) return 0;
	size_t s = opt_len(ex->rootfs_host) + opt_len(ex->guest_exe) +
	           opt_len(ex->store_host);
	for (uint32_t i = 0; i < ex->n_binds; i++) {
		if (ex->bind_src) s += opt_len(ex->bind_src[i]);
		if (ex->bind_dst) s += opt_len(ex->bind_dst[i]);
	}
	for (uint32_t i = 0; i < ex->n_shm; i++)
		if (ex->shm_name) s += opt_len(ex->shm_name[i]);
	return s;
}

size_t tawcroot_exec_state_estimate_bytes(const char *path,
                                          int argc, const char *const *argv,
                                          const char *const *envp,
                                          const tawcroot_exec_state_extras *ex)
{
	size_t s = HDR + strlen(path) + 1;
	/* Stops at an embedded NULL so a caller that got argc wrong still
	 * gets a (short) answer rather than a fault; the writer rejects
	 * that shape outright. */
	for (int i = 0; i < argc && argv[i]; i++) s += strlen(argv[i]) + 1;
	for (int i = 0; envp[i]; i++) s += strlen(envp[i]) + 1;
	return s + tawcroot_exec_state_extras_bytes(ex);
}

long tawcroot_exec_state_append_str(void *buf, size_t cap, size_t *len,
                                    const char *str)
{
	size_t n = strlen(str) + 1;
	if (*len > cap || cap - *len < n) return TAWC_ENOSPC;
	memcpy((uint8_t *)buf + *len, str, n);
	*len += n;
	return 0;
}

long tawcroot_exec_state_begin(void *buf, size_t cap, size_t *len,
                               const char *path)
{
	if (!buf || !path) return TAWC_EINVAL;
	if (cap < HDR) return TAWC_ENOSPC;
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	memset(h, 0, sizeof *h);
	h->magic   = TAWCROOT_EXEC_STATE_MAGIC;
	h->version = TAWCROOT_EXEC_STATE_VERSION;
	*len = HDR;
	return tawcroot_exec_state_append_str(buf, cap, len, path);
}

void tawcroot_exec_state_set_args(void *buf, size_t argv_off,
                                  uint32_t argc, size_t argv_bytes,
                                  uint32_t envc, size_t envp_bytes,
                                  uint32_t cmdline_bytes)
{
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	h->argc          = argc;
	h->envc          = envc;
	h->argv_off      = (uint32_t)(argv_off - HDR);
	h->argv_bytes    = (uint32_t)argv_bytes;
	h->envp_off      = (uint32_t)(argv_off + argv_bytes - HDR);
	h->envp_bytes    = (uint32_t)envp_bytes;
	h->cmdline_bytes = cmdline_bytes;
}

/* Append an optional string; returns its strings-relative offset, or 0
 * (absent) for NULL. Capacity was checked up front by finish. */
static uint32_t emit_opt(uint8_t *base, size_t *len, const char *str)
{
	if (!str) return 0;
	uint32_t at = (uint32_t)(*len - HDR);
	size_t n = strlen(str) + 1;
	memcpy(base + *len, str, n);
	*len += n;
	return at;
}

long tawcroot_exec_state_finish(void *buf, size_t cap, size_t *len,
                                const tawcroot_exec_state_extras *ex)
{
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	if (ex && ex->n_binds > TAWCROOT_EXEC_STATE_MAX_BINDS) return TAWC_E2BIG;
	if (ex && ex->n_shm   > TAWCROOT_EXEC_STATE_MAX_SHM)   return TAWC_E2BIG;
	/* shm names are required (the reader has no absent sentinel for
	 * them); reject NULL rather than alias the path at offset 0. */
	if (ex)
		for (uint32_t i = 0; i < ex->n_shm; i++)
			if (!ex->shm_name || !ex->shm_name[i]) return TAWC_EINVAL;
	size_t need = tawcroot_exec_state_extras_bytes(ex);
	if (*len > cap || cap - *len < need) return TAWC_ENOSPC;

	uint8_t *base = (uint8_t *)buf;
	if (ex) {
		h->rootfs_host_off = emit_opt(base, len, ex->rootfs_host);
		h->guest_exe_off   = emit_opt(base, len, ex->guest_exe);
		h->store_host_off  = emit_opt(base, len, ex->store_host);
		h->n_binds = ex->n_binds;
		for (uint32_t i = 0; i < ex->n_binds; i++) {
			if (ex->bind_src)
				h->bind_src_off[i] = emit_opt(base, len, ex->bind_src[i]);
			if (ex->bind_dst)
				h->bind_dst_off[i] = emit_opt(base, len, ex->bind_dst[i]);
			h->bind_ro[i] = ex->bind_ro ? ex->bind_ro[i] : 0;
		}
		h->root_ro = ex->root_ro ? 1 : 0;
		h->n_shm = ex->n_shm;
		for (uint32_t i = 0; i < ex->n_shm; i++) {
			h->shm_name_off[i] = emit_opt(base, len, ex->shm_name[i]);
			h->shm_fd[i] = (uint32_t)ex->shm_fd[i];
		}
		if (ex->identity) {
			h->has_identity = 1;
			h->identity     = *ex->identity;
		}
	}
	h->string_bytes = (uint32_t)(*len - HDR);
	return 0;
}

long tawcroot_exec_state_write(void *buf, size_t buf_cap,
                               const char *path,
                               int argc, const char *const *argv,
                               const char *const *envp,
                               const tawcroot_exec_state_extras *ex)
{
	if (!buf || !path || !argv || !envp || argc < 0) return TAWC_EINVAL;
	/* argv[0..argc) must be fully populated (estimate_bytes stops at
	 * an embedded NULL; the copy below would deref it). */
	for (int i = 0; i < argc; i++)
		if (!argv[i]) return TAWC_EINVAL;

	size_t len;
	long r = tawcroot_exec_state_begin(buf, buf_cap, &len, path);
	if (r < 0) return r;
	size_t argv_off = len;
	for (int i = 0; i < argc; i++) {
		r = tawcroot_exec_state_append_str(buf, buf_cap, &len, argv[i]);
		if (r < 0) return r;
	}
	size_t envp_off = len;
	uint32_t envc = 0;
	for (; envp[envc]; envc++) {
		r = tawcroot_exec_state_append_str(buf, buf_cap, &len, envp[envc]);
		if (r < 0) return r;
	}
	tawcroot_exec_state_set_args(buf, argv_off, (uint32_t)argc,
	                             envp_off - argv_off, envc,
	                             len - envp_off,
	                             /* argc 0 runs as argv = {""} */
	                             (uint32_t)(argc > 0 ? envp_off - argv_off : 1));
	r = tawcroot_exec_state_finish(buf, buf_cap, &len, ex);
	if (r < 0) return r;
	return (long)len;
}

/* Region [off, off+bytes) within `n_strings` bytes must hold exactly
 * `count` NUL-terminated strings. */
static int region_ok(const char *strings, uint32_t n_strings,
                     uint32_t off, uint32_t bytes, uint32_t count)
{
	if (off > n_strings || bytes > n_strings - off) return 0;
	if (bytes == 0) return count == 0;
	const char *p = strings + off;
	if (p[bytes - 1] != 0) return 0;
	uint32_t nuls = 0;
	for (uint32_t i = 0; i < bytes; i++) nuls += p[i] == 0;
	return nuls == count;
}

/* A NUL-terminated string starts at `off`. */
static int str_ok(const char *strings, uint32_t n_strings, uint32_t off)
{
	if (off >= n_strings) return 0;
	const char *p = strings + off, *end = strings + n_strings;
	while (p < end && *p) p++;
	return p < end;
}

long tawcroot_exec_state_read(const void *buf, size_t buf_size,
                              tawcroot_exec_state *out)
{
	if (!buf || !out) return TAWC_EINVAL;
	if (buf_size < HDR) return TAWC_EINVAL;

	const tawcroot_exec_state_header *h =
	    (const tawcroot_exec_state_header *)buf;
	if (h->magic != TAWCROOT_EXEC_STATE_MAGIC) return TAWC_EINVAL;
	if (h->version != TAWCROOT_EXEC_STATE_VERSION) return TAWC_EINVAL;
	if (h->n_binds > TAWCROOT_EXEC_STATE_MAX_BINDS) return TAWC_EINVAL;
	if (h->n_shm   > TAWCROOT_EXEC_STATE_MAX_SHM)   return TAWC_EINVAL;
	if (buf_size - HDR < h->string_bytes) return TAWC_EINVAL;
	if (h->has_identity && h->identity.ngroups > TAWC_IDENTITY_NGROUPS)
		return TAWC_EINVAL;

	const char *strings = (const char *)buf + HDR;
	uint32_t n = h->string_bytes;

	#define REQ(off) do { if (!str_ok(strings, n, (off))) return TAWC_EINVAL; } while (0)
	#define OPT(off) do { if ((off) != 0) REQ(off); } while (0)
	if (h->path_off != 0) return TAWC_EINVAL;
	REQ(h->path_off);
	if (!region_ok(strings, n, h->argv_off, h->argv_bytes, h->argc) ||
	    !region_ok(strings, n, h->envp_off, h->envp_bytes, h->envc))
		return TAWC_EINVAL;
	OPT(h->rootfs_host_off);
	OPT(h->guest_exe_off);
	OPT(h->store_host_off);
	for (uint32_t i = 0; i < h->n_binds; i++) {
		OPT(h->bind_src_off[i]);
		OPT(h->bind_dst_off[i]);
	}
	for (uint32_t i = 0; i < h->n_shm; i++) REQ(h->shm_name_off[i]);
	#undef REQ
	#undef OPT

	out->path          = strings + h->path_off;
	out->argc          = h->argc;
	out->envc          = h->envc;
	out->argv_strings  = strings + h->argv_off;
	out->argv_bytes    = h->argv_bytes;
	out->envp_strings  = strings + h->envp_off;
	out->envp_bytes    = h->envp_bytes;
	out->cmdline_bytes = h->cmdline_bytes;

	#define OPTSTR(off) ((off) ? strings + (off) : (const char *)0)
	out->rootfs_host = OPTSTR(h->rootfs_host_off);
	out->guest_exe   = OPTSTR(h->guest_exe_off);
	out->store_host  = OPTSTR(h->store_host_off);
	out->n_binds     = h->n_binds;
	for (uint32_t i = 0; i < h->n_binds; i++) {
		out->bind_src[i] = OPTSTR(h->bind_src_off[i]);
		out->bind_dst[i] = OPTSTR(h->bind_dst_off[i]);
		out->bind_ro[i]  = h->bind_ro[i] ? 1 : 0;
	}
	#undef OPTSTR
	out->root_ro = h->root_ro ? 1 : 0;
	out->n_shm = h->n_shm;
	for (uint32_t i = 0; i < h->n_shm; i++) {
		out->shm_name[i] = strings + h->shm_name_off[i];
		out->shm_fd[i]   = (int)h->shm_fd[i];
	}
	out->has_identity = h->has_identity ? 1 : 0;
	if (h->has_identity) out->identity = h->identity;
	return 0;
}

void tawcroot_exec_state_split(const char *region, uint32_t n,
                               const char **ptrs)
{
	for (uint32_t i = 0; i < n; i++) {
		ptrs[i] = region;
		region += strlen(region) + 1;
	}
	ptrs[n] = (const char *)0;
}
