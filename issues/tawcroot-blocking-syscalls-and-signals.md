# tawcroot: signals vs. blocking syscalls

Two related gaps between tawcroot and Linux in how signals interact with
blocking syscalls. Upstream: [#27](https://github.com/wmww/tawc/issues/27)
(problem B).

## A. Blocking calls completed inside the handler ignore signals

The SIGSYS handler runs with every signal masked (`handler.c`,
`sa_mask = ~0`), so that a nested trap can't kill the process. Any
blocking raw syscall the handler issues therefore waits with all signals
blocked. Only `SIGKILL` gets through. Signals like `SIGINT`, `SIGTERM`
and `SIGCHLD` stay pending until the call finishes by itself.

Measured on the x86_64 host build (`SIGALRM` after 1 s, no `SA_RESTART`):

| Call | Linux | tawcroot |
|---|---|---|
| `open` FIFO, no writer | `EINTR` at 1 s | hangs; `SIGTERM` ignored, needed `SIGKILL` |
| `accept` on a blocking unix socket | `EINTR` | hangs; `SIGTERM` ignored |
| `fcntl(F_SETLKW)` on a contended lock | `EINTR` | waits until the lock is free, then 0 |
| x86 `poll` / `epoll_wait` (legacy redirects) | `EINTR` | waits the full timeout |

Only syscalls the handler completes itself are affected. Untrapped
blocking calls (`read`, `write`, `nanosleep`, `wait4`, `futex`, and on
aarch64 `ppoll`/`epoll_pwait`) behave normally: Ctrl-C on `sleep 10`
works. Calls that complete inside the handler and can block without a
time limit:
- FIFO `open`/`openat`/`openat2`/`creat`
- `accept`/`accept4` on a blocking socket
- blocking `connect`/`sendmsg`/`sendto`
- `fcntl(F_SETLKW / F_OFD_SETLKW)`
- tty-drain `ioctl`s (`TCSETSW*`, `TCSETSF*`, `TCSBRK*`, `TCDRAIN`)
- the x86_64 `poll`/`select`/`epoll_wait` redirects

`pthread_cancel` of a thread blocked in one of these doesn't work either,
since `SIGCANCEL` is masked too.

User-visible results:
- A server in `accept()` can't be stopped with Ctrl-C, `kill` or
  `timeout`.
- The same goes for a script blocked opening a FIFO, or a program
  waiting on a file lock.
- A worker thread blocked in one of these calls can't be cancelled.

## B. SIGSYS in a temporary wait mask kills the process (#27)

`ppoll`, `pselect6`, `epoll_pwait{,2}`, `rt_sigsuspend` and
`io_pgetevents` aren't trapped, so the guest's wait mask reaches the
kernel unchanged. A handler that interrupts a wait whose mask includes
SIGSYS ("everything but the awaited signal", the usual `sigsuspend`
shape) runs with SIGSYS blocked. Its first trapped syscall then gets the
process force-killed (exit 159). Reproduced on the host build for
`ppoll`, `pselect6`, `epoll_pwait` and `rt_sigsuspend` with #27's
reproducer. The `sa_mask` variant is already fixed (`dd50607`).

## Constraints any fix must respect

- **Async handlers inspect the interrupted PC.** glibc ≥2.41
  `SIGCANCEL` acts only if the PC is inside
  `__syscall_cancel_arch_start…_end` (checked in glibc 2.44 on the dev
  box). musl does the same with `__cp_begin/__cp_end`. Profilers and Go
  preemption look at the PC too. If a blocking call runs from a
  tawcroot address, cancelling a thread blocked there won't act until
  the call returns.
- **No nested SIGSYS while SIGSYS is blocked** (`notes/tawcroot/sigsys-handler.md`).
- **Hot paths stay untrapped.** NULL-mask `ppoll`/`epoll_pwait` (glibc's
  `poll`/`epoll_wait` on aarch64), and the plain-file `open` path pays
  nothing extra.
- **The window between a handler returning and the syscall
  re-executing.** A signal handler can run in that gap. Anything a
  rewritten argument points at must survive that handler making the
  same kind of call.

## Possible fixes

### 1. Rewind with immutable arguments

The handler rewrites argument registers, moves the PC back one
instruction (`syscall_rollback` already left the registers as they were
before the call), and returns. The guest re-executes its own syscall
instruction, and the filter allows it because the rewritten argument is
recognisable. Rewritten memory must never change: for masks, use an
append-only, deduplicated table of SIGSYS-stripped masks, and have the
filter allow mask pointers inside the table.

- **Pros:**
  - Exact: the call runs at the guest's own PC and on its own stack,
    with the real `uc_sigmask`, and cancellation and kernel restart
    work natively.
  - One trap and no asm.
- **Cons:**
  - Only works when no post-processing is needed and the rewritten
    arguments are in registers or immutable memory.
  - The mask-pointer register afterwards points at our copy (identical
    contents). Every libc wrapper treats it as clobbered.
- **Fits:** the masked waits (B); x86 `epoll_wait` → `epoll_pwait`
  (registers only).

### 2. Narrow the filter so the call never traps

- `fcntl`: trap only for `fd ≥ reserved floor` or the seal commands
  `shm.c` emulates.
- `accept`: trap only when the address argument is non-NULL (the
  reverse translation needs it).
- **Pros:** exact, and the simplest option.
- **Cons:** only possible where the handler has nothing to do for that
  argument shape.

### 3. Handoff: run the call from a trampoline in guest context

1. The handler saves the guest registers into a block below the guest
   `sp`, and puts the rewritten arguments in that block as well.
2. It returns into a trampoline in the stub section, which issues the
   call from a second allowlisted IP.
3. The trampoline then makes a marker syscall that traps back into the
   handler.
4. The handler runs the post phase (for example the reverse translation
   for `accept`) and restores everything.

- **Pros:**
  - Handles post phases and arbitrary rewritten memory.
  - Signals are delivered in guest context, on the guest's stack or
    altstack, with the real mask. Kernel restart, nesting, `fork` and
    `siglongjmp` all work.
- **Cons:**
  - The blocking `svc` runs at a tawcroot PC, which breaks cancellation
    and PC-inspecting handlers. CFI for the trampoline would help only
    the older async-cancel glibc.
  - Async handlers see tawcroot registers.
  - Uses about 300 B plus scratch of guest stack.
  - Two traps per blocking call.
  - Moving parts: arch trampolines, a second allowlisted IP, and the
    marker trap.
- **Fits:** everything left in A (FIFO `open`, `accept` with an
  address, `connect`/`sendmsg`/`sendto` needing a translated unix
  address, tty-drain `ioctl`, x86 `poll`/`select`). Use it only when
  the call can block: the fd lacks `O_NONBLOCK`, no `MSG_DONTWAIT`, a
  nonzero timeout. FIFO `open` needs a cheap FIFO check that doesn't
  slow down plain-file opens (measure it). It could reopen through
  `/proc/self/fd/N` so no full path is needed in scratch.

### 4. Unmask signals during the call, inside the handler

Set the guest's mask around the blocking raw call.

- **Pros:** small change.
- **Cons:**
  - Guest handlers run nested on our frame, on whatever stack we're on.
    That is often the 16 KB fallback altstack, and a nested `SA_ONSTACK`
    delivery reuses its top and corrupts our frame.
  - It re-enters the handler mid-call: every lock and static buffer
    needs auditing.
  - Guests see a tawcroot PC and `uc_sigmask`.
- **Rejected.**

### 5. seccomp user-notify with a supervisor process

The guest's syscall blocks natively in the kernel, so it can be
interrupted.

- **Cons:** rewrites tawcroot's architecture, the supervisor has to
  cancel its own blocking work when the notification goes stale, and
  kernel and policy support is uneven across target Android devices.
- **Out of scope.**

## Suggested direction

Prefer exact over fewer mechanisms:
- 1 for B and x86 `epoll_wait`
- 2 for `fcntl` locks and NULL-address `accept`
- 3 only for what's left, with its divergences documented in
  `notes/tawcroot/status.md`

That is two mechanisms (1 and 3). Option 3 alone could cover both
problems, at the cost of regressing cancellation in masked waits, which
works today because they aren't trapped.

Tests for any fix, compared against native behaviour:
- each call above, with and without `SA_RESTART`
- a `SIGTERM` default action ending the process
- #27's reproducer for each wait
- nested blocking calls inside a handler
- `siglongjmp` out of a handler, and `fork` in one
- SIGSTOP/SIGCONT in the middle of a call
- remaining time written back by `select`/`ppoll`
- `pthread_cancel` of a thread blocked in each call
- fast paths and NULL masks not trapping
- the synthesized Android filter (`tests/handler/androidfilter`)

Add a no-unbounded-blocking-in-the-handler rule to
`notes/tawcroot/sigsys-handler.md`.
