# Remote access (sshyeet client in Rust)

A "Remote access" entry in the home screen ⋮ menu that makes the open
distro reachable over ssh from any other machine, on any network, through
the sshyeet.com relay. TAWC ships its own Rust implementation of the
sshyeet *client*; the relay stays a third-party service that only splices
bytes. Nothing ssh-related runs until the user taps Start, and it all
stops on Stop, TTL, or the notification's Exit.

Background: [notes/session-service.md](../notes/session-service.md),
[notes/terminal.md](../notes/terminal.md) (spawn path),
[notes/architecture.md](../notes/architecture.md) (JNI shape),
[notes/exec-broker.md](../notes/exec-broker.md) (test actions).

## Why a Rust port

The upstream client is ~2,400 lines of Go (source: `https://sshyeet.com/dl/src.tar.gz`,
version `46kr0c202g`, sha256 `91e3d978…7c4a8`, reviewed 2026-09-27). It
was read end to end; the model holds: host key and secret are generated
locally, the relay sees only SSH ciphertext, nothing from the relay can
reach a shell. Porting rather than vendoring because: no Go toolchain in
the APK build or the F-Droid recipe; the code is ours to review and
change like the rest of the Rust; it can spawn shells through the same
tawcroot envelope the in-app terminal uses; the Go tarball ships no
license. Cost: we own a fork of a small protocol and must track relay
changes (the relay is versioned, see "Protocol").

## User-facing design

- **Entry point:** ⋮ → "Remote access" on the home screen, enabled for a
  READY tawcroot install (same gate as the Terminal FAB). Opens
  `RemoteAccessActivity` for the open distro.
- **Idle screen:** explanation (one paragraph: what the relay sees, that
  anyone with the secret gets a root shell in this distro), a TTL choice
  (15 min / 1 h / 8 h / until stopped; default 1 h), Start.
- **Running screen:** the `ssh -J sshyeet.com <secret>@<id>` command
  (large, tap to copy), the host key fingerprint, the pinned command
  (`-o KnownHostsCommand=…`, collapsed), time remaining, a live list of
  connection events (from, authenticated/exec/disconnected), Stop. Also
  "New secret" (rolls key + secret + id; kicks current clients).
- **Not shown:** the relay's browser-terminal link. It puts the secret in
  a URL fragment for sshyeet.com's JavaScript, which we don't want users
  to trust by default. Reconsider later as an explicit "open in browser"
  with a warning.
- **Notification:** a `Remote(distroId)` hold in `SessionHolds` while the
  agent runs, so the text reads e.g. "remote access · 1 client" and the
  process stays a foreground service (Doze would otherwise cut the
  tunnel). Exit stops it. Screen off with the SoC suspended still stalls
  it; that is [plans/wakelock.md](wakelock.md)'s problem, link the two.
- **One session at a time**, process-wide, bound to one install. Opening
  the screen for another distro while one runs shows which distro it is
  for and offers Stop.
- **Later (phase 3):** key auth instead of a secret (paste authorized
  keys or a GitHub username, fetched from github.com directly), sftp/scp,
  `-L` forwarding toggle. Settings card gets a relay URL override
  (debug builds only; the default is `https://sshyeet.com`).

## Security rules

- No listener, ever. Outbound only: `wss://sshyeet.com:443`, plus
  `https://github.com/<user>.keys` in key mode.
- No thread, socket, or tokio runtime exists until `nativeRemoteStart`.
  `nativeRemoteStop` joins the thread. Compositor activation, install
  steps, and the terminal never touch the module.
- Secret and host key are generated per Start from the OS CSPRNG. The
  secret is shown on the device and never logged (no logcat of usernames
  or auth attempts, per the sparse-logging rule; the debug broker
  `remote-status` action is the only other place it appears, debug builds
  only).
- Secret: two words from the EFF long wordlist (7,776 words) plus three
  digits, ~35.8 bits, e.g. `acid-vowel-417`, generated from the OS CSPRNG. Stronger
  than upstream's ~22.6-bit `adjective-noun-NN` but still one line to read
  off a phone and type; the relay never sees it, so its format is ours.
  Online defence as upstream: compared in constant time as the SSH
  username with the `none` method; whole-agent token bucket of 10 burst /
  10 per minute with a 1 s stall per counted wrong guess; secret auth
  switches off for the run after 300 wrong guesses (one chance in ~200
  million per session). Usernames not shaped like a secret (two
  dash-separated lowercase words, then three digits) cost nothing.
- Everything from the relay's `ready` message is validated before it is
  shown or used: the id must derive from *our* host key, the jump host
  must match `^([A-Za-z0-9.-]{1,253}|\[[0-9A-Fa-f:.]{2,45}\])(:[0-9]{1,5})?$`,
  free text is stripped of control characters and truncated.
- The hello `comment` is omitted (upstream sends `user@host`); `agent` is
  `tawc/<versionName> <os>/<arch>`. Ignore `latest` (refers to upstream
  binaries).
- Default TTL 1 h. Stop closes the tunnel and closes every pty master, so
  spawned shells get SIGHUP like a closed terminal; stragglers fall into
  the session service's stray tail.
- Logins are root inside the distro, as the app uid outside, same as the
  in-app terminal. Say so on the idle screen.

## Code layout

- **`remote/`** — new crate `tawc_remote`, pure Rust, host-buildable
  (no `ndk`/`jni` deps), with its own `Cargo.lock` like
  `tests/integration`. Modules: `proto` (JSON messages, framing),
  `sid` (ids + frozen word lists), `tunnel` (WebSocket + yamux, reconnect
  loop, handshake), `sshd` (russh server: auth policy, sessions, exec,
  pty, direct-tcpip, later sftp), `spawn` (the process-launch trait, see
  below), `agent` (ties it together, status/event API). Runs on its own
  tokio runtime on one thread named `tawc-remote`.
- **`compositor/src/remote_jni.rs`** — JNI glue only:
  `nativeRemoteStart(json) -> Boolean`, `nativeRemoteStop()`,
  `nativeRemoteStatus() -> String?` (JSON), `nativeRemoteRollSecret()`,
  and reverse-JNI `onRemoteEvent(json)` for the live event list (same
  pattern as `onActivationRequested`). `compositor/Cargo.toml` gets
  `tawc_remote = { path = "../remote" }`; Gradle's
  `buildRustLibrary<Abi>` adds `remote/src`, `remote/Cargo.toml` to its
  inputs. One cdylib keeps packaging unchanged; the module is inert
  until called.
- **Kotlin** — `remote/RemoteAccessActivity.kt`, `RemoteSession`
  (process-wide state + `SessionHolds` hold + TTL timer),
  `SessionHolds.Reason.Remote`, ⋮ entry in `MainActivity`, strings.
  `TawcrootMethod.ptyShellExec` is refactored so the envelope (argv up to
  and including the `env` args, resolved shell, `TMPDIR`, cwd) can be
  handed to native as JSON without a command baked in.
- **Spawn trait:** `spawn::Launcher { fn shell(pty) ; fn exec(cmd, pty) ; fn subsystem(name) }`.
  On device, the implementation appends `<shell> -l` (interactive) or
  `/bin/bash -lc <cmd>` (exec, matching every other command spawn, see
  terminal.md) to the tawcroot argv, `openpty`s via rustix, forks,
  `setsid`s in the child (rootfs-session invariant), execs. On the host
  tests use `/bin/sh` directly.
- **sftp (phase 3):** the agent sees the host view of the rootfs, so a
  Rust sftp server would need path rewriting and would miss binds. Instead
  run the distro's own `sftp-server` (`/usr/lib/ssh/sftp-server` on Arch,
  `/usr/lib/openssh/sftp-server` on Debian) through tawcroot in pipe
  mode; if absent, reject the subsystem with "install openssh in the
  distro". scp on OpenSSH ≥ 9 uses sftp, so that covers it.
- **direct-tcpip** dials from the app process. tawcroot shares the
  network namespace, so `ssh -L 8080:localhost:8080` reaches a server in
  the rootfs. Behind a toggle, default on like upstream. No `-R`.

Crates: `tokio`, `tokio-tungstenite` (rustls + `webpki-roots`: a bionic
process has no `/etc/ssl`), `yamux` (libp2p's, spec-compatible with
Hashicorp's) + `tokio-util` compat, `russh`, `russh-keys`, `serde`,
`serde_json`, `sha2`, `data-encoding`, `subtle`, `rustix` (pty). All
MIT/Apache. Prefer the `ring` rustls backend; `aws-lc-rs` needs cmake
under cargo-ndk. Update `notes/building.md` (new crate, cargo-ndk inputs)
and the fdroid recipe needs nothing new: everything is pinned in
`compositor/Cargo.lock`.

## Protocol (from the upstream client, v1)

Transport: `GET wss://<relay>/v1/tunnel`, header `User-Agent: tawc/…`,
optional `fly-force-instance-id: <node>` to land on the node we were on
(from a previous `ready.node`), compression off, no read limit. Every
WebSocket **binary** message is a chunk of one byte stream (frames may
split yamux frames). Over it, **yamux**, agent = yamux *client*
(hashicorp/yamux spec: 12-byte big-endian header `version=0, type u8,
flags u16, stream_id u32, length u32`; types Data 0 / WindowUpdate 1 /
Ping 2 / GoAway 3; flags SYN 1 / ACK 2 / FIN 4 / RST 8; client streams
odd, server streams even; initial window 256 KiB). Upstream keepalive:
ping every 25 s, write timeout 20 s. Answer relay pings; libp2p `yamux`
does.

**Control stream** = the first stream the agent opens. Newline-delimited
JSON, one object per line, lines ≤ 16 KiB, strictly alternating:

1. relay → `{"op":"challenge","v":1,"nonce":<base64 bytes, ≥16>}`.
   `v > 1` → fatal "relay protocol changed; update TAWC".
2. agent → `{"op":"hello","v":1,"host_key":"ssh-ed25519 AAAA…","sig":<base64>,"ttl":<s>,"agent":"tawc/…"}`.
   `host_key` is authorized_keys format, trimmed. `sig` is the SSH wire
   encoding of the signature (`string format || string blob`, i.e.
   `ssh.Marshal(ssh.Signature)`) over `"sshyeet hello v1\x00" || nonce`.
   `ttl` seconds requested, 0 = relay max (24 h).
3. relay → `{"op":"ready","id":"sunny-tidy-crab","jump":"sshyeet.com","web":"https://sshyeet.com","node":"…","region":"…","expires":<unix>,"latest":"…","notice":"…"}`
   or `{"op":"error","msg":"…"}`. `msg == "session id taken"` → with an
   ephemeral key, roll a new key *and* secret and reconnect at once (up to
   8 times; warn after the second); any other error is fatal.
4. Later relay → `{"op":"bye","reason":"…","reconnect":bool}`; reconnect
   = come back (backoff 1 s doubling to 1 min, reset after a minute up),
   otherwise fatal. Stream/socket closure without bye = reconnect. Give up
   after 4 failed attempts if never `ready`.

**Client streams** are opened by the relay, one per ssh client. First a
JSON line `{"from":"1.2.3.4:5678","via":"…"}` (read with a 15 s
deadline), then raw bytes that are the SSH transport from the client's
`direct-tcpip` channel. Feed them to the embedded SSH server. Half-close
propagates; linger 2 min then close both.

**Session id** = pure function of the host key:
`H = SHA-256("sshyeet id v2\0" || ssh-wire(pubkey))`;
short `ADJ[H[0]]-ADJ[H[1]]-NOUN[H[2]]`; long `short-base32(H[3..13])`
with alphabet `abcdefghijklmnopqrstuvwxyz234567`, no padding, 16 chars.
Word lists: 256 adjectives, 256 nouns, positional, frozen. Copied
verbatim from the upstream tarball (`internal/sid/words.go`) into
`remote/src/sid/words.rs`, with permission. The relay accepts
`<id>`, `<long-id>`, and `.yeet`/`.sshyeet` suffixes, and routes on the
three words.

**Secret** (ours, not the relay's concern): two words drawn uniformly
from the EFF long wordlist plus a three-digit number `000`–`999`, joined
with `-`. Upstream's is
`ADJ[r0]-NOUN[r1]-NN` from the 256-word lists; we do not copy that. The
word file lives in `remote/src/sshd/eff_large_wordlist.txt` (CC-BY 3.0,
credit in `licenses/`).

**Embedded SSH server:** ed25519 host key; `none` auth succeeds iff the
username is the secret (username travels encrypted, the relay never sees
it); `publickey` refuses everything in secret mode (so failures read
"Permission denied (publickey)") and matches the authorized set in key
mode; MaxAuthTries 6; 2 min handshake deadline; ≤ 64 concurrent
connections. Channels: `session` (pty-req, env, window-change, signal,
shell, exec, subsystem sftp; exit-status on completion; SIGHUP on channel
close), `direct-tcpip`. Global requests: reply false.

## Testing

1. **Spike first (phase 0).** `cargo run --example relay-spike` in
   `remote/` connects to the live relay from the dev box, completes the
   handshake, prints the `ssh -J` command, serves one shell via `/bin/sh`.
   Connect with the host's `ssh`. This is the only real unknown (yamux
   interop and the signature/id encodings); do it before writing the
   rest. If libp2p `yamux` misbehaves, a minimal yamux is ~400 lines.
2. **Host unit tests (`cargo test` in `remote/`)**: sid derivation
   against vectors (fixed key → expected words + long form; confirm the
   first vector once against what the live relay assigns), secret shape,
   `looks_like_secret`, token bucket atomicity and lifetime cap, `ready`
   validation (jump regex, id mismatch → fatal, control-char stripping),
   framing (16 KiB line cap, split frames).
3. **Host end-to-end tests with a fake relay** (`remote/tests/`): a
   tokio-tungstenite server + yamux *server* in-process that speaks the
   control protocol and exposes a TCP port whose connections become
   client streams with a `from` header. Tests run the host `ssh`/`scp`
   binaries against `ssh -p <port> <secret>@localhost` with
   `StrictHostKeyChecking=no`: correct secret → shell runs `echo`;
   wrong secret ×N → locked out, right secret refused until refill;
   pty exec with window-change; non-pty exec exit status; `-L` forward
   to a local echo server; `session id taken` → new key/secret; `bye
   reconnect` → reconnects and keeps the same id; stop → child gets
   SIGHUP. No network needed, so it runs in CI.
4. **Live interop test**, opt-in (`TAWC_LIVE_RELAY=1`), same test file:
   agent on host → sshyeet.com, `ssh -J sshyeet.com <secret>@<id> true`.
   Run manually before merging and whenever the relay changes.
5. **On-device integration tests** (`tests/integration/tests/remote.rs`),
   new broker actions `remote-start ttl=… `, `remote-status` (the status
   JSON incl. secret), `remote-stop`. With network on the target and the
   live relay: start, read the command, from the host run
   `ssh -J sshyeet.com <secret>@<id> 'cat /etc/os-release'` and assert
   the distro name; `session-state` shows `Remote`; stop → hold gone,
   spawned shell gone. Gated like the other target-specific pins
   (`cfg(tawc_skip_network_on_target)` or a runtime probe), never
   default in `run-integration-tests.sh` without network. Plus a
   no-network test: before any start, `session-state` has no `Remote`
   and `remote-status` reports `stopped`.
6. **Manual UI check** on `.tawctarget`: start from the ⋮ menu, connect
   from a laptop on a different network (phone on mobile data), verify
   the event list, TTL expiry, Stop, Exit from the notification,
   rotation keeps the screen state. Screenshot per the usual rules.
7. **App unit tests**: `RemoteSession` state machine and hold lifecycle
   (JVM, like `SessionHolds`).

## Phases

0. Spike against the live relay (see Testing 1). Half a day.
1. `remote/` crate: proto, sid, tunnel, sshd with secret auth, shell/exec/
   pty, direct-tcpip; fake-relay tests. The bulk of the work.
2. JNI + Kotlin: envelope refactor, `Remote` hold, activity, ⋮ entry,
   TTL, roll secret, event list; broker actions + device tests;
   `notes/remote-access.md`, building.md, testing.md updates.
3. Key auth (paste / GitHub), sftp via the distro's `sftp-server`,
   forwarding toggle, debug relay override.

## Open questions for sshyeet

- Is protocol v1 (as above) stable? Will `v` be bumped for breaking
  changes, and can the relay keep speaking v1 for a while after?
- Is a non-upstream agent string welcome, and any rate limits per relay
  node we should respect (the relay returns 429 on the WebSocket dial)?
