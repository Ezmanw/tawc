# tawcroot: exec mappings leak into the parent of a vfork/posix_spawn child

A guest `execve` from a `CLONE_VM` child (`vfork`, glibc `posix_spawn`)
runs `tawcroot_exec_handler_prepare`/`commit` in the parent's address
space. On success the child's `execveat` swaps in a new mm, but the
mappings made in the shared mm stay behind in the parent:

- the exec_state memfd `MAP_SHARED` mapping (`prep.map`, 64 KiB up to
  the state size rounded to a power of two), which also keeps the
  memfd's pages alive;
- commit()'s anon scratch (re-exec argv/envp pointer arrays + title
  pieces, about the guest cmdline size).

So every successful spawn leaks roughly 2× its argv+envp bytes in the
spawning process. Predates the kernel-exact limits change (commit()
already mmap'd the state and an envp array and never unmapped them on
success); larger argvs now make each leak larger. Long-running spawners
(Firefox, IDEs, make/ninja via posix_spawn) accumulate it.

Not easily fixed in commit(): execveat needs those strings mapped until
it succeeds, and nothing runs in the parent's mm afterwards. Options:
have the parent reclaim after `vfork` returns (it knows its child
exec'd — e.g. a per-mm list of pending exec mappings swept on the next
trapped syscall), or build the re-exec argv/envp on a region the parent
already owns and reuses.
