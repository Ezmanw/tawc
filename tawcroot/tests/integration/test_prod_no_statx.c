/* tawcroot on a kernel without statx (pre-4.11, wmww/tawc#30).
 *
 * Runs `tawcroot -r /` under the androidfilter wrapper's --no-statx
 * rule, so every raw statx tawcroot issues gets ENOSYS while guest
 * statx still traps to the handler. A shell script exercising exec
 * (dynamic binary, shebang, EISDIR, EACCES) and stat must print
 * exactly what a native run prints.
 *
 * Host-only, like test_exec_limits.c: `-r /` with the host's own
 * binaries is the cheapest real libc guest. The device run covers
 * the statx fallback through the androidfilter_nostatx suite. */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rootfs_helpers.h"

#ifndef TAWCROOT_PROD_BIN
# error "TAWCROOT_PROD_BIN must be defined"
#endif
#ifndef TAWCROOT_ANDROID_FILTER_WRAP
# error "TAWCROOT_ANDROID_FILTER_WRAP must be defined"
#endif
#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#if !defined(__ANDROID__)
static cstr run_script(bool guest, const char *script, int *rc)
{
	/* Start from /: a host cwd outside the guest view would make the
	 * guest shells' getcwd fail, which is unrelated noise here. */
	VecStr v = c_init(vec_str, {"/usr/bin/env", "-C", "/"});
	if (guest) {
		vec_str_push(&v, TAWCROOT_ANDROID_FILTER_WRAP);
		vec_str_push(&v, "--no-statx");
		vec_str_push(&v, "--");
		vec_str_push(&v, TAWCROOT_PROD_BIN);
		vec_str_push(&v, "-r");
		vec_str_push(&v, "/");
		vec_str_push(&v, "--");
	}
	vec_str_push(&v, "/bin/sh");
	vec_str_push(&v, "-c");
	vec_str_push(&v, script);
	cstr out, err;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = v, .stdout = &out, .stderr = &err, .exit_code = rc,
	});
	failable_result_drop(&res);
	cstr_drop(&err);
	return out;
}
#endif

test(no_statx_exec_and_stat_match_native)
{
#if defined(__ANDROID__)
	printf("    skipping (host-only: -r / with host binaries)\n");
	return;
#else
	char dir[256], p[300];
	snprintf(dir, sizeof dir, "%s/tawcroot-nostatx-%d",
	         TAWCROOT_TEST_TMPDIR, (int)getpid());
	rh_rmrf(dir);
	snprintf(p, sizeof p, "%s/dir", dir);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/script", dir);
	test_true(rh_write_text(p, "#!/bin/sh\necho shebang-ok\n"));
	test_int_eq(chmod(p, 0755), 0);
	snprintf(p, sizeof p, "%s/nonexec", dir);
	test_true(rh_write_text(p, "#!/bin/sh\necho nope\n"));

	/* The #30 symptom was `env: '/bin/bash': Function not
	 * implemented` from the first line. */
	char script[2048];
	snprintf(script, sizeof script,
	         "exec 2>&1\n"
	         "env bash -c 'echo env-bash-ok'\n"
	         "%1$s/script\n"
	         "%1$s/dir; echo dir=$?\n"
	         "%1$s/nonexec; echo nonexec=$?\n"
	         "stat -c '%%s %%h %%F' %1$s/script %1$s/dir\n"
	         "stat -L -c '%%F' /proc/self/exe\n",
	         dir);

	int nrc = -1, grc = -1;
	cstr native = run_script(false, script, &nrc);
	cstr guest  = run_script(true, script, &grc);
	test_int_eq(nrc, 0);
	test_true(strstr(cstr_str(&native), "env-bash-ok\n") != NULL);
	test_true(strstr(cstr_str(&native), "shebang-ok\n") != NULL);
	if (strcmp(cstr_str(&native), cstr_str(&guest)) != 0)
		printf("native:\n%s\nguest:\n%s\n", cstr_str(&native),
		       cstr_str(&guest));
	test_str_eq(cstr_str(&guest), cstr_str(&native));
	test_int_eq(grc, nrc);
	cstr_drop(&native);
	cstr_drop(&guest);
	rh_rmrf(dir);
#endif
}
