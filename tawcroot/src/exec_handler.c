/* SIGSYS-handler-side execve interception. See include/exec_handler.h.
 *
 * Async-signal-safe; uses raw_sys.h for every syscall. */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "exec_handler.h"
#include "exec_state.h"
#include "identity.h"
#include "io.h"
#include "linkstore.h"
#include "loader_elf.h"
#include "loader_exec.h"
#include "loader_map.h"
#include "path.h"
#include "proc_shadow.h"
#include "raw_sys.h"
#include "shm.h"
#include "syscalls_fs.h"
#include "tawc_uapi.h"
#include "usercopy.h"


#define MiB (1024u * 1024u)
/* include/uapi/linux/limits.h ARG_MAX: the budget floor. */
#define KERNEL_ARG_MAX   (128u * 1024u)
#define RLIMIT_STACK_RES 3
#define MREMAP_MAYMOVE   1
#define MAP_SHARED_      0x01
/* First size of the state mapping; it doubles on demand. */
#define STATE_INITIAL    (64u * 1024u)

/* MAX_ARG_STRLEN: 32 pages, NUL included. */
static size_t max_arg_strlen(void)
{
	return 32 * tawcroot_host_page_size();
}

/* fs/exec.c bprm_stack_limits: the argv+envp budget, read fresh from
 * RLIMIT_STACK on every exec (tawcroot passes it straight through). */
static size_t arg_budget(void)
{
	struct { unsigned long long cur, max; } rl;
	size_t limit = 6 * MiB;   /* _STK_LIM / 4 * 3 */
	if (tawc_getrlimit64(RLIMIT_STACK_RES, &rl) == 0 && rl.cur / 4 < limit)
		limit = (size_t)(rl.cur / 4);
	return limit < KERNEL_ARG_MAX ? KERNEL_ARG_MAX : limit;
}

/* Validate an exec-probe fd the way execve(2) would validate the file:
 * directories are EISDIR, non-regular files and files with no execute
 * bit at all are EACCES. The O_RDONLY open alone passes for mode-644
 * files and directories, and by the time the loader discovers the
 * problem (post-execveat) the calling program is already gone — `bash
 * -c /etc` must get a clean error, not a destroyed shell. */
static long probe_check_executable(int fd)
{
	struct statx stx;
	long sr = tawc_statx(fd, "", AT_EMPTY_PATH, STATX_TYPE | STATX_MODE,
	                     &stx);
	if (sr < 0) return sr;
	if (S_ISDIR(stx.stx_mode)) return TAWC_EISDIR;
	if (!S_ISREG(stx.stx_mode)) return TAWC_EACCES;
	if ((stx.stx_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
		return TAWC_EACCES;
	return 0;
}


/* Require the file at `fd` to parse as an ET_EXEC/ET_DYN ELF for this
 * machine. A wrong-arch / malformed / too-short file is -ENOEXEC, which
 * is what the kernel returns and what makes a shell fall back to `sh
 * file`. */
static long classify_elf(int fd)
{
	uint8_t ebuf[sizeof(tawc_elf64_ehdr)];
	long n = tawc_pread64(fd, ebuf, sizeof ebuf, 0);
	if (n != (long)sizeof ebuf) return TAWC_ENOEXEC;
	/* Static: ~900 bytes, and this sits at the bottom of the recursive
	 * shebang chain on the guest stack. Caller holds exec_lock. */
	static struct tawc_loader_image img;
	if (tawc_loader_parse_ehdr(ebuf, sizeof ebuf, &img) != 0)
		return TAWC_ENOEXEC;  /* bad magic / class / unknown machine */
	/* The parser accepts both supported machines (its unit tests are
	 * cross-arch); the RUN decision is ours. Without this, an x86_64
	 * binary in an aarch64 rootfs passes the probe and the loader
	 * jumps into foreign code post-commit — SIGILL instead of the
	 * kernel-shaped ENOEXEC a shell needs for its fallback. */
	if (img.e_machine != TAWC_EM_HOST) return TAWC_ENOEXEC;
	if (img.e_type != TAWC_ET_EXEC && img.e_type != TAWC_ET_DYN)
		return TAWC_ENOEXEC;
	return 0;
}

/* Shebang accounting for classify_loadable. binfmt_script drops
 * argv[0] and adds the script path, the interpreter and its optional
 * argument; the kernel charges those against the same budget as the
 * caller's strings, so a near-limit script exec is E2BIG. */
struct shebang_acct {
	size_t extra;       /* interpreter + arg bytes added, all levels */
	size_t argv0_bytes; /* caller's argv[0] incl. NUL ("" if argc 0) */
	size_t path_bytes;  /* exec path incl. NUL */
	size_t left;        /* budget remaining after the caller's strings */
};

/* Classify whether the open file is something the loader can actually
 * run, BEFORE the execveat commit point. Without this, problems the
 * loader only discovers post-commit (a `chmod +x`'d text file, a
 * shebang whose interpreter is missing, a wrong-arch ELF) kill the
 * calling program with a loader exit code instead of returning a clean
 * execve errno. Resolves the shebang chain the same way the kernel and
 * loader_exec.c do; the final file must be a valid ELF.
 *
 * Note: a narrow TOCTOU window remains (the file could be replaced
 * between this probe and the child's open), same as for the existing
 * executable-bit probe — but that window is far narrower than the
 * always-dies-post-commit behaviour it replaces. */
static long classify_loadable(int fd, int depth, struct shebang_acct *a)
{
	if (depth > TAWC_SHEBANG_MAX_DEPTH) return TAWC_ELOOP;

	uint8_t magic[2];
	long n = tawc_pread64(fd, magic, 2, 0);
	if (n < 0) return n;
	if (n < 2 || !(magic[0] == '#' && magic[1] == '!'))
		return classify_elf(fd);

	/* Shebang: parse the line, charge the rewrite against the budget,
	 * open the interpreter through the view, require it executable,
	 * and recurse. Same order as binfmt_script. */
	char line[TAWC_SHEBANG_BUF];
	const char *interp;
	const char *sarg;
	long pe = tawcroot_shebang_read(fd, line, sizeof line, &interp, &sarg);
	if (pe < 0) return pe;
	size_t add = tawc_strlen(interp) + 1 + (sarg ? tawc_strlen(sarg) + 1 : 0);
	/* Level 0 swaps argv[0] for the path; deeper levels swap the
	 * previous interpreter for itself. */
	if (depth == 0) {
		a->left += a->argv0_bytes;
		if (a->left < a->path_bytes) return TAWC_E2BIG;
		a->left -= a->path_bytes;
	}
	if (a->left < add) return TAWC_E2BIG;
	a->left  -= add;
	a->extra += add;

	long ifd = tawcroot_open_in_view(interp);
	if (ifd < 0) return ifd;  /* missing interpreter → ENOENT, etc. */
	long ck = probe_check_executable((int)ifd);
	if (ck == 0) ck = classify_loadable((int)ifd, depth + 1, a);
	tawc_close((int)ifd);
	return ck;
}

/* The growing state memfd + its MAP_SHARED mapping. */
struct collect {
	int      fd;
	uint8_t *base;
	size_t   cap;
	size_t   len;
	size_t   soft_max;   /* doubling stops here; exact growth after */
};

static long collect_grow(struct collect *c, size_t need)
{
	if (need <= c->cap) return 0;
	size_t nc = c->cap;
	while (nc < need) nc *= 2;
	if (nc > c->soft_max) nc = need > c->soft_max ? need : c->soft_max;
	nc = (nc + 4095) & ~(size_t)4095;
	long r = tawc_ftruncate(c->fd, (long)nc);
	if (r < 0) return r;
	long m = tawc_mremap(c->base, c->cap, nc, MREMAP_MAYMOVE);
	if (tawc_loader_mmap_is_err((uintptr_t)m)) return m;
	c->base = (uint8_t *)(uintptr_t)m;
	c->cap  = nc;
	return 0;
}

/* Guest-memory readers for the argv/envp walk: one for the pointer
 * arrays, one for the strings, so the two walks don't evict each
 * other's page. Static (4 KiB each, over the handler frame cap);
 * callers hold exec_lock. */
static struct tawc_guest_reader g_ptr_reader;
static struct tawc_guest_reader g_str_reader;

/* Count a guest pointer array (NULL = empty). Stops with -E2BIG past
 * `max` entries — the pointers alone would already blow the budget. */
static long count_array(const char *const *arr, size_t max, size_t *out)
{
	size_t n = 0;
	if (arr) {
		for (;;) {
			uintptr_t p;
			if (tawc_guest_read(&g_ptr_reader, &p, sizeof p, &arr[n]) < 0)
				return TAWC_EFAULT;
			if (!p) break;
			if (++n > max) return TAWC_E2BIG;
		}
	}
	*out = n;
	return 0;
}

/* Copy n guest strings from `arr` to the end of the state, charging
 * each (NUL included) against *left. */
static long collect_strings(struct collect *c, const char *const *arr,
                            size_t n, size_t *left)
{
	size_t smax = max_arg_strlen();
	for (size_t i = 0; i < n; i++) {
		uintptr_t p;
		if (tawc_guest_read(&g_ptr_reader, &p, sizeof p, &arr[i]) < 0)
			return TAWC_EFAULT;
		/* NULL here means the guest rewrote its array mid-exec. */
		if (!p) return TAWC_EFAULT;
		size_t lim = smax < *left ? smax : *left;
		for (;;) {
			size_t room = c->cap - c->len;
			size_t cap = room < lim ? room : lim;
			long sn = tawc_guest_read_string(&g_str_reader,
			                                 (char *)c->base + c->len,
			                                 cap, (const char *)p);
			if (sn >= 0) {
				c->len += (size_t)sn + 1;
				*left  -= (size_t)sn + 1;
				break;
			}
			if (sn != TAWC_ENAMETOOLONG) return sn;
			if (cap == lim) return TAWC_E2BIG;
			long g = collect_grow(c, c->cap + 1);
			if (g < 0) return g;
		}
	}
	return 0;
}

long tawcroot_exec_handler_prepare(const char *path,
                                   const char *const *argv,
                                   const char *const *envp,
                                   struct tawcroot_exec_prep *prep)
{
	if (!path || !prep) return TAWC_EINVAL;

	/* execve("/proc/self/exe", …): the kernel-level link points at
	 * libtawcroot.so, which the loader would then try to map over the
	 * running tawcroot image (LOADER_FAIL 68). Substitute the stashed
	 * guest exe path — the same synthesis readlinkat performs — so the
	 * guest re-execs the binary it believes it is running. Load-bearing
	 * for busybox's standalone-shell mode (Debian's config), where ash
	 * runs nearly every applet via a /proc/self/exe re-exec. */
	if (tawcroot_guest_exe_path_len > 0 && tawcroot_is_proc_self_exe(path))
		path = tawcroot_guest_exe_path;

	/* (1) Open + probe the file first: like the kernel, lookup and
	 * permission errors win over E2BIG. In rootfs mode the path is
	 * guest-relative and the open goes through the view; in legacy
	 * --exec-via-handler mode it is a host path. */
	long probe = tawcroot_open_in_view(path);
	if (probe < 0) return probe;
	long r = probe_check_executable((int)probe);
	if (r < 0) { tawc_close((int)probe); return r; }

	/* (2) Count, then check the pointer budget (bprm_stack_limits). */
	g_ptr_reader.len = 0;
	g_str_reader.len = 0;
	size_t limit = arg_budget();
	size_t argc = 0, envc = 0;
	r = count_array(argv, limit / sizeof(void *), &argc);
	if (r == 0) r = count_array(envp, limit / sizeof(void *), &envc);
	if (r < 0) { tawc_close((int)probe); return r; }
	size_t ptrs = ((argc ? argc : 1) + envc) * sizeof(void *);
	size_t path_bytes = tawc_strlen(path) + 1;
	/* Strings budget: the exec filename and, for argc == 0, the ""
	 * the kernel substitutes as argv[0] are charged too. */
	size_t charged = path_bytes + (argc ? 0 : 1);
	if (limit <= ptrs || limit - ptrs < charged) {
		tawc_close((int)probe);
		return TAWC_E2BIG;
	}
	size_t left = limit - ptrs - charged;

	/* (3) Collect into a non-CLOEXEC memfd (it must survive the
	 * execveat into our own re-exec), growing it on demand so a small
	 * exec never maps megabytes. */
	struct collect c = { .fd = -1 };
	long mfd = tawc_memfd_create("tawcroot-exec-state", 0);
	if (mfd < 0) { tawc_close((int)probe); return mfd; }
	c.fd = (int)mfd;
	c.soft_max = sizeof(tawcroot_exec_state_header) + limit + 64 * 1024;
	r = tawc_ftruncate(c.fd, STATE_INITIAL);
	if (r == 0) {
		long m = tawc_mmap((void *)0, STATE_INITIAL,
		                   TAWC_MM_PROT_READ | TAWC_MM_PROT_WRITE,
		                   MAP_SHARED_, c.fd, 0);
		if (tawc_loader_mmap_is_err((uintptr_t)m)) r = m;
		else { c.base = (uint8_t *)(uintptr_t)m; c.cap = STATE_INITIAL; }
	}
	/* 64 KiB always holds the header + a path (≤ 16 KiB). */
	if (r == 0) r = tawcroot_exec_state_begin(c.base, c.cap, &c.len, path);
	size_t argv_off = c.len;
	if (r == 0) r = collect_strings(&c, argv, argc, &left);
	size_t argv_bytes = c.len - argv_off;
	if (r == 0) r = collect_strings(&c, envp, envc, &left);
	size_t envp_bytes = c.len - argv_off - argv_bytes;

	/* (4) Binary format, after the size checks like the kernel's
	 * search_binary_handler: ENOEXEC, shebang interpreter errors, and
	 * E2BIG for a rewrite that no longer fits. */
	struct shebang_acct sa = {
		.argv0_bytes = 1,
		.path_bytes  = path_bytes,
		.left        = left,
	};
	if (r == 0) {
		if (argc)
			sa.argv0_bytes = tawc_strlen((const char *)c.base +
			                             argv_off) + 1;
		r = classify_loadable((int)probe, 0, &sa);
	}
	tawc_close((int)probe);

	/* cmdline_bytes: the NUL-joined argv the loader will install, which
	 * commit() sizes the re-exec's arg region to (proctitle.h):
	 *   shebang:     interp/arg prepends + path + argv[1..]
	 *   non-shebang: argv[0] + argv[1..]  ("" when argc == 0) */
	size_t cmdline = (argc ? argv_bytes : 1);
	if (sa.extra > 0)
		cmdline = cmdline - sa.argv0_bytes + path_bytes + sa.extra;

	/* (5) Extras. When tawcroot has a rootfs view set
	 * up (production -r mode), capture rootfs path + bind specs +
	 * guest_exe so --exec-child can re-establish the view in the new
	 * tawcroot incarnation. Without these, the new process would run
	 * handler-less and against the host filesystem — guest's
	 * post-exec syscalls would either crash (no SIGSYS handler) or
	 * route to host paths (no rootfs_fd). */
	/* Static: over a kilobyte of pointer/name staging would bust the
	 * handler frame cap (stack_budget.h). Exec stages through static
	 * buffers — callers hold exec_lock (thread-safety note in
	 * exec_handler.h). */
	static const char *bind_src_arr[TAWCROOT_EXEC_STATE_MAX_BINDS];
	static const char *bind_dst_arr[TAWCROOT_EXEC_STATE_MAX_BINDS];
	static unsigned char bind_ro_arr[TAWCROOT_EXEC_STATE_MAX_BINDS];
	static char shm_name_buf[TAWCROOT_EXEC_STATE_MAX_SHM]
	                        [TAWCROOT_SHM_NAME_MAX + 1];
	static const char *shm_name_arr[TAWCROOT_EXEC_STATE_MAX_SHM];
	static int         shm_fd_arr[TAWCROOT_EXEC_STATE_MAX_SHM];
	tawcroot_exec_state_extras extras = { 0 };

	/* Virtual identity survives execve (fork inherits it for free;
	 * exec must ferry it — a dropped sshd session exec'ing the user's
	 * shell must not come back as fake root). Carried in both rootfs
	 * and legacy --exec-via-handler modes. */
	tawc_identity ident_snap;
	tawcroot_identity_get(&ident_snap);
	extras.identity = &ident_snap;

	if (tawcroot_rootfs_fd >= 0 && tawcroot_rootfs_host_path_len > 0) {
		extras.rootfs_host = tawcroot_rootfs_host_path;
		/* Don't ferry the parent's stashed guest_exe through. After
		 * the guest's execve, /proc/self/exe should resolve to the
		 * newly-exec'd binary, not the original tawcroot argv. With
		 * `extras.guest_exe == NULL`, --exec-child's
		 * `tawcroot_loader_exec_child` falls back to `st.path` — the
		 * exec target — which is the correct value for AT_EXECFN /
		 * /proc/self/exe synthesis. Without this, Firefox's stub binary
		 * (which looks at /proc/self/exe to find its own dir, then
		 * dlopen's libxul.so relative to it) sees /bin/bash and prints
		 * "Couldn't load XPCOM." */
		extras.guest_exe   = (const char *)0;
		/* Hardlink-emulation store: ferry the ORIGINAL store path.
		 * Deriving from rootfs_host in the child would be wrong
		 * after a guest chroot (rootfs_host is then the chrooted
		 * root, not the store's sibling). */
		extras.store_host = tawcroot_store_host_path_len > 0
			? tawcroot_store_host_path : (const char *)0;
		/* Source host paths come straight off the bind table.
		 * Earlier revisions recovered them via readlinkat
		 * /proc/self/fd/<src_fd>, which broke once gpgme's fork-child
		 * closefrom() raced against the recovery readlink — keeping
		 * the strings around makes the dependency on a live src_fd
		 * disappear.
		 *
		 * No lock needed for the bind-table snapshot itself: the
		 * append-only contract is no longer strictly true (chroot
		 * mutates `active` and rewrites `dst` in place for
		 * surviving binds), but a re-anchor only runs when the
		 * guest issues `chroot()` — chroot is itself trapped, so a
		 * concurrent execve trap on another thread would have to
		 * race AGAINST chroot mid-rewrite. The window is microseconds
		 * and there's no realistic workload that issues chroot from
		 * one thread while another execs (pacman 6.x's chroot is
		 * single-threaded, post-startup). shm by contrast is mutated
		 * by the guest at runtime (shm_open / shm_unlink
		 * interception), so its snapshot needs the export lock.
		 *
		 * Inactive binds (deactivated by chroot when they fell
		 * outside the new view) are SKIPPED here. Their src_fd
		 * stays reserved in the parent, but their dst was cleared
		 * to ""; passing them to --exec-child would trip
		 * tawcroot_path_add_bind's empty-dst -EINVAL. */
		uint32_t nb = 0;
		for (size_t i = 0; i < tawcroot_n_binds &&
				   nb < TAWCROOT_EXEC_STATE_MAX_BINDS; i++) {
			if (!tawcroot_binds[i].active) continue;
			bind_src_arr[nb] = tawcroot_binds[i].src;
			bind_dst_arr[nb] = tawcroot_binds[i].dst;
			bind_ro_arr[nb]  = (unsigned char)
				tawcroot_binds[i].read_only;
			nb++;
		}
		extras.n_binds  = nb;
		extras.bind_src = bind_src_arr;
		extras.bind_dst = bind_dst_arr;
		extras.bind_ro  = bind_ro_arr;
		extras.root_ro  = (uint32_t)tawcroot_root_ro;

		/* /dev/shm name table. The internal memfds are non-CLOEXEC
		 * and survive execveat as the same fd numbers, so the new
		 * tawcroot just re-registers (name, fd) without re-creating
		 * any kernel-side memfd object. Snapshot under one lock so
		 * concurrent shm ops on other threads can't shift indices
		 * mid-export. */
		size_t shm_n = tawcroot_shm_export_all(
			shm_name_buf, shm_fd_arr,
			TAWCROOT_EXEC_STATE_MAX_SHM);
		for (size_t i = 0; i < shm_n; i++)
			shm_name_arr[i] = shm_name_buf[i];
		extras.n_shm    = (uint32_t)shm_n;
		extras.shm_name = shm_name_arr;
		extras.shm_fd   = shm_fd_arr;
	}

	if (r == 0) {
		tawcroot_exec_state_set_args(c.base, argv_off, (uint32_t)argc,
		                             argv_bytes, (uint32_t)envc,
		                             envp_bytes, (uint32_t)cmdline);
		r = collect_grow(&c, c.len +
		                 tawcroot_exec_state_extras_bytes(&extras));
	}
	if (r == 0) r = tawcroot_exec_state_finish(c.base, c.cap, &c.len,
	                                           &extras);
	/* Trim the memfd to the state; the child sizes it with lseek. */
	if (r == 0) r = tawc_ftruncate(c.fd, (long)c.len);
	if (r < 0) {
		if (c.base) (void)tawc_munmap(c.base, c.cap);
		tawc_close(c.fd);
		return r;
	}
	prep->mfd     = c.fd;
	prep->map     = c.base;
	prep->map_cap = c.cap;
	prep->len     = c.len;
	return 0;
}

static void prep_release(struct tawcroot_exec_prep *prep)
{
	(void)tawc_munmap(prep->map, prep->map_cap);
	tawc_close(prep->mfd);
}

static void *anon_map(size_t len)
{
	long m = tawc_mmap((void *)0, len,
	                   TAWC_MM_PROT_READ | TAWC_MM_PROT_WRITE,
	                   TAWC_MM_MAP_PRIVATE | TAWC_MM_MAP_ANON, -1, 0);
	return tawc_loader_mmap_is_err((uintptr_t)m) ? (void *)0
	                                             : (void *)(uintptr_t)m;
}

long tawcroot_exec_handler_commit(struct tawcroot_exec_prep *prep)
{
	/* Open /proc/self/exe so we can execveat ourselves with
	 * AT_EMPTY_PATH: our own filesystem path depends on how tawcroot
	 * was invoked (the APK's libtawcroot.so, a test scratch dir), and
	 * /proc/self/exe always names the running binary. */
	long exe_fd = tawc_openat(AT_FDCWD, "/proc/self/exe",
	                          O_RDONLY | O_CLOEXEC, 0);
	if (exe_fd < 0) {
		prep_release(prep);
		return TAWC_ENOEXEC;
	}

	char fdstr[24];
	int fdlen = tawc_int_to_str(fdstr, sizeof fdstr, prep->mfd);
	if (fdlen <= 0) {
		tawc_close((int)exe_fd);
		prep_release(prep);
		return TAWC_EFAULT;
	}

	const tawcroot_exec_state_header *h =
		(const tawcroot_exec_state_header *)prep->map;
	const char *strings = (const char *)prep->map + sizeof *h;

	/* argv for the re-exec: [t0, "--exec-child", fd, t1, t2, ...].
	 *
	 * The title pieces tN size the new process's kernel arg region
	 * (argv[0] .. argv[argc-1], contiguous) to exactly cmdline_bytes,
	 * so the loader can overwrite it in place with the guest's real
	 * NUL-joined argv (proctitle.h); without them every guest's
	 * /proc/<pid>/cmdline reads "tawcroot --exec-child <fd>". One
	 * kernel string is capped at MAX_ARG_STRLEN, so a long cmdline
	 * spans several pieces. The protocol args stay in slots 1-2 for
	 * classify_entry. Content is visible only until the loader's
	 * rewrite: the guest argv space-joined, then space padding.
	 * Cmdlines shorter than the protocol args keep a few trailing
	 * NULs — nothing to shrink below them. */
	size_t overhead = sizeof "--exec-child" + (size_t)fdlen + 1;
	size_t title = h->cmdline_bytes > overhead + 1
		? h->cmdline_bytes - overhead : 1;
	size_t smax = max_arg_strlen();
	size_t npieces = (title + smax - 1) / smax;
	size_t argv_len = (npieces + 3) * sizeof(char *);
	size_t envp_len = ((size_t)h->envc + 1) * sizeof(char *);
	size_t scratch_len = argv_len + envp_len + title;
	/* Private anon scratch: commit runs unlocked and may touch no
	 * shared statics (see header note). */
	uint8_t *scratch = anon_map(scratch_len);
	if (!scratch) {
		tawc_close((int)exe_fd);
		prep_release(prep);
		return TAWC_ENOMEM;
	}
	char **new_argv = (char **)scratch;
	char **new_envp = (char **)(scratch + argv_len);
	char *tbuf = (char *)(scratch + argv_len + envp_len);

	const char *src = strings + h->argv_off;
	size_t src_left = h->argv_bytes;
	size_t pos = 0;
	for (size_t i = 0; i < npieces; i++) {
		size_t n = title - pos < smax ? title - pos : smax;
		char *piece = tbuf + pos;
		for (size_t k = 0; k + 1 < n; k++) {
			char ch = ' ';
			if (src_left > 0) { ch = *src ? *src : ' '; src++; src_left--; }
			piece[k] = ch;
		}
		piece[n - 1] = 0;
		pos += n;
		size_t slot = i == 0 ? 0 : i + 2;
		new_argv[slot] = piece;
	}
	new_argv[1] = (char *)"--exec-child";
	new_argv[2] = fdstr;
	new_argv[npieces + 2] = (char *)0;

	/* envp for the re-exec: the guest's envp, so the kernel-built env
	 * region (/proc/<pid>/environ, ps e) carries the guest environment.
	 * The synthesized guest stack gets its envp from the same state,
	 * so the two views agree byte for byte. The new tawcroot never
	 * reads config from its environ (Environment rule,
	 * notes/tawcroot/architecture.md). */
	tawcroot_exec_state_split(strings + h->envp_off, h->envc,
	                          (const char **)new_envp);

	long er = tawc_execveat((int)exe_fd, "", new_argv, new_envp,
	                        AT_EMPTY_PATH);
	/* On success execveat does not return. On failure er is -errno. */
	(void)tawc_munmap(scratch, scratch_len);
	tawc_close((int)exe_fd);
	prep_release(prep);
	return er;
}

long tawcroot_exec_handler_perform(const char *path,
                                   const char *const *argv,
                                   const char *const *envp)
{
	/* argv/envp are ours, but prepare reads them as guest memory. */
	if (!tawc_usercopy_works) {
		long u = tawc_usercopy_init();
		if (u < 0) return u;
	}
	struct tawcroot_exec_prep prep;
	long r = tawcroot_exec_handler_prepare(path, argv, envp, &prep);
	if (r < 0) return r;
	return tawcroot_exec_handler_commit(&prep);
}
