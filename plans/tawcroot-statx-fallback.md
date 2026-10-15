# tawcroot: work without statx (kernels < 4.11)

Make tawcroot work on kernels that lack `statx` (added in 4.11). Many
Android 11 phones ship 4.9. Resolves upstream
[#30](https://github.com/wmww/tawc/issues/30).

**Status: plan, not started.**

## Problem

tawcroot issues raw `statx` from its own code with no fallback. On a
kernel without it the call returns ENOSYS and that error leaks to the
guest. The first casualty is exec: `probe_check_executable`
(`src/exec_handler.c`) statx'es the target before every exec, so every
`execve` fails with ENOSYS. `apt-get update` dies with
`env: '/bin/bash': Function not implemented`, exit 126.

The issue guesses seccomp. It isn't: the filter allows the call and
the kernel doesn't implement it.

Reproduced on the host: run host tawcroot under a stacked seccomp
filter that returns `ERRNO(ENOSYS)` for `statx` (ERRNO outranks the
handler's ALLOW, TRAP outranks ERRNO, so guest statx still traps and
only tawcroot's own raw calls see ENOSYS — exactly the old-kernel
shape). `env bash -c 'echo ok'` fails as above. The same command
without tawcroot works. `ls`/`stat` in the guest still work, since
coreutils falls back to `fstatat` on ENOSYS.

Not to be confused with `084aeeb` (synthesize `STATX_MNT_ID` on
pre-5.8 kernels). That assumes `statx` exists and fills one field.

## Raw statx call sites

- `src/exec_handler.c` `probe_check_executable` — exec probe (the #30 bug).
- `src/syscalls_stat.c` `handle_statx` — AT_EMPTY_PATH and path
  branches, plus the emulated-hardlink object stat.
- `src/syscalls_stat.c` `tawcroot_statx_fill_mnt_id` — dev/ino recheck
  of the re-opened leaf.
- `src/shm.c` — shm entry statx and the mount-id probe.
- `src/proc_shadow.c` `proc_dev_probe` — procfs dev number.

Re-grep `TAWC_SYS_statx` before starting; the list may have grown.

## Fix

1. **One helper.** Add `tawc_statx(dirfd, path, flags, mask, buf)`
   with raw-statx semantics. It calls `statx`. On ENOSYS it sets a
   process-global "no statx" flag (re-learned after each exec) and
   from then on goes straight to `newfstatat`:
   - Strip `AT_STATX_SYNC_TYPE` bits; keep `AT_EMPTY_PATH`,
     `AT_SYMLINK_NOFOLLOW`, `AT_NO_AUTOMOUNT`. Anything else is
     `EINVAL`, as the kernel would.
   - Convert `struct stat` → `struct statx`: zero the struct, copy
     fields, split `st_dev`/`st_rdev` into major/minor, times into
     `statx_timestamp`.
   - `stx_mask = STATX_BASIC_STATS`. No btime, no mnt id, no
     attributes. `stx_blksize`/`stx_blocks` as fstatat reports them.
2. **Switch every call site** to the helper. Callers keep their logic.
3. **Guest statx** goes through `handle_statx`, which now uses the
   helper, so guests get a working `statx` even on 4.9. That matters
   beyond exec: systemd ≥260 requires `statx` with `STATX_MNT_ID`.
   The existing fdinfo mount-id fill (`tawcroot_statx_fill_mnt_id`)
   still runs on top; `/proc/self/fdinfo` has `mnt_id` since 3.15.
4. Nothing new in the seccomp filter: `newfstatat` is already used
   raw by the handlers.

## Tests

- Add `--no-statx` to `tests/handler/androidfilter/wrap.c`: an extra
  `RET_ERRNO|ENOSYS` rule for `statx`. Run the existing
  androidfilter handler surface under it.
- New cases under it: exec of a dynamic binary and a shebang script;
  exec of a dir (EISDIR) and a non-executable file (EACCES); guest
  `statx` of a file, `AT_EMPTY_PATH` fd, symlink with NOFOLLOW,
  `/dev/shm` entry, `/proc` shadow entry; `STATX_MNT_ID` still filled.
- Unit-test the stat → statx conversion (dev split, timestamps, mask).
- Run `tawcroot/test.sh --host` and `--device` on the phone; the
  normal-kernel path must be unchanged.

## Out of scope / unknowns

- 4.9 may lack other syscalls tawcroot or the guest rely on. No
  device that old is available, so after this lands #30's exact
  symptom is fixed but full Android 11 + sid support is unverified.
  Ask the reporter to retest and file what breaks next.
- Worth auditing in the same pass: other raw syscalls newer than 4.9
  that tawcroot issues without a fallback. Quick check: grep
  `TAWC_RAW(TAWC_SYS_` and compare each against its kernel version.
  Known-safe so far: `memfd_create` 3.17, `execveat` 3.19,
  `renameat2` 3.15. `openat2`/`faccessat2`/`clone3`/`close_range`
  already have ENOSYS paths.

## Docs

On completion: add the minimum-kernel story to
`notes/tawcroot/status.md` (and `notes/tawcroot/testing.md` for
`--no-statx`), delete this plan.
