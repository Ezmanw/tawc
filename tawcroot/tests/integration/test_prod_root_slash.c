/* `tawcroot -r /`: the rootfs prefix is "/" itself. Reverse
 * translation used to demand a '/' after the prefix, so every host
 * path except "/" fell outside the view: getcwd was ENOENT from any
 * cwd but "/", and /proc/self/cwd failed to resolve.
 *
 * Host-only, like test_exec_limits.c (host binaries as the guest). */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#include <stdio.h>
#include <string.h>

#ifndef TAWCROOT_PROD_BIN
# error "TAWCROOT_PROD_BIN must be defined"
#endif

test(root_slash_getcwd_reverse_translates)
{
#if defined(__ANDROID__)
	printf("    skipping (host-only: -r / with host binaries)\n");
	return;
#else
	VecStr v = c_init(vec_str, {"/usr/bin/env", "-C", "/usr/lib",
		TAWCROOT_PROD_BIN, "-r", "/", "--", "/bin/sh", "-c",
		"exec 2>&1; /bin/pwd; readlink /proc/self/cwd; "
		"cd /tmp && /bin/pwd"});
	cstr out, err;
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = v, .stdout = &out, .stderr = &err, .exit_code = &rc,
	});
	failable_result_drop(&res);
	test_int_eq(rc, 0);
	test_str_eq(cstr_str(&out), "/usr/lib\n/usr/lib\n/tmp\n");
	cstr_drop(&out);
	cstr_drop(&err);
#endif
}
