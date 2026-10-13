/* Round-trip tests for the exec_state memfd format
 * (tawcroot/src/exec_state.c).
 *
 * Pure data: writer + reader are libc-free; we exercise them under
 * hosted glibc and assert that what we write comes back identical.
 * The on-device usage path (handler writes → re-exec → child reads)
 * is integration-tested in tests/integration/test_exec_child.c.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "exec_state.h"

/* Pointer arrays for a read state (freed by the caller). */
static const char **split_argv(const tawcroot_exec_state *st)
{
	const char **v = malloc((st->argc + 1) * sizeof *v);
	tawcroot_exec_state_split(st->argv_strings, st->argc, v);
	return v;
}

static const char **split_envp(const tawcroot_exec_state *st)
{
	const char **v = malloc((st->envc + 1) * sizeof *v);
	tawcroot_exec_state_split(st->envp_strings, st->envc, v);
	return v;
}

/* Write a small valid state into buf; returns its size. */
static size_t small_state(uint8_t *buf, size_t cap)
{
	const char *argv[] = { "p", "q", NULL };
	const char *envp[] = { "E=1", NULL };
	long w = tawcroot_exec_state_write(buf, cap, "/p", 2, argv, envp, NULL);
	return w > 0 ? (size_t)w : 0;
}

test(exec_state_basic_roundtrip)
{
	const char *argv[] = { "/bin/true", "alpha", "beta", NULL };
	const char *envp[] = { "PATH=/bin", "HOME=/root", NULL };

	size_t need = tawcroot_exec_state_estimate_bytes("/bin/true", 3, argv, envp, NULL);
	uint8_t *buf = malloc(need);
	test_nonnull(buf);

	long w = tawcroot_exec_state_write(buf, need, "/bin/true", 3, argv, envp, NULL);
	test_int_eq((int)w, (int)need);

	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, &out), 0);

	test_str_eq(out.path, "/bin/true");
	test_int_eq((int)out.argc, 3);
	test_int_eq((int)out.envc, 2);
	const char **av = split_argv(&out), **ev = split_envp(&out);
	test_str_eq(av[0], "/bin/true");
	test_str_eq(av[1], "alpha");
	test_str_eq(av[2], "beta");
	test_ptr_eq(av[3], NULL);
	test_str_eq(ev[0], "PATH=/bin");
	test_str_eq(ev[1], "HOME=/root");
	test_ptr_eq(ev[2], NULL);
	/* argv bytes: "/bin/true alpha beta" NUL-joined */
	test_int_eq((int)out.cmdline_bytes, 21);
	test_int_eq((int)out.argv_bytes, 21);
	test_ptr_eq(out.rootfs_host, NULL);
	free(av); free(ev);
	free(buf);
}

/* The handler builds the state in steps (strings land straight in
 * the memfd); the result must match the one-shot writer byte for
 * byte. */
test(exec_state_stepwise_matches_write)
{
	const char *argv[] = { "worker", "--flag", NULL };
	const char *envp[] = { "A=1", NULL };
	tawcroot_exec_state_extras ex = { 0 };
	ex.rootfs_host = "/host/root";

	size_t need = tawcroot_exec_state_estimate_bytes("/bin/worker", 2,
	                                                 argv, envp, &ex);
	uint8_t *one = calloc(1, need), *two = calloc(1, need);
	test_int_eq((int)tawcroot_exec_state_write(one, need, "/bin/worker",
	                                           2, argv, envp, &ex),
	            (int)need);

	size_t len;
	test_int_eq((int)tawcroot_exec_state_begin(two, need, &len,
	                                           "/bin/worker"), 0);
	size_t argv_off = len;
	test_int_eq((int)tawcroot_exec_state_append_str(two, need, &len, "worker"), 0);
	test_int_eq((int)tawcroot_exec_state_append_str(two, need, &len, "--flag"), 0);
	size_t envp_off = len;
	test_int_eq((int)tawcroot_exec_state_append_str(two, need, &len, "A=1"), 0);
	tawcroot_exec_state_set_args(two, argv_off, 2, envp_off - argv_off,
	                             1, len - envp_off,
	                             (uint32_t)(envp_off - argv_off));
	test_int_eq((int)tawcroot_exec_state_finish(two, need, &len, &ex), 0);
	test_int_eq((int)len, (int)need);
	test_true(memcmp(one, two, need) == 0);

	/* Steps refuse to run past cap instead of writing past it. */
	size_t l2 = need - 2;
	test_int_eq((int)tawcroot_exec_state_append_str(two, need, &l2, "xyz"),
	            -28 /* ENOSPC */);
	free(one); free(two);
}

test(exec_state_zero_argv_envp)
{
	const char *argv[] = { NULL };
	const char *envp[] = { NULL };
	size_t need = tawcroot_exec_state_estimate_bytes("/x", 0, argv, envp, NULL);
	uint8_t *buf = malloc(need);
	test_int_eq((int)tawcroot_exec_state_write(buf, need, "/x", 0, argv, envp, NULL),
	            (int)need);

	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, &out), 0);
	test_str_eq(out.path, "/x");
	test_int_eq((int)out.argc, 0);
	test_int_eq((int)out.envc, 0);
	test_int_eq((int)out.argv_bytes, 0);
	test_int_eq((int)out.cmdline_bytes, 1);  /* runs as argv = {""} */
	const char **av = split_argv(&out);
	test_ptr_eq(av[0], NULL);
	free(av);
	free(buf);
}

/* argc must match the array. estimate_bytes stops at an embedded NULL
 * while the writer's emit loop would deref it, so the writer rejects
 * the shape rather than under-sizing the buffer and faulting. */
test(exec_state_argc_past_null_argv_is_rejected)
{
	const char *argv[] = { "/bin/true", NULL, "unreachable", NULL };
	const char *envp[] = { NULL };

	/* The estimate short-circuits at the NULL — it is deliberately
	 * survivable, just wrong (it sizes for argc == 1). */
	size_t need = tawcroot_exec_state_estimate_bytes("/bin/true", 3, argv, envp, NULL);
	uint8_t *buf = malloc(need + 4096);
	test_nonnull(buf);

	/* Generous buffer, so a rejection here is the argv scan, not ENOSPC. */
	long w = tawcroot_exec_state_write(buf, need + 4096, "/bin/true", 3,
	                                   argv, envp, NULL);
	test_true(w < 0);
	free(buf);
}

test(exec_state_buf_too_small)
{
	const char *argv[] = { "long-argument-name", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[16];   /* nowhere near enough */
	test_int_eq((int)tawcroot_exec_state_write(buf, sizeof buf,
	                                           "/usr/bin/some-program",
	                                           1, argv, envp, NULL),
	            -28 /* ENOSPC */);
}

test(exec_state_rejects_bad_magic)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[256];
	tawcroot_exec_state_write(buf, sizeof buf, "/p", 1, argv, envp, NULL);

	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	h->magic = 0xdeadbeef;

	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, sizeof buf, &out),
	            -22);
}

test(exec_state_rejects_bad_version)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[256];
	tawcroot_exec_state_write(buf, sizeof buf, "/p", 1, argv, envp, NULL);
	((tawcroot_exec_state_header *)buf)->version = 99;
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, sizeof buf, &out),
	            -22);
}

test(exec_state_rejects_truncated_strings)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[256];
	long w = tawcroot_exec_state_write(buf, sizeof buf, "/p", 1, argv, envp, NULL);

	/* Truncate to just the header. Reader should refuse — claimed
	 * string_bytes won't fit. */
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf,
	                                          sizeof(tawcroot_exec_state_header), &out),
	            -22);
	(void)w;
}

test(exec_state_rejects_offset_past_strings)
{
	const char *argv[] = { "p", NULL };
	const char *envp[] = { NULL };
	uint8_t buf[256];
	tawcroot_exec_state_write(buf, sizeof buf, "/p", 1, argv, envp, NULL);
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	h->path_off = h->string_bytes + 100;
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, sizeof buf, &out),
	            -22);
}

test(exec_state_carries_shm_table)
{
	const char *argv[] = { "/p", NULL };
	const char *envp[] = { NULL };

	const char *shm_names[] = { "moz-ipc-1", "moz-ipc-2", "media-buf" };
	const int   shm_fds[]   = { 1042, 1099, 1130 };

	tawcroot_exec_state_extras ex = { 0 };
	ex.n_shm    = 3;
	ex.shm_name = shm_names;
	ex.shm_fd   = shm_fds;

	size_t need = tawcroot_exec_state_estimate_bytes("/p", 1, argv, envp,
							 &ex);
	uint8_t *buf = malloc(need);
	test_nonnull(buf);
	long w = tawcroot_exec_state_write(buf, need, "/p", 1, argv, envp, &ex);
	test_int_eq((int)w, (int)need);

	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, &out),
		    0);
	test_int_eq((int)out.n_shm, 3);
	test_str_eq(out.shm_name[0], "moz-ipc-1");
	test_str_eq(out.shm_name[1], "moz-ipc-2");
	test_str_eq(out.shm_name[2], "media-buf");
	test_int_eq(out.shm_fd[0], 1042);
	test_int_eq(out.shm_fd[1], 1099);
	test_int_eq(out.shm_fd[2], 1130);
	free(buf);
}

test(exec_state_carries_identity)
{
	const char *argv[] = { "/p", NULL };
	const char *envp[] = { NULL };

	tawc_identity id = { 0 };
	id.ruid = id.euid = id.suid = id.fsuid = 994;
	id.rgid = id.egid = id.sgid = id.fsgid = 994;
	id.ngroups   = 2;
	id.groups[0] = 994;
	id.groups[1] = 5;

	tawcroot_exec_state_extras ex = { 0 };
	ex.identity = &id;

	size_t need = tawcroot_exec_state_estimate_bytes("/p", 1, argv, envp,
							 &ex);
	uint8_t *buf = malloc(need);
	test_nonnull(buf);
	long w = tawcroot_exec_state_write(buf, need, "/p", 1, argv, envp, &ex);
	test_int_eq((int)w, (int)need);

	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, &out),
		    0);
	test_int_eq(out.has_identity, 1);
	test_int_eq((int)out.identity.ruid, 994);
	test_int_eq((int)out.identity.suid, 994);
	test_int_eq((int)out.identity.fsgid, 994);
	test_int_eq((int)out.identity.ngroups, 2);
	test_int_eq((int)out.identity.groups[1], 5);
	free(buf);

	/* Absent identity reads back as absent. */
	need = tawcroot_exec_state_estimate_bytes("/p", 1, argv, envp, NULL);
	buf = malloc(need);
	test_nonnull(buf);
	test_int_eq((int)tawcroot_exec_state_write(buf, need, "/p", 1, argv,
						   envp, NULL),
		    (int)need);
	test_int_eq((int)tawcroot_exec_state_read(buf, need, &out),
		    0);
	test_int_eq(out.has_identity, 0);
	free(buf);
}

test(exec_state_rejects_oversize_identity_ngroups)
{
	const char *argv[] = { "/p", NULL };
	const char *envp[] = { NULL };
	tawc_identity id = { 0 };
	id.ngroups = 1;
	tawcroot_exec_state_extras ex = { 0 };
	ex.identity = &id;

	size_t need = tawcroot_exec_state_estimate_bytes("/p", 1, argv, envp,
							 &ex);
	uint8_t *buf = malloc(need);
	test_nonnull(buf);
	long w = tawcroot_exec_state_write(buf, need, "/p", 1,
					   argv, envp, &ex);
	test_true(w > 0);

	/* Corrupt the embedded ngroups past the shadow capacity; the
	 * reader must reject rather than let --exec-child copy OOB. */
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	h->identity.ngroups = TAWC_IDENTITY_NGROUPS + 1;

	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, (size_t)w, &out),
		    -22 /* EINVAL */);
	free(buf);
}

test(exec_state_too_many_shm)
{
	const char *argv[] = { "/p", NULL };
	const char *envp[] = { NULL };
	const char *names[TAWCROOT_EXEC_STATE_MAX_SHM + 1];
	int fds[TAWCROOT_EXEC_STATE_MAX_SHM + 1];
	for (int i = 0; i < TAWCROOT_EXEC_STATE_MAX_SHM + 1; i++) {
		names[i] = "n";
		fds[i]   = 1000 + i;
	}
	tawcroot_exec_state_extras ex = { 0 };
	ex.n_shm    = TAWCROOT_EXEC_STATE_MAX_SHM + 1;
	ex.shm_name = names;
	ex.shm_fd   = fds;

	uint8_t buf[16 * 1024];
	test_int_eq((int)tawcroot_exec_state_write(buf, sizeof buf, "/p", 1,
						   argv, envp, &ex),
		    -7 /* E2BIG */);
}

test(exec_state_with_many_args_and_env)
{
	enum { NA = 100, NE = 80 };
	const char *argv[NA + 1];
	const char *envp[NE + 1];
	for (int i = 0; i < NA; i++) argv[i] = "argument-with-some-length";
	argv[NA] = NULL;
	for (int i = 0; i < NE; i++) envp[i] = "VAR=value";
	envp[NE] = NULL;

	size_t need = tawcroot_exec_state_estimate_bytes("/path/to/program",
	                                                 NA, argv, envp, NULL);
	uint8_t *buf = malloc(need);
	test_int_eq((int)tawcroot_exec_state_write(buf, need, "/path/to/program",
	                                           NA, argv, envp, NULL),
	            (int)need);

	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, &out), 0);
	test_int_eq((int)out.argc, NA);
	test_int_eq((int)out.envc, NE);
	const char **av = split_argv(&out), **ev = split_envp(&out);
	for (int i = 0; i < NA; i++) test_str_eq(av[i], "argument-with-some-length");
	for (int i = 0; i < NE; i++) test_str_eq(ev[i], "VAR=value");
	test_ptr_eq(av[NA], NULL);
	test_ptr_eq(ev[NE], NULL);
	free(av); free(ev);
	free(buf);
}

/* No argc/envc caps (the old format stopped at 4096 / 1024): a
 * 100k-arg state round-trips. */
test(exec_state_100k_args_roundtrip)
{
	enum { NA = 100000, NE = 5000 };
	static const char *argv[NA + 1];
	static const char *envp[NE + 1];
	for (int i = 0; i < NA; i++) argv[i] = "globbed-filename-12345.txt";
	argv[NA] = NULL;
	for (int i = 0; i < NE; i++) envp[i] = "SOME_EXPORTED_VAR=some-value";
	envp[NE] = NULL;

	size_t need = tawcroot_exec_state_estimate_bytes("/usr/bin/rm",
	                                                 NA, argv, envp, NULL);
	uint8_t *buf = malloc(need);
	test_int_eq((int)tawcroot_exec_state_write(buf, need, "/usr/bin/rm",
	                                           NA, argv, envp, NULL),
	            (int)need);
	/* The header no longer scales with argc. */
	test_true(sizeof(tawcroot_exec_state_header) < 2048);

	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, need, &out), 0);
	test_int_eq((int)out.argc, NA);
	test_int_eq((int)out.envc, NE);
	const char **av = split_argv(&out), **ev = split_envp(&out);
	test_str_eq(av[0], "globbed-filename-12345.txt");
	test_str_eq(av[NA - 1], "globbed-filename-12345.txt");
	test_str_eq(ev[NE - 1], "SOME_EXPORTED_VAR=some-value");
	test_ptr_eq(av[NA], NULL);
	test_ptr_eq(ev[NE], NULL);
	free(av); free(ev);
	free(buf);
}

test(exec_state_rejects_count_nul_mismatch)
{
	uint8_t buf[4096];
	size_t n = small_state(buf, sizeof buf);
	test_true(n > 0);
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	tawcroot_exec_state out;
	test_int_eq((int)tawcroot_exec_state_read(buf, n, &out), 0);

	h->argc = 3;      /* region holds 2 strings */
	test_int_eq((int)tawcroot_exec_state_read(buf, n, &out), -22);
	h->argc = 1;
	test_int_eq((int)tawcroot_exec_state_read(buf, n, &out), -22);
	h->argc = 2;
	h->envc = 0;      /* non-empty region, zero count */
	test_int_eq((int)tawcroot_exec_state_read(buf, n, &out), -22);
}

test(exec_state_rejects_unterminated_region)
{
	uint8_t buf[4096];
	size_t n = small_state(buf, sizeof buf);
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	tawcroot_exec_state out;
	/* Chop the argv region's final NUL off: "p\0q" — count matches
	 * NULs only if the reader also checks termination. */
	h->argv_bytes -= 1;
	h->argc = 1;
	test_int_eq((int)tawcroot_exec_state_read(buf, n, &out), -22);
}

test(exec_state_rejects_region_out_of_bounds)
{
	uint8_t buf[4096];
	size_t n = small_state(buf, sizeof buf);
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)buf;
	tawcroot_exec_state out;
	uint32_t saved = h->envp_off;
	h->envp_off = h->string_bytes;           /* starts at the end */
	test_int_eq((int)tawcroot_exec_state_read(buf, n, &out), -22);
	h->envp_off = saved;
	h->envp_bytes = 0xfffffff0u;             /* overflows */
	test_int_eq((int)tawcroot_exec_state_read(buf, n, &out), -22);
}
