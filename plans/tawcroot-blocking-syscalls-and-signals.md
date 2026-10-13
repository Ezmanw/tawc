# tawcroot: signals vs. blocking syscalls

Make signals interact with blocking syscalls the way they do on Linux:
no uninterruptible waits inside the SIGSYS handler, and no guest wait
mask that blocks SIGSYS. Resolves
[issues/tawcroot-blocking-syscalls-and-signals.md](../issues/tawcroot-blocking-syscalls-and-signals.md)
and upstream [#27](https://github.com/wmww/tawc/issues/27).

**Status: plan, not started.**

## Problem

Two gaps, both caused by the handler running with every signal masked
(`sa_mask = ~0`, needed so a nested trap can't kill the process):

- **A.** Any raw syscall the handler completes itself waits with all
  signals blocked. Only `SIGKILL` gets through. A server in `accept()`,
  a script opening a FIFO, a thread waiting for a file lock, and every
  forwarded `ioctl` (binder loopers, GPU fence waits, tty drains) can't
  be interrupted, stopped, timed out or cancelled.
- **B.** `ppoll`, `pselect6`, `epoll_pwait{,2}`, `rt_sigsuspend` and
  `io_pgetevents` aren't trapped, so a guest wait mask that includes
  SIGSYS reaches the kernel. A handler interrupting that wait runs with
  SIGSYS blocked; its first trapped syscall force-kills the process
  (exit 159). Firefox under `RLIMIT_AS` died this way.

The issue file has the measurements and the option analysis. This plan
is the resolution.

## Invariants

The plan establishes two invariants and keeps them mechanically.

1. **The handler never waits on another process, peer or device.** Any
   kernel call that can wait unboundedly on something outside this
   thread runs in guest context: at the guest's PC or from a trampoline
   on the guest stack, with the guest's mask, on the guest's stack. The
   kernel's own restart, stop/continue, nesting and `siglongjmp`
   semantics then apply unchanged.
2. **No guest-supplied sigset with SIGSYS set becomes a blocked set in
   the kernel.** Already true for `rt_sigprocmask` and `sa_mask`
   (`dd50607`); extended to every wait mask.

Waits bounded by the kernel or a filesystem (FUSE round trips, `fsync`
class, page-cache I/O) stay in the handler. They answer to `SIGKILL`
only; natively they are also mostly uninterruptible.

## The mechanism ladder

Every trapped syscall that can block is assigned to the lowest rung that
is exact. Lower is simpler and faster.

### Rung 1: don't trap it

Narrow the filter so the call never reaches the handler. Exact by
definition, and removes work from hot paths.

- `fcntl`: trap only `fd >= reserved_fd_floor` or
  `cmd ∈ {F_ADD_SEALS, F_GET_SEALS}` (the two things `handle_fcntl`
  does). `F_SETLKW`/`F_OFD_SETLKW` then wait natively.
- `ioctl`: trap only `cmd ∈ {TCGETS2, TCSETS2, TCSETSW2, TCSETSF2}` and
  the netdev-get set in `is_netdev_get_ioctl`. Everything else, binder
  and sync-fence waits included, goes to the kernel directly. The
  reserved-fd `EBADF` guard is not applied to `ioctl` today, so nothing
  is lost.
- `accept4` and x86 `accept`: stop trapping. The only in-handler work
  is reverse-translating a pathname-bound stream client's `sun_path`,
  which `recvfrom`/`recvmsg` already don't do. `getpeername` keeps
  translating, so the guest-view path is still reachable. This makes
  the most common blocking call fully native, `pthread_cancel`
  included. Verify nothing else depends on seeing accept's new fd.

The filter builder grows three predicate kinds, emitted per trapped
number: `arg_ge(k, v)` (low word), `arg_in_set(k, {...})` (low word),
and `arg_nonnull_outside(k, lo, hi)` (64-bit). Keep the generated
program well under the 4096-instruction cap; the predicate lists are
short.

### Rung 2: rewind

The handler rewrites argument registers, moves the saved PC back one
instruction (2 bytes on x86_64, 4 on aarch64; the kernel's
`syscall_rollback` already restored the number and first argument), and
returns. The guest re-executes its own syscall instruction with the
real `uc_sigmask`. The filter admits the re-execution because the
rewritten pointer lies inside a tawcroot table.

Rules that make rewind safe:

- **Register rule.** Only argument registers within the syscall's
  arity may change, and a rewritten pointer must target byte-identical
  memory. The kernel preserves every other register and compilers rely
  on that: inline-asm wrappers list only the arguments as operands, so a
  value in an unused argument register may be live. Consequences: no
  cookie in a spare register to mark rewound calls, and no x86
  `epoll_wait` to `epoll_pwait` rewrite (it would need the fifth
  argument register zeroed). The syscall-number register is also
  rewritable on x86_64 only, where the return value clobbers it anyway.
- **Immutable targets.** Rewritten memory lives in an append-only,
  deduplicated BSS table: `{ u64 mask; struct { const u64 *p; size_t n; } hdr; }`
  per entry, mask = guest mask with SIGSYS cleared. `ppoll`,
  `epoll_pwait{,2}` and `rt_sigsuspend` point at `mask`; `pselect6` and
  `io_pgetevents` point at `hdr`, the `{ptr, size}` pack the kernel
  reads. One range check covers both. 64 entries, CAS publish, linear
  dedup. A full table falls back to reissuing the wait in-handler with
  the stripped mask (degrades to problem A, never a kill).
- **Nothing to clean up.** Pending signals deliver at the guest PC
  before the re-execution; a handler that `siglongjmp`s away leaves
  nothing behind because the table is immutable.
- **Stacked-filter guard.** Android's filter may TRAP a number we only
  ALLOW, and a rewound call would then re-trap forever. A trap whose
  mask pointer is already inside our table can only come from another
  filter: complete it in-handler and mark the number non-rewindable for
  the process. Tag our own TRAPs with a nonzero `SECCOMP_RET_DATA` so
  `si_errno` distinguishes our traps from Android-only ones generally.
- `sigsetsize != 8` returns `EINVAL` from the handler, never a rewind.

Filter rule for each rung-2 number: TRAP iff the mask argument is
non-NULL and outside the table. NULL-mask `ppoll`/`epoll_pwait`
(glibc's `poll`/`epoll_wait` on aarch64) and NULL-pack `pselect6`
(glibc's `select`) stay untrapped.

### Rung 3: handoff

For calls that need a post phase or per-call scratch, run the kernel
call from a trampoline in guest context:

1. The handler builds a save block below the guest SP (below the
   x86_64 red zone, 16-byte aligned): saved guest registers, the
   rewritten arguments, scratch (translated sockaddr, copied `msghdr`,
   converted timespec, path suffix), and a post-phase selector.
2. It points SP below the block and the PC at the trampoline in the
   stub section, and returns. The trampoline loads the arguments and
   issues the syscall at a second allowlisted IP.
3. The trampoline then executes a marker syscall at a third, not
   allowlisted IP. The handler recognises the marker by
   `si_call_addr`, runs the post phase (copy back, convert, translate),
   restores the guest registers from the block, and returns to the
   guest's resume PC with the result.

Rules:

- **A handoff carries no tawcroot-owned state across the wait.**
  Everything it needs is in the save block or in process-lifetime
  tables. `siglongjmp`, `fork`, thread exit or a nested signal during
  the wait then need no cleanup. Concretely: tier-3 connect/sendto
  paths reserve the parent fd in `sock_parents` the way `bind` does
  instead of a transient fd; FIFO opens copy the path suffix into the
  block rather than holding an O_PATH fd.
- **Only when the call can block.** Gate on the fd lacking
  `O_NONBLOCK`, no `MSG_DONTWAIT`, a nonzero timeout. Fast cases stay
  in-handler as today.
- **Stacked-filter guard.** If the trampoline's syscall traps (Android
  traps the number), the handler sees a trap from the trampoline IP and
  completes the call in-handler instead of looping.
- Nested handoffs are fine: a signal handler during the wait runs
  below the block, and its own handoff goes below that.
- Guest stack cost: about 300 bytes plus scratch, up to `PATH_MAX`
  for a long FIFO path. Same order as a signal frame; the altstack
  floor in `sigalt.c` already assumes that budget.

Divergence inherent to rung 3: signals during the wait see a tawcroot
PC and registers. glibc ≥ 2.41 and musl act on `SIGCANCEL` only when
the interrupted PC is inside their own syscall bridge, so
`pthread_cancel` of a thread blocked in a handed-off call acts when the
call returns. No in-process design can fix this; only a kernel-native
supervisor could, and that is out of scope. After rung 1 this affects
only the rung-3 set, where cancellation while blocked is rare.

## Assignment

| Call | Blocks on | Rung | Notes |
|---|---|---|---|
| `fcntl(F_SETLKW, F_OFD_SETLKW)` | lock holder | 1 | filter: fd ≥ floor or seal cmds |
| `ioctl` (binder, fences, drains, anything) | device | 1 | filter: termios2 + netdev-get cmds only |
| `accept4`, x86 `accept` | peer | 1 | untrap; drop accept's reverse translation |
| `ppoll`, `epoll_pwait`, `epoll_pwait2`, `rt_sigsuspend` | wait | 2 | mask pointer → table |
| `pselect6`, `io_pgetevents` | wait | 2 | pack pointer → table `hdr` |
| x86 `poll`, `select`, `epoll_wait` | wait | 3 | Android traps these numbers, so they can't be untrapped or rewound; `select` post phase converts remaining time back to `timeval` |
| `connect`, `sendto`, `sendmsg` (pathname AF_UNIX) | peer backlog / queue | 3 | translated sockaddr or `msghdr` in the block; no transient fd |
| `openat`/`open`/`creat` of a FIFO or char device | writer/reader, carrier | 3 | one `fstatat` probe on opens lacking `O_NONBLOCK`, `O_PATH`, `O_DIRECTORY`, `O_TMPFILE`; `S_IFIFO`/`S_IFCHR` → handoff. TOCTOU degrades to today's behaviour |
| `TCSETSW2`/`TCSETSF2` legacy fallback | tty drain | 3 | only on the SELinux xperm denial path |
| `openat2` | n/a | — | already `ENOSYS` |
| `io_uring_enter` | n/a | — | already `ENOSYS` |

`read`, `write`, `recv*`, `wait4`, `futex`, `nanosleep`, `flock` and
`epoll_pwait` with a NULL mask are untrapped today and stay that way.

## Decisions stated up front

- **FIFO detection costs one `fstatat` per eligible open.** There is no
  free probe: adding `O_NONBLOCK` and clearing it afterwards changes
  FIFO semantics (a reader's open must wait for a writer). Measure with
  `tawcroot/perf`; expected a few percent of open latency under
  tawcroot, which already pays a trap plus resolution per open.
- **Accept's peer pathname is the host spelling** for stream clients
  that bound a path. Same policy as `recvfrom`; `getpeername` still
  translates. Recorded in `status.md` with the others.
- **Guest handlers see the stripped mask** in `uc_sigmask` during a
  rung-2 wait. Same as the existing `rt_sigprocmask` policy; no shadow.
- Guest code that hand-edits `uc_sigmask` to block SIGSYS before
  `sigreturn` stays unsupported. Nothing practical does it.

## Guardrails

- **Audit table** in `notes/tawcroot/sigsys-handler.md`: every raw
  syscall number the handler issues, classified bounded or
  blocking-capable, with the rung that owns it. Plus the rules: no
  unbounded blocking in the handler, the register rule, handoff carries
  no state.
- **Source-scan test** (hosted layer): fails on `TAWC_RAW(TAWC_SYS_<n>`
  for a blocking-capable `n` (`accept4`, `connect`, `sendto`,
  `sendmsg`, `ppoll`, `pselect6`, `epoll_pwait*`, `rt_sigsuspend`,
  `nanosleep`, `wait4`, `futex`, `read`, `write`, `recv*`) outside an
  allowlisted guarded call site, and requires `openat`/`ioctl`/`fcntl`
  to go through the guarded helpers. This is what stops the next
  handler from reintroducing an in-handler wait.
- **Trap-count hook** (test/debug hatch in `handler.c`) so tests can
  assert that fast paths don't trap at all.

## Phases

Each phase lands on its own with tests.

1. **Groundwork.** Filter predicate DSL + `RET_DATA` tag + unit tests
   on the generated program. PC-rewind helper per arch with a smoke
   test: trap, rewind with an in-table pointer, prove the guest
   re-executes once and the filter admits it. The audit table.
2. **Rung 1.** Narrow `fcntl`, `ioctl`; untrap `accept4`/`accept`;
   delete `handle_accept4`/`handle_accept`. Tests: `F_SETLKW` and
   `accept` get `EINTR` and `SIGTERM` default action; unknown `ioctl`
   doesn't trap; termios2 and netdev paths still work under the
   synthesized Android filter.
3. **Rung 2.** Mask table + filter rules + rewind for the six numbers.
   Tests: #27's reproducer per wait; `SA_RESTART` on and off;
   `pthread_cancel` in `ppoll` with a mask; nested handlers; remaining
   time written back by `ppoll`; NULL masks don't trap; table overflow
   fallback; the stacked-filter guard with a synthesized filter that
   traps `epoll_pwait2`.
4. **Rung 3 primitive.** Arch trampolines, second IP in the filter
   prologue, marker trap, save-block layout, handoff API
   `tawc_handoff(nr, args, post, scratch)`. First users: x86 legacy
   `poll`/`select`/`epoll_wait` (testable on the host, `select` has a
   post phase). Tests: `EINTR` vs restart, `SIGSTOP`/`SIGCONT` mid-call,
   `siglongjmp` and `fork` in a handler during the wait, nested
   handoff, remaining-time writeback for `select`, the trampoline-trap
   guard.
5. **Rung 3 users.** Unix `connect`/`sendto`/`sendmsg` with a full
   peer queue; termios2 drain fallback. Tier-3 paths move to the
   persistent parent-fd table.
6. **FIFO and char-device opens.** The probe, the handoff, and a perf
   run recorded in `tawcroot/perf/results.md`. Tests: FIFO open with
   no peer gets `EINTR`/`SIGTERM`; plain-file opens don't hand off;
   `O_NONBLOCK` opens don't probe.
7. **Docs and cleanup.** Update `sigsys-handler.md` (rules, audit
   table, trampoline contract), `seccomp-filter.md` (predicates, second
   IP, `RET_DATA`), `status.md` (residuals), delete the issue file, note
   on #27. Add the no-unbounded-blocking rule to the handler coding
   conventions.

## Test matrix

Run the same C programs natively and under tawcroot and compare exit
codes and outputs (host build, hosted/handler layers; `--device` for
both ABIs). For every call in the assignment table:

- interrupted by a handled signal, with and without `SA_RESTART`
- `SIGTERM` with default action ends the process
- `SIGSTOP`/`SIGCONT` mid-call, then completion
- `pthread_cancel` of a thread blocked in the call (expect native
  behaviour for rungs 1 and 2; deferred until return for rung 3)
- nested blocking call inside a handler that interrupted the wait
- `siglongjmp` out of such a handler; `fork` inside it
- remaining time written back (`ppoll`, `select`)
- the fast path (NULL mask, plain file, `O_NONBLOCK` fd) does not trap
- all of the above under `tests/handler/androidfilter`

## Host vs. device validation

The host build (x86_64, glibc, our filter only) validates the whole
design: all signal semantics, the x86_64 rewind and trampoline, the
mask table, the filter predicates, and the synthesized Android filter.
Develop and iterate there.

Device runs are still required before calling a phase done:

- **aarch64 (phone, `.tawctarget=physical`):** the aarch64 rewind
  (PC − 4, `x8` untouched) and trampoline, `restore_altstack`
  differences already seen between arm64 and x86 kernels, bionic-loaded
  guests, real Android kernels (5.4 through 6.x).
- **Android x86_64 (emulator):** the real stacked filter, which traps
  the legacy numbers rung 3 serves and may trap `epoll_pwait2` or
  `io_pgetevents` on some versions (exercises the stacked-filter
  guards); the SELinux termios2 xperm denial path.

`tawcroot/test.sh --device` runs the same suite on either target.
