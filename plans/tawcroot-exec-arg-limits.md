# tawcroot exec: kernel-exact argv/envp limits

Make guest `execve`/`execveat` accept exactly what the kernel accepts, with
no tawcroot-specific caps on string length, string count, or total size.
Fixes [#28](https://github.com/wmww/tawc/issues/28) (a 16 KB per-string cap
breaks `bash -c <45 KB script>` and ninja's `sh -c <24 KB link line>`).

**Status: plan, not started.**

## Problem

Every exec is pushed through fixed-size static buffers, and each buffer
has its own limit:

| Limit | Where | Kernel equivalent |
|---|---|---|
| 16 KB per string | `syscalls_exec.c` `MAX_STR` | 128 KB (`MAX_ARG_STRLEN`) |
| 64 KB argv total | `argv_strings[]` | shared argv+envp budget, ~2 MB |
| 256 KB envp total | `envp_strings[]` | same shared budget |
| 4096 args / 1024 envs | `exec_state.h` offset arrays, loader arrays | none (2^31) |
| ~84 KB cmdline | proctitle title/bounce buffers | the whole argv |

Just making the buffers bigger only moves these limits. Programs size
their command lines from `sysconf(_SC_ARG_MAX)` (= `RLIMIT_STACK/4`, so
2 MB by default): `xargs`, `find -exec {} +`, make, ninja, cmake, linkers
with response-file fallback. Any fixed limit below what the kernel allows
breaks them.

## Target semantics (mirror `fs/exec.c`)

- Each argv/envp string can be up to `MAX_ARG_STRLEN` = 131072 bytes
  including the NUL, so up to 131071 characters. Anything longer is `E2BIG`.
- Total budget: `limit = max(min(6 MiB, RLIMIT_STACK.cur / 4), 128 KiB)`.
  Count `(max(argc,1) + envc) * 8` bytes of pointers against it first
  (`E2BIG` if the pointers alone fill it). The bytes of the strings
  (path + envp + argv, NULs included) must then fit in the rest. The kernel
  copies the exec filename (`execfn`) into the same space, so the path
  counts too.
- Shebang: binfmt_script drops argv[0] and adds the interpreter, the
  optional argument and the script path, and that also has to fit the
  budget. Check again on the post-shebang argv.
- `RLIMIT_STACK` passes straight through (tawcroot doesn't emulate it), so
  read it with raw `prlimit64(0, RLIMIT_STACK, NULL, &r)` on every exec.
  `ulimit -s` in the guest then behaves as it does on Linux.
- Errno order: file lookup errors (`ENOENT`, `EACCES`, ...) come before
  `E2BIG`/`EFAULT`, and `ENOEXEC` comes after them. Today we collect strings
  first, so `execve("/missing", <huge argv>)` returns `E2BIG` where Linux
  returns `ENOENT`. Open and probe the file before collecting.

This mirrors the formula used since Linux 4.13, which covers every kernel
we target. The kernel still checks the re-exec into self (below), so any
mismatch between our pre-check and the kernel shows up as a clean `E2BIG`
to the guest, not a crash. `commit()` already returns `execveat` errors
to the caller.

## Design

### 1. Collect straight into the memfd

In `tawcroot_exec_handler_prepare` (and its callers in `syscalls_exec.c`):

1. Open and probe the file first (moved up; see errno order).
2. Read `RLIMIT_STACK` and compute `limit`.
3. `memfd_create`, `ftruncate` to a small initial size (64 KB),
   `mmap(MAP_SHARED)`.
4. Walk the guest argv/envp pointer arrays and copy each string straight
   into the mapping, checking the per-string and total limits as we go.
   When the next string doesn't fit, grow the memfd with `ftruncate` and
   the mapping with `mremap(MREMAP_MAYMOVE)`, doubling each time and
   never past what `limit` can still need. Store offsets, not pointers,
   while collecting, because the mapping can move. Growing only on demand
   means a small exec never reserves megabytes of address space, which
   could fail with `ENOMEM` under a tight `RLIMIT_AS` where Linux would
   have succeeded.
5. Append the extras (binds, shm names, identity, ...) as now, then
   `ftruncate` down to the real size. Hand the mapping (address + length)
   to `commit()` with the fd instead of unmapping it. `commit()` currently
   `mmap`s the memfd again just to read the title/envp; reusing the
   mapping saves that mmap and a `munmap`, and the `munmap`'s TLB
   shootdown in multithreaded guests. If the exec fails, `commit()`
   unmaps it.

The cost compared to today: about two `ftruncate`s instead of a `write()`
copy of the whole state. Each touched page faults once on first write,
which is the same per-byte cost as the copy it replaces.

This removes `argv_strings`, `envp_strings`, `argv_ptrs`, `envp_ptrs`,
`state_buf` and `title_buf` (about 700 KB of BSS) and every fixed cap with
them. Keep `exec_lock`: other statics are still used under it
(`classify_elf`, the bind/shm export buffers, path buffers), and removing
it isn't a goal here.

### 2. exec_state format v7: no offset arrays

Drop `argv_off[4096]` / `envp_off[1024]` and `proctitle_off`. Store
`argc`, `envc` and a byte length for each region. argv strings and envp
strings each sit back to back, so a reader rebuilds the pointers by
walking the NULs. The header shrinks from about 20 KB to a few hundred
bytes. The bind/shm arrays can keep their small fixed caps (16/64), since
they are internal and not guest-facing.

`tawcroot_exec_state_read` allocates the argv/envp pointer arrays itself
(anon `mmap`, sized from argc/envc) instead of taking `MAX_ARGS+1` caller
buffers. Validation: the counts must match the NUL counts in each region,
and each region must end in a NUL. The parent and child are always the
same binary (`/proc/self/exe`), so bumping the version is enough. No
compatibility path is needed.

### 3. Loader: no fixed arrays

- `loader_exec.c`: allocate `eff_argv` with `mmap` sized to
  `argc + 2*TAWC_SHEBANG_MAX_DEPTH + 1`, replacing the static
  `[TAWC_EXEC_EFF_ARGV_MAX+1]`. Do the same for the `argv_buf`/`envp_buf`
  statics. Delete `TAWC_EXEC_EFF_ARGV_MAX`.
- `loader_stack.c`: drop the on-stack `argv_buf[]`/`envp_buf[]`. Strings
  are laid out back to back, so pointers come from a second walk over
  the string area.
- Guest stack: map `8 MiB + round_up(strings + pointers + auxv)` instead
  of a flat 8 MiB, so a 6 MB command line doesn't eat into the guest's
  usable stack. That is slightly more generous than Linux, which counts
  args against the rlimit, and it does no harm.

### 4. Exact proctitle for any size

The kernel arg region must be exactly as large as the post-shebang
argv, but one kernel string can't go over 128 KB. Fix: split the title
over several argv strings in the re-exec. Keep the protocol arguments in
slots 1–2 so `classify_entry` doesn't change:

```
argv = [ t0, "--exec-child", "<fd>", t1, t2, ... ]   // each tN ≤ MAX_ARG_STRLEN
```

`proctitle_stash` already treats `argv[0]..argv[argc-1]` as one
contiguous region. Size the pieces so that region length = the
NUL-joined eff_argv byte count, with the same overhead math as today.
`commit()` builds the pieces in a private anon mapping, from the state's
argv and the shebang extra length that `prepare` stored. It no longer
needs a stored title string. In `proctitle.c`, `g_bounce` becomes an
anon mapping sized to the region. With this, `/proc/<pid>/cmdline` is
exact at every size, and the truncation divergence goes away.

The re-exec's own kernel cost is the post-shebang argv bytes plus the
same envp, and its pointer count is about the same or smaller. So it
fits whenever the pre-check passes.

### 5. Faster guest reads

`tawc_copy_string_from_guest` reads 256 bytes per `process_vm_readv`, so a
2 MB argv takes about 8k syscalls. Read up to the next page boundary
instead (a partial read is only possible at a page edge, which the
current loop already handles). Read the pointer arrays in page-bounded
batches instead of one pointer per syscall. This takes a 100k-arg exec
from about 100k syscalls to about 200.

### 6. Exec path length (small, same change)

The kernel rejects a path of `PATH_MAX` (4096) bytes or more with
`ENAMETOOLONG`. Copy the guest path with a `PATH_MAX` limit instead of
16 KB. The `resolved` buffer for dirfd-relative `execveat` stays larger,
since a joined path can legally go over `PATH_MAX`.

## Tests

Host tests (`tawcroot/test.sh`), in `test_exec_via_handler.c` unless noted.
Compare against the kernel, not against constants:

- One argument of 131071 characters works; 131072 gives `E2BIG`. Same
  for one env string.
- Total argv+envp just under the computed `limit` works, and just over
  gives `E2BIG`. Run once at the default rlimit and once under a lowered
  `RLIMIT_STACK` (e.g. 1 MiB → 256 KB limit) set in the child before the
  exec.
- 100k short arguments (well past the old 4096) round-trip.
- `/proc/self/cmdline` of the exec'd guest matches the NUL-joined argv
  exactly, for 300 KB and 2 MB argvs, both with and without a shebang.
- Shebang near the limit: a script exec where the post-shebang argv goes
  over `limit` gives `E2BIG` before commit (the caller survives).
- `execve("/nonexistent", <over-limit argv>)` gives `ENOENT`.
- A small exec works under a tight `RLIMIT_AS`, and a multi-MB argv
  exec still works, which exercises the `mremap` growth path.
- `xargs --show-limits` / `getconf ARG_MAX` inside the guest agree with
  what actually works: `xargs` fed 5 MB of names runs without `E2BIG`.
- Unit: v7 `exec_state` round-trip and rejecting malformed input
  (count/NUL mismatch, unterminated region). Update `test_loader_stack`
  (currently built around `TAWC_EXEC_EFF_ARGV_MAX`) and `test_ro_binds`
  (fixed `MAX_ARGS` buffers).
- The existing concurrent-exec and posix_spawn tests must stay green;
  the lock behaviour doesn't change.

On the device (`.tawctarget`): the #28 repro, a `bash -c` script of
about 100 KB, and `xargs` over a large `find`.

## Docs

- `notes/tawcroot/status.md`: remove any remaining argv-limit or cmdline
  divergences, and record that limits now follow `RLIMIT_STACK`.
- `notes/tawcroot/sigsys-handler.md` / `architecture.md`: the exec_state
  v7 layout, the split-title re-exec argv, and collecting into the memfd.
- Reply on #28 when done. Credit the reporter's repro and test ideas.

## Order

1. Format v7 + reader/writer + unit tests (both sides fixed-cap-free).
2. Collect into the memfd + kernel-formula pre-check + errno order.
3. Loader arrays + stack sizing.
4. Split-title proctitle.
5. Usercopy batching, path limit.
6. Device check, docs, then delete this plan.

Steps 1–3 have to land together, since any fixed cap left in the chain
kills processes after the commit point instead of returning `E2BIG`.
Steps 4–5 can land separately.
