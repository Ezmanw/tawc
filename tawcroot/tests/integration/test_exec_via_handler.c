/* End-to-end test of the SIGSYS-handler-side execve interception via
 * the testhost-only `--exec-via-handler` diagnostic.
 *
 * `--exec-via-handler` exercises `tawcroot_exec_handler_perform()`,
 * which builds an exec_state in a memfd, opens /proc/self/exe, and
 * execveats into us with `--exec-child <fd>` — the same dance the
 * real SIGSYS handler will perform when it traps the guest's
 * `execve(2)`. Production gates `--exec-via-handler` off (only the
 * `--exec-child` re-entry is reachable in production); tawcroot-
 * testhost exposes both halves so we can test the round-trip.
 *
 * The handler's `/proc/self/exe` re-exec lands back in testhost main
 * with `--exec-child <bare-int>`, which testhost dispatches to the
 * production loader-child path (see main.c). So this test exercises
 * the same code path production runs at SIGSYS-driven exec time.
 *
 * Success of these tests means front + back halves of the phase-2.6
 * dance are wired correctly. The remaining phase-2.6 work (hooking
 * the handler into the dispatch table for actual SIGSYS-driven
 * traps) is a small wrapper that reads guest memory via usercopy.c
 * and calls into this same code path.
 */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "exec_state.h"
#include "loader_elf.h"

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#ifndef TAWCROOT_TESTHOST_BIN
# error "TAWCROOT_TESTHOST_BIN must be defined"
#endif
#ifndef TAWCROOT_STATIC_EXIT42_BIN
# error "TAWCROOT_STATIC_EXIT42_BIN must be defined"
#endif
#ifndef TAWCROOT_DYNAMIC_EXIT42_BIN
# error "TAWCROOT_DYNAMIC_EXIT42_BIN must be defined"
#endif

static int run(const char *const *extra_args)
{
	VecStr cmd = c_init(vec_str, {TAWCROOT_TESTHOST_BIN});
	for (const char *const *p = extra_args; *p; p++) {
		vec_str_push(&cmd, *p);
	}
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = cmd, .exit_code = &rc
	});
	failable_result_drop(&res);
	return rc;
}

/* Run `script` under /bin/sh -c — for tests that need a wrapper shell
 * around the testhost invocation (e.g. to inject an env var). */
static int run_sh(const char *script)
{
	VecStr cmd = c_init(vec_str, {"/bin/sh", "-c"});
	vec_str_push(&cmd, script);
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = cmd, .exit_code = &rc
	});
	failable_result_drop(&res);
	return rc;
}

test(exec_via_handler_static_exit42)
{
	const char *args[] = { "--exec-via-handler",
	                       TAWCROOT_STATIC_EXIT42_BIN, NULL };
	test_int_eq(run(args), 42);
}

test(exec_via_handler_dynamic_exit42)
{
	const char *args[] = { "--exec-via-handler",
	                       TAWCROOT_DYNAMIC_EXIT42_BIN, NULL };
	test_int_eq(run(args), 42);
}

test(exec_via_handler_system_bin_true)
{
	const char *args[] = { "--exec-via-handler", "/bin/true", NULL };
	test_int_eq(run(args), 0);
}

test(exec_via_handler_nonexistent_returns_50)
{
	/* `--exec-via-handler` reports any handler-perform negative
	 * return code via main and exits 50. Probe of nonexistent path
	 * fails at the open() step inside the handler. */
	const char *args[] = {
		"--exec-via-handler",
		"/this/path/does/not/exist/promise", NULL,
	};
	test_int_eq(run(args), 50);
}

test(exec_via_handler_directory_returns_50)
{
	/* execve of a directory must fail cleanly at the probe (EISDIR)
	 * — NOT execveat into the loader, which would destroy the calling
	 * process and exit with a loader code. Regression: the probe's
	 * O_RDONLY open succeeds on directories. */
	const char *args[] = { "--exec-via-handler", "/etc", NULL };
	test_int_eq(run(args), 50);
}

test(exec_via_handler_non_executable_returns_50)
{
	/* Same for a mode-644 regular file: real execve gives EACCES and
	 * the caller survives. /etc/hostname is a stable non-executable
	 * file on every host we run on. */
	const char *args[] = { "--exec-via-handler", "/etc/hostname", NULL };
	test_int_eq(run(args), 50);
}

/* Write `contents` to a fresh 0755 temp file; returns the path in a
 * static buffer (single-threaded test, one outstanding at a time). */
static const char *make_exec_file(const char *suffix, const char *contents)
{
	static char path[256];
	snprintf(path, sizeof path, "%s/tawcroot-classify-%s",
	         TAWCROOT_TEST_TMPDIR, suffix);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) return NULL;
	size_t n = strlen(contents);
	if (write(fd, contents, n) != (ssize_t)n) { close(fd); return NULL; }
	close(fd);
	return path;
}

test(exec_via_handler_non_elf_text_returns_50)
{
	/* A `chmod +x`'d text file with no shebang: real execve returns
	 * -ENOEXEC (so a shell falls back to `sh file`). Pre-fix the probe
	 * passed it through to execveat and the loader died post-commit
	 * with exit 61. The classification probe must turn it into a clean
	 * -ENOEXEC, reported as exit 50. */
	const char *p = make_exec_file("text", "this is not a program\n");
	test_true(p != NULL);
	const char *args[] = { "--exec-via-handler", p, NULL };
	test_int_eq(run(args), 50);
	(void)unlink(p);
}

test(exec_via_handler_wrong_arch_elf_returns_50)
{
	/* A structurally valid ELF64 for the OTHER machine: real execve
	 * returns -ENOEXEC. Pre-fix, classify_elf checked only e_type, so
	 * the probe passed a cross-arch binary through to the commit and
	 * the loader mapped and jumped into foreign code (SIGILL — a
	 * destroyed caller instead of a shell's `cannot execute binary
	 * file`). Header fields other than e_machine are all valid so
	 * only the machine check can reject it. */
	unsigned char eh[64] = {
		0x7f, 'E', 'L', 'F',
		2,  /* ELFCLASS64 */
		1,  /* ELFDATA2LSB */
		1,  /* EV_CURRENT */
	};
	uint16_t wrong = (TAWC_EM_HOST == TAWC_EM_X86_64)
		? TAWC_EM_AARCH64 : TAWC_EM_X86_64;
	eh[16] = 2;                          /* e_type = ET_EXEC */
	eh[18] = (unsigned char)(wrong & 0xff);      /* e_machine */
	eh[19] = (unsigned char)(wrong >> 8);
	eh[20] = 1;                          /* e_version = EV_CURRENT */
	eh[32] = 64;                         /* e_phoff = sizeof(ehdr) */
	eh[54] = 56;                         /* e_phentsize = sizeof(phdr) */
	eh[56] = 1;                          /* e_phnum = 1 */

	static char path[256];
	snprintf(path, sizeof path, "%s/tawcroot-classify-xarch",
	         TAWCROOT_TEST_TMPDIR);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	test_true(fd >= 0);
	test_true(write(fd, eh, sizeof eh) == (ssize_t)sizeof eh);
	close(fd);

	const char *args[] = { "--exec-via-handler", path, NULL };
	test_int_eq(run(args), 50);
	(void)unlink(path);
}

test(exec_via_handler_sets_comm_and_cmdline)
{
	/* Kernel-visible identity must match the guest after the dance:
	 * comm = basename of the exec path (PR_SET_NAME at the loader
	 * jump), cmdline = the guest argv NUL-joined (in-place arg-region
	 * rewrite; prefix match because the script text is itself the
	 * last argv entry). Pre-fix both read as the re-exec protocol
	 * ("tawcroot --exec-child <fd>" / comm "<fd>"), so pgrep/pkill/ps
	 * couldn't identify any guest process. */
	const char *script =
		"[ \"$(cat /proc/$$/comm)\" = sh ] || exit 7; "
		"case \"$(tr '\\0' ' ' < /proc/$$/cmdline)\" in "
		"'/bin/sh -c '*) exit 42;; *) exit 8;; esac";
	const char *args[] = { "--exec-via-handler", "/bin/sh", "-c",
	                       script, NULL };
	test_int_eq(run(args), 42);
}

test(exec_via_handler_custom_argv0_passthrough)
{
	/* Caller argv[0] reaches the guest verbatim — on the synthesized
	 * stack ($0) and in the kernel cmdline — while comm stays the
	 * exec-path basename, exactly like a real execve. Login shells'
	 * "-sh", busybox applet dispatch, and `exec -a` all depend on
	 * this. Pre-fix the loader replaced argv[0] with the exec path
	 * (it smuggled the script path through slot 0 for shebangs). */
	const char *script =
		"[ \"$0\" = customsh0 ] || exit 7; "
		"[ \"$(cat /proc/$$/comm)\" = sh ] || exit 8; "
		"case \"$(tr '\\0' ' ' < /proc/$$/cmdline)\" in "
		"'customsh0 -c '*) exit 42;; *) exit 9;; esac";
	const char *args[] = { "--exec-via-handler", "--argv0=customsh0",
	                       "/bin/sh", "-c", script, NULL };
	test_int_eq(run(args), 42);
}

test(exec_via_handler_cmdline_exact_length)
{
	/* The arg region is sized to the byte: prepare() computes the
	 * post-shebang NUL-joined argv length and shrinks the proctitle
	 * by the "--exec-child <fd>" protocol overhead, so the rewritten
	 * cmdline ends exactly at the last argument's NUL. tr maps NULs
	 * to '|': an exact region ends "...#end|"; any trailing slack
	 * would end "...||" (the earlier slack-padded sizing left ~17). */
	const char *script =
		"cl=$(tr '\\0' '|' < /proc/$$/cmdline); "
		"case \"$cl\" in *'#end|') ;; *) exit 8;; esac; "
		"case \"$cl\" in *'||') exit 9;; esac; "
		"exit 42 #end";
	const char *args[] = { "--exec-via-handler", "/bin/sh", "-c",
	                       script, NULL };
	test_int_eq(run(args), 42);
}

test(exec_via_handler_environ_region_matches_guest_env)
{
	/* /proc/<pid>/environ is the kernel env region, rebuilt by each
	 * real exec from the execveat's envp. commit() must forward the
	 * guest envp there (pointers into the mapped exec_state) or every
	 * guest process shows an EMPTY environ — to itself and to `ps e`.
	 * The wrapper injects a marker into testhost's environment;
	 * perform() forwards testhost's envp as the guest envp, so the
	 * guest sh must find the marker in its own /proc/$$/environ. */
	static char script[512];
	snprintf(script, sizeof script,
	         "TAWC_E2E_ENVIRON=visible exec '%s' --exec-via-handler "
	         "/bin/sh -c 'tr \"\\\\0\" \"\\\\n\" < /proc/$$/environ | "
	         "grep -qx TAWC_E2E_ENVIRON=visible && exit 42; exit 8'",
	         TAWCROOT_TESTHOST_BIN);
	test_int_eq(run_sh(script), 42);
}

test(exec_via_handler_shebang_cmdline_has_interpreter)
{
	/* Shebang fidelity: like the kernel's binfmt_script, cmdline shows
	 * [interp, script-path, ...] and comm is the SCRIPT's basename
	 * (truncated to 15 chars, TASK_COMM_LEN), not the interpreter's.
	 * The exec handler reserves arg-region space for the interpreter
	 * prepend via cmdline_bytes (classify_loadable's shebang accounting). */
	const char *p = make_exec_file("title",
		"#!/bin/sh\n"
		"[ \"$(cat /proc/$$/comm)\" = tawcroot-classi ] || exit 7\n"
		"case \"$(tr '\\0' ' ' < /proc/$$/cmdline)\" in\n"
		"\"/bin/sh \"*\"tawcroot-classify-title\"*) exit 42;;\n"
		"*) exit 8;;\n"
		"esac\n");
	test_true(p != NULL);
	const char *args[] = { "--exec-via-handler", p, NULL };
	test_int_eq(run(args), 42);
	(void)unlink(p);
}

test(exec_via_handler_300_args_roundtrip)
{
	/* The loader's eff_argv was once sized to ~264 entries; the 265th
	 * arg destroyed the exec'd process post-commit (bare exit 74, no
	 * errno, no stderr) — `cat *` over a few hundred files silently
	 * no-op'd. Anything the collection layer accepts must survive. */
	enum { N = 300 };
	static const char *args[N + 6];
	int n = 0;
	args[n++] = "--exec-via-handler";
	args[n++] = "/bin/sh";
	args[n++] = "-c";
	args[n++] = "[ \"$#\" = 300 ] && exit 42; exit 8";
	args[n++] = "sh";
	for (int i = 0; i < N; i++) args[n++] = "a";
	args[n] = NULL;
	test_int_eq(run(args), 42);
}

test(exec_via_handler_shebang_300_args_roundtrip)
{
	/* Same wall via a #! script (threshold was one lower there: the
	 * interpreter prepend consumed an eff_argv slot). Mirrors the
	 * original repro: ./argc.sh with a few hundred args exited 75. */
	const char *p = make_exec_file("argc300",
	                               "#!/bin/sh\n"
	                               "[ \"$#\" = 300 ] && exit 42\n"
	                               "exit 8\n");
	test_true(p != NULL);
	enum { N = 300 };
	static const char *args[N + 3];
	int n = 0;
	args[n++] = "--exec-via-handler";
	args[n++] = p;
	for (int i = 0; i < N; i++) args[n++] = "a";
	args[n] = NULL;
	test_int_eq(run(args), 42);
	(void)unlink(p);
}

test(exec_via_handler_100k_args_roundtrip)
{
	/* No tawcroot-side argc cap: the old 4096-entry exec_state limit
	 * is gone, so anything the kernel let reach us must survive the
	 * dance (test_exec_limits.c covers the kernel-exact edges). */
	enum { NA = 100000 };
	static char script[48];
	snprintf(script, sizeof script,
	         "[ \"$#\" = %d ] && exit 42; exit 8", (int)NA);
	static const char *args[NA + 6];
	int n = 0;
	args[n++] = "--exec-via-handler";
	args[n++] = "/bin/sh";
	args[n++] = "-c";
	args[n++] = script;
	args[n++] = "sh";
	for (int i = 0; i < NA; i++) args[n++] = "a";
	args[n] = NULL;
	test_int_eq(run(args), 42);
}

test(exec_via_handler_shebang_missing_interp_returns_50)
{
	/* A script whose interpreter doesn't exist: real execve returns
	 * -ENOENT. Pre-fix the loader chased the shebang post-commit and
	 * died with exit 75. The probe resolves the shebang and surfaces
	 * the missing-interpreter errno as exit 50. */
	const char *p = make_exec_file("badinterp",
	                               "#!/no/such/interpreter/here\n"
	                               "echo hi\n");
	test_true(p != NULL);
	const char *args[] = { "--exec-via-handler", p, NULL };
	test_int_eq(run(args), 50);
	(void)unlink(p);
}
