/* Exec-limit probe: runs a set of execve experiments around the
 * kernel's argv/envp limits and prints one "name: outcome" line each.
 * The test runs it natively and under tawcroot and requires identical
 * output, so tawcroot is compared against the kernel, not against
 * constants baked into the test.
 *
 *   dynamic_exec_limits report <self-path> <tmpdir>
 *
 * Exec targets are this binary in a check mode (argv[1] after shebang
 * rewriting): "ok" exits 0; "count N" exits 0 iff argc == N; "cmdline"
 * exits 0 iff /proc/self/cmdline is exactly the NUL-joined argv.
 *
 * Outcomes: "ok", "exit N", "signal N", or the execve errno name.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *self;
static char tmpdir[1024];
static size_t max_strlen;       /* MAX_ARG_STRLEN incl. NUL */

static size_t budget(rlim_t stack)
{
	size_t lim = 6u << 20;
	if (stack / 4 < lim) lim = (size_t)(stack / 4);
	return lim < (128u << 10) ? (128u << 10) : lim;
}

static char *filled(size_t n, char c)
{
	char *s = malloc(n + 1);
	memset(s, c, n);
	s[n] = 0;
	return s;
}

static const char *errname(int e)
{
	static char buf[16];
	switch (e) {
	case E2BIG:        return "E2BIG";
	case ENOENT:       return "ENOENT";
	case ENAMETOOLONG: return "ENAMETOOLONG";
	case EFAULT:       return "EFAULT";
	case EACCES:       return "EACCES";
	case ENOEXEC:      return "ENOEXEC";
	case ENOMEM:       return "ENOMEM";
	}
	snprintf(buf, sizeof buf, "errno %d", e);
	return buf;
}

/* Fork, optionally set RLIMIT_STACK / RLIMIT_AS, execve, and describe
 * the result. */
static void try_exec(const char *name, const char *path, char **argv,
                     char **envp, rlim_t stack, rlim_t as)
{
	int pfd[2];
	if (pipe2(pfd, O_CLOEXEC) != 0) { perror("pipe"); exit(1); }
	pid_t pid = fork();
	if (pid == 0) {
		struct rlimit rl = { stack, stack };
		if (stack) setrlimit(RLIMIT_STACK, &rl);
		struct rlimit al = { as, as };
		if (as) setrlimit(RLIMIT_AS, &al);
		execve(path, argv, envp);
		int e = errno;
		(void)!write(pfd[1], &e, sizeof e);
		_exit(127);
	}
	close(pfd[1]);
	int e = 0;
	ssize_t n = read(pfd[0], &e, sizeof e);
	close(pfd[0]);
	int st;
	waitpid(pid, &st, 0);
	if (n == (ssize_t)sizeof e) printf("%s: %s\n", name, errname(e));
	else if (WIFSIGNALED(st)) printf("%s: signal %d\n", name, WTERMSIG(st));
	else if (WEXITSTATUS(st)) printf("%s: exit %d\n", name, WEXITSTATUS(st));
	else printf("%s: ok\n", name);
	fflush(stdout);
}

/* argv = { "x", mode, fillers... }, sized so pointers + strings (with
 * `path` and no env) land exactly `delta` bytes past the budget. */
static char **budget_argv(const char *path, const char *mode, size_t lim,
                          long delta)
{
	size_t base = strlen(path) + 1 + 2 + strlen(mode) + 1;
	for (size_t k = 1;; k++) {
		size_t ptrs = (2 + k) * sizeof(char *);
		long fill = (long)lim + delta - (long)ptrs - (long)base;
		if (fill < (long)k) { fprintf(stderr, "budget\n"); exit(1); }
		if ((size_t)fill > k * max_strlen) continue;
		char **v = calloc(k + 3, sizeof *v);
		v[0] = "x";
		v[1] = (char *)mode;
		size_t each = (size_t)fill / k, extra = (size_t)fill % k;
		for (size_t i = 0; i < k; i++)
			v[2 + i] = filled(each + (i < extra) - 1, 'f');
		return v;
	}
}

static char *write_script(const char *tag, const char *mode)
{
	char *p = malloc(1200);
	snprintf(p, 1200, "%s/exec-limits-%d-%s", tmpdir, (int)getpid(), tag);
	FILE *f = fopen(p, "w");
	fprintf(f, "#!%s %s\n", self, mode);
	fclose(f);
	chmod(p, 0755);
	return p;
}

static void report(void)
{
	char *noenv[] = { NULL };
	const rlim_t MiB = 1 << 20;

	/* One string at MAX_ARG_STRLEN (NUL included), and one past it. */
	char *a_ok[]  = { "x", "ok", filled(max_strlen - 1, 'a'), NULL };
	char *a_big[] = { "x", "ok", filled(max_strlen, 'a'), NULL };
	try_exec("arg_strlen_max", self, a_ok, noenv, 8 * MiB, 0);
	try_exec("arg_strlen_over", self, a_big, noenv, 8 * MiB, 0);
	char *e_ok[]  = { filled(max_strlen - 1, 'e'), NULL };
	char *e_big[] = { filled(max_strlen, 'e'), NULL };
	char *plain[] = { "x", "ok", NULL };
	try_exec("env_strlen_max", self, plain, e_ok, 8 * MiB, 0);
	try_exec("env_strlen_over", self, plain, e_big, 8 * MiB, 0);

	/* Total budget, at the default and a lowered RLIMIT_STACK. */
	rlim_t stacks[] = { 8 * MiB, 1 * MiB };
	for (int i = 0; i < 2; i++) {
		char name[64];
		size_t lim = budget(stacks[i]);
		snprintf(name, sizeof name, "budget_%dM_exact", (int)(stacks[i] / MiB));
		try_exec(name, self, budget_argv(self, "ok", lim, 0), noenv,
		         stacks[i], 0);
		snprintf(name, sizeof name, "budget_%dM_over", (int)(stacks[i] / MiB));
		try_exec(name, self, budget_argv(self, "ok", lim, 1), noenv,
		         stacks[i], 0);
	}

	/* 100k short arguments. */
	enum { N = 100000 };
	char **many = calloc(N + 4, sizeof *many);
	many[0] = "x"; many[1] = "count"; many[2] = "100003";
	for (int i = 3; i < N + 3; i++) many[i] = "a";
	try_exec("args_100k", self, many, noenv, 8 * MiB, 0);

	/* /proc/self/cmdline is exact at 300 KB and ~2 MB, with and
	 * without a shebang in front. */
	size_t sizes[] = { 300 << 10, budget(8 * MiB) - 4096 };
	for (int i = 0; i < 2; i++) {
		char name[64];
		snprintf(name, sizeof name, "cmdline_%zuk", sizes[i] >> 10);
		try_exec(name, self, budget_argv(self, "cmdline", sizes[i], 0),
		         noenv, 8 * MiB, 0);
		char *script = write_script("cmdline", "cmdline");
		char **v = budget_argv(script, "y", sizes[i], 0);
		snprintf(name, sizeof name, "cmdline_shebang_%zuk", sizes[i] >> 10);
		try_exec(name, script, v, noenv, 8 * MiB, 0);
		unlink(script);
	}

	/* Fits as given, but not after the shebang rewrite. */
	char *script = write_script("near", "ok");
	try_exec("shebang_over_budget", script,
	         budget_argv(script, "y", budget(8 * MiB), 0), noenv, 8 * MiB, 0);
	unlink(script);

	/* Lookup errors come before E2BIG. */
	try_exec("missing_over_budget", "/nonexistent/exec-limits",
	         a_big, noenv, 8 * MiB, 0);

	/* Path length: PATH_MAX - 1 chars reaches the lookup, PATH_MAX is
	 * ENAMETOOLONG. */
	char *p_ok = filled(4095, '/'), *p_big = filled(4096, '/');
	memcpy(p_ok + 4095 - 4, "nope", 4);
	try_exec("path_4095", p_ok, plain, noenv, 8 * MiB, 0);
	try_exec("path_4096", p_big, plain, noenv, 8 * MiB, 0);

	/* Bad pointers. */
	char *bad[] = { "x", (char *)8, NULL };
	try_exec("argv_bad_ptr", self, bad, noenv, 8 * MiB, 0);
	try_exec("argv_bad_array", self, (char **)8, noenv, 8 * MiB, 0);

	/* A small exec still works under a tight address-space limit. */
	long vm = 0;
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "VmSize: %ld", &vm) == 1) break;
	if (f) fclose(f);
	try_exec("small_exec_tight_as", self, plain, noenv, 8 * MiB,
	         (rlim_t)vm * 1024 + 48 * MiB);
}

static int check_cmdline(int argc, char **argv)
{
	size_t want = 0;
	for (int i = 0; i < argc; i++) want += strlen(argv[i]) + 1;
	char *buf = malloc(want + 2);
	int fd = open("/proc/self/cmdline", O_RDONLY);
	size_t got = 0;
	for (;;) {
		ssize_t n = read(fd, buf + got, want + 2 - got);
		if (n <= 0) break;
		got += (size_t)n;
		if (got == want + 2) break;
	}
	close(fd);
	if (got != want) return 3;
	char *p = buf;
	for (int i = 0; i < argc; i++) {
		if (strcmp(p, argv[i]) != 0) return 4;
		p += strlen(p) + 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	max_strlen = 32 * (size_t)sysconf(_SC_PAGESIZE);
	if (argc < 2) return 2;
	if (!strcmp(argv[1], "ok")) return 0;
	if (!strcmp(argv[1], "count"))
		return argc >= 3 && argc == atoi(argv[2]) ? 0 : 3;
	if (!strcmp(argv[1], "cmdline")) return check_cmdline(argc, argv);
	if (!strcmp(argv[1], "report") && argc == 4) {
		self = argv[2];
		snprintf(tmpdir, sizeof tmpdir, "%s", argv[3]);
		report();
		return 0;
	}
	return 2;
}
