/* Guest execve argv/envp limits match the kernel's (plan #28 fix).
 *
 * Runs host programs as guests with `tawcroot -r /` and diffs their
 * output against a native run, so the expectation is whatever this
 * kernel does — MAX_ARG_STRLEN from the page size, the budget from
 * RLIMIT_STACK — not constants baked into the test.
 *
 * Host-only: `-r /` with the host's own binaries is the cheapest way to
 * get a real libc guest that execs; the device run covers exec through
 * the regular rootfs suites. */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef TAWCROOT_PROD_BIN
# error "TAWCROOT_PROD_BIN must be defined"
#endif
#ifndef TAWCROOT_DYNAMIC_EXEC_LIMITS_BIN
# error "TAWCROOT_DYNAMIC_EXEC_LIMITS_BIN must be defined"
#endif
#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#if !defined(__ANDROID__)
/* Run `cmd`, natively or under `tawcroot -r / --`; returns stdout. */
static cstr run_capture(bool guest, const char *const *cmd, int *rc)
{
	VecStr v = vec_str_init();
	if (guest) {
		vec_str_push(&v, TAWCROOT_PROD_BIN);
		vec_str_push(&v, "-r");
		vec_str_push(&v, "/");
		vec_str_push(&v, "--");
	}
	for (const char *const *p = cmd; *p; p++) vec_str_push(&v, *p);
	cstr out, err;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = v, .stdout = &out, .stderr = &err, .exit_code = rc,
	});
	failable_result_drop(&res);
	cstr_drop(&err);
	return out;
}

static bool has_line(const cstr *out, const char *line)
{
	char want[128];
	snprintf(want, sizeof want, "%s\n", line);
	return strstr(cstr_str(out), want) != NULL;
}
#endif

test(exec_limits_match_kernel)
{
#if defined(__ANDROID__)
	printf("    skipping (host-only: -r / with host binaries)\n");
	return;
#else
	const char *cmd[] = { TAWCROOT_DYNAMIC_EXEC_LIMITS_BIN, "report",
	                      TAWCROOT_DYNAMIC_EXEC_LIMITS_BIN,
	                      TAWCROOT_TEST_TMPDIR, NULL };
	int nrc = -1, grc = -1;
	cstr native = run_capture(false, cmd, &nrc);
	cstr guest  = run_capture(true, cmd, &grc);
	test_int_eq(nrc, 0);
	test_int_eq(grc, 0);

	/* The experiments must hit both sides of each limit natively, or
	 * agreement proves nothing. */
	const char *expect[] = {
		"arg_strlen_max: ok", "arg_strlen_over: E2BIG",
		"env_strlen_max: ok", "env_strlen_over: E2BIG",
		"budget_8M_exact: ok", "budget_8M_over: E2BIG",
		"budget_1M_exact: ok", "budget_1M_over: E2BIG",
		"args_100k: ok",
		"cmdline_300k: ok", "cmdline_shebang_300k: ok",
		"shebang_over_budget: E2BIG",
		"missing_over_budget: ENOENT",
		"path_4095: ENOENT", "path_4096: ENAMETOOLONG",
		"argv_bad_ptr: EFAULT", "argv_bad_array: EFAULT",
		"small_exec_tight_as: ok",
	};
	for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
		if (!has_line(&native, expect[i]))
			printf("native lacks: %s\n", expect[i]);
		test_true(has_line(&native, expect[i]));
	}
	if (strcmp(cstr_str(&native), cstr_str(&guest)) != 0)
		printf("native:\n%s\nguest:\n%s\n", cstr_str(&native),
		       cstr_str(&guest));
	test_str_eq(cstr_str(&guest), cstr_str(&native));
	cstr_drop(&native);
	cstr_drop(&guest);
#endif
}

test(exec_limits_shell_tools_match_kernel)
{
#if defined(__ANDROID__)
	printf("    skipping (host-only: -r / with host binaries)\n");
	return;
#else
	/* The #28 shapes: a ~100 KB `sh -c` script, and xargs sizing its
	 * batches from ARG_MAX over 5 MB of names. Batch sizes depend only
	 * on the limits, so they must match a native run exactly. */
	char script[256];
	snprintf(script, sizeof script, "%s/exec-limits-tools-%d.sh",
	         TAWCROOT_TEST_TMPDIR, (int)getpid());
	FILE *f = fopen(script, "w");
	test_nonnull(f);
	fputs("s=$(yes 'true;' | head -n 20000 | tr -d '\\n')\n"
	      "sh -c \"$s\" && echo sh-c-ok\n"
	      "seq -f n%07g 1 500000 | xargs -s 2000000 sh -c 'echo $#' sh\n"
	      "getconf ARG_MAX\n", f);
	fclose(f);

	const char *cmd[] = { "/bin/sh", script, NULL };
	int nrc = -1, grc = -1;
	cstr native = run_capture(false, cmd, &nrc);
	cstr guest  = run_capture(true, cmd, &grc);
	unlink(script);
	test_int_eq(nrc, 0);
	test_int_eq(grc, 0);
	test_true(has_line(&native, "sh-c-ok"));
	if (strcmp(cstr_str(&native), cstr_str(&guest)) != 0)
		printf("native:\n%s\nguest:\n%s\n", cstr_str(&native),
		       cstr_str(&guest));
	test_str_eq(cstr_str(&guest), cstr_str(&native));
	cstr_drop(&native);
	cstr_drop(&guest);
#endif
}
