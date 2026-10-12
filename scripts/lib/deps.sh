# Shared dep-fetch + verify helper. Sourced by every build script that
# vendors a third-party repo. Single source of truth for pins lives in
# `deps/deps.list`. See AGENTS.md "Vendored deps" for the policy.
#
# Public API (after sourcing):
#   dep_dir <name>     -- echo absolute checkout path for <name>
#   dep_ensure <name>  -- clone if missing, verify HEAD == pinned commit.
#                         Errors on commit mismatch (uncommitted edits OK).
#   dep_apply_patches <name> <patch-dir> [sed-expr]
#                      -- ensure <name>, reset/clean it if the patch hash
#                         changed, then apply *.patch from <patch-dir>.
#                         Optional sed expression transforms patch contents
#                         before hashing/applying.
#   dep_reset <name>   -- fetch + `git reset --hard <commit>`; on an
#                         actual pin move also `git clean -fdx` (stale
#                         in-tree build dirs + untracked files). Wipes
#                         any per-dep `.tawc-patches-applied-*` sentinel
#                         so the build's apply_patches stage re-runs.
#                         Used exclusively by `scripts/update-deps.sh`.
#   deps_verify_all    -- error if any *existing* checkout's HEAD differs
#                         from its pin. Missing checkouts are skipped
#                         (cloned on demand by whichever build needs them).
#   deps_all_names     -- echo every dep name, one per line, in manifest order.
#   dep_fetch_tarball <url> <sha256> <dest>
#                      -- download (if absent) and sha256-verify a tarball
#                         dep. For the few deps that ship as release
#                         tarballs instead of git repos.
#   deps_tree_state <name|dest-prefix/>...
#                      -- emit a working-tree fingerprint line per dep
#                         (HEAD + tracked-edit hash). Read-only; used by
#                         Gradle as an input property so dep-built
#                         artifacts track checkout content.
#   deps_names_with_dest_prefix <dest-prefix/>
#                      -- echo the names whose dest starts with the prefix.
#
# Offline mode: if `deps/.offline` exists, dep_ensure and dep_fetch_tarball
# refuse to download anything and fail instead. The F-Droid recipe
# creates it after `scripts/fetch-release-deps.sh --offline` has fetched
# every dep, so a dep that script forgot fails the build loudly rather
# than being cloned after F-Droid's source scan has run.
#
# dep_ensure and dep_apply_patches run deps_verify_all first (once per
# process), so any build that touches one dep fails on drift in any other.
# dep_reset is exempt — update-deps.sh exists to fix exactly that drift.
#
# Concurrency: callers serialise clone/checkout via flock on
# `$DEPS_REPO_DIR/build/.deps.lock` so two parallel `dep_ensure` calls
# (e.g. Gradle's per-ABI tasks running in parallel) can't race a clone.
#
# Sourcing this file requires DEPS_REPO_DIR to point at the repo root, OR
# leaves it derived from the location of this script.

# shellcheck shell=bash

if [ -z "${DEPS_REPO_DIR:-}" ]; then
    # scripts/lib/deps.sh -> ../..  is the repo root.
    DEPS_REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fi
DEPS_MANIFEST="$DEPS_REPO_DIR/deps/deps.list"
DEPS_LOCKFILE="$DEPS_REPO_DIR/build/.deps.lock"
DEPS_OFFLINE_MARKER="$DEPS_REPO_DIR/deps/.offline"

# Non-zero (with a message) if offline mode forbids fetching $1.
_deps_check_online() {
    [ -e "$DEPS_OFFLINE_MARKER" ] || return 0
    echo "ERROR: $1 is not fetched and ${DEPS_OFFLINE_MARKER#$DEPS_REPO_DIR/} forbids fetching." >&2
    echo "  Add it to scripts/fetch-release-deps.sh, which runs before the marker is set." >&2
    return 1
}

# Walk deps.list emitting (name, repo, commit, ref, dest) tab-separated.
# Skips comments and blank lines. We only have ~30 lines; a pure-bash
# loop is fine.
_deps_emit() {
    local name repo commit ref dest
    while IFS=$'\t' read -r name repo commit ref dest; do
        case "$name" in ''|'#'*) continue ;; esac
        # Defensive: trim a stray CR from windows-edited files.
        name="${name%$'\r'}"; repo="${repo%$'\r'}"
        commit="${commit%$'\r'}"; ref="${ref%$'\r'}"; dest="${dest%$'\r'}"
        printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$repo" "$commit" "$ref" "$dest"
    done <"$DEPS_MANIFEST"
}

deps_all_names() {
    _deps_emit | cut -f1
}

deps_names_with_dest_prefix() {
    _deps_emit | awk -F'\t' -v p="$1" 'index($5, p) == 1 { print $1 }'
}

# Look up <name> and populate DEP_NAME/DEP_REPO/DEP_COMMIT/DEP_REF/DEP_DEST.
# DEP_DEST is absolute. Returns non-zero if the name isn't in the manifest.
_dep_lookup() {
    local want="$1" name repo commit ref dest
    while IFS=$'\t' read -r name repo commit ref dest; do
        case "$name" in ''|'#'*) continue ;; esac
        name="${name%$'\r'}"; repo="${repo%$'\r'}"
        commit="${commit%$'\r'}"; ref="${ref%$'\r'}"; dest="${dest%$'\r'}"
        if [ "$name" = "$want" ]; then
            DEP_NAME="$name"
            DEP_REPO="$repo"
            DEP_COMMIT="$commit"
            DEP_REF="$ref"
            DEP_DEST="$DEPS_REPO_DIR/$dest"
            return 0
        fi
    done <"$DEPS_MANIFEST"
    echo "ERROR: dep '$want' not in $DEPS_MANIFEST" >&2
    return 1
}

dep_dir() {
    _dep_lookup "$1" || return 1
    printf '%s\n' "$DEP_DEST"
}

# True iff $DEP_COMMIT exists locally as a reachable commit.
_dep_have_commit() {
    git -C "$DEP_DEST" rev-parse --verify --quiet "$DEP_COMMIT^{commit}" >/dev/null
}

# Fetch from origin until $DEP_COMMIT is present, or give up. Tries
# (cheapest first): the ref-hint, the commit by name, unshallow,
# fetch-everything.
_dep_fetch_commit() {
    _dep_have_commit && return 0
    if [ "$DEP_REF" != "-" ] && [ -n "$DEP_REF" ]; then
        git -C "$DEP_DEST" fetch --quiet origin "$DEP_REF" 2>/dev/null || true
        _dep_have_commit && return 0
    fi
    # Some servers (gitlab, github) accept fetch-by-sha when uploadpack.
    # allowReachableSHA1InWant or allowAnySHA1InWant is set.
    git -C "$DEP_DEST" fetch --quiet origin "$DEP_COMMIT" 2>/dev/null || true
    _dep_have_commit && return 0
    if [ -f "$DEP_DEST/.git/shallow" ]; then
        git -C "$DEP_DEST" fetch --quiet --unshallow origin 2>/dev/null || true
        _dep_have_commit && return 0
    fi
    git -C "$DEP_DEST" fetch --quiet origin 2>/dev/null || true
    _dep_have_commit
}

# Fresh clone of $DEP_REPO into $DEP_DEST. Uses --branch when we have a
# ref hint (cheaper for tags / single branches); otherwise full clone.
# A pre-existing non-git $DEP_DEST (e.g. left behind by Gradle's
# outputs.dir declaration) is wiped — git refuses non-empty targets.
_dep_clone() {
    [ -e "$DEP_DEST" ] && [ ! -d "$DEP_DEST/.git" ] && rm -rf "$DEP_DEST"
    mkdir -p "$(dirname "$DEP_DEST")"
    if [ "$DEP_REF" != "-" ] && [ -n "$DEP_REF" ]; then
        echo "==> cloning $DEP_NAME @ $DEP_REF -> ${DEP_DEST#$DEPS_REPO_DIR/}"
        # `--branch` accepts both branches and tags. We don't pass
        # --depth: the commit pin may not be at the ref tip, and a
        # shallow clone forces an --unshallow round-trip later. The
        # repos in deps.list are small enough (~tens of MB at most)
        # that a full clone is faster end-to-end than depth-juggling.
        git clone --quiet --branch "$DEP_REF" "$DEP_REPO" "$DEP_DEST"
    else
        echo "==> cloning $DEP_NAME -> ${DEP_DEST#$DEPS_REPO_DIR/}"
        git clone --quiet "$DEP_REPO" "$DEP_DEST"
    fi
}

# After cloning or resetting, check the resolved tip matches the pin. If
# the user has uncommitted edits on top, we don't care — only the parent
# commit matters.
_dep_verify_head() {
    local actual
    actual="$(git -C "$DEP_DEST" rev-parse HEAD)"
    if [ "$actual" != "$DEP_COMMIT" ]; then
        cat >&2 <<EOF
ERROR: dep '$DEP_NAME' is at the wrong commit.
       expected: $DEP_COMMIT  (from deps/deps.list)
       got:      $actual      (HEAD of ${DEP_DEST#$DEPS_REPO_DIR/})
        Run \`scripts/update-deps.sh $DEP_NAME\` to align, OR — if you
       deliberately moved the dep — bump the commit in deps/deps.list
       to $actual and re-run.
EOF
        return 1
    fi
}

# Run a function while holding an exclusive flock over $DEPS_LOCKFILE so
# parallel build-script invocations (e.g. Gradle's per-ABI tasks) can't
# race a clone or checkout. The lock is reentrant within the same shell
# (FD 9 already open) so `dep_reset` calling `dep_reset` doesn't deadlock.
_dep_with_lock() {
    if [ "${_DEP_LOCK_HELD:-}" = "1" ]; then
        "$@"
        return
    fi
    mkdir -p "$(dirname "$DEPS_LOCKFILE")"
    : >>"$DEPS_LOCKFILE"
    (
        flock 9
        _DEP_LOCK_HELD=1 "$@"
    ) 9>"$DEPS_LOCKFILE"
}

_deps_verify_all_inner() {
    local name repo commit ref dest actual bad=0
    while IFS=$'\t' read -r name repo commit ref dest; do
        [ -d "$DEPS_REPO_DIR/$dest/.git" ] || continue
        actual="$(git -C "$DEPS_REPO_DIR/$dest" rev-parse HEAD 2>/dev/null || echo '<unreadable>')"
        if [ "$actual" != "$commit" ]; then
            echo "ERROR: dep '$name' is at the wrong commit." >&2
            echo "       expected: $commit  (from deps/deps.list)" >&2
            echo "       got:      $actual  (HEAD of $dest)" >&2
            bad=$((bad + 1))
        fi
    done < <(_deps_emit)
    if [ "$bad" -ne 0 ]; then
        echo "Run \`scripts/update-deps.sh\` to align checkouts with deps/deps.list, OR —" >&2
        echo "if you deliberately moved a dep — bump its commit in deps/deps.list." >&2
        return 1
    fi
}

deps_verify_all() {
    _dep_with_lock _deps_verify_all_inner
}

# One fingerprint line: "<name> <head> <diffhash>". <diffhash> is a
# short sha1 of `git diff-index -p HEAD` (staged + unstaged edits to
# tracked files) or "-" when clean; diff-index is plumbing, so user
# diff config (external drivers, noprefix) can't skew the hash.
# Untracked files are deliberately excluded: they survive `dep_reset`,
# so they can't make an artifact stale the way discarded tracked edits
# can, and including them would churn on in-tree build dirs.
# A missing checkout reports "<pin> -" — identical to the fresh clone
# dep_ensure produces, so absence alone never invalidates a build,
# while deleting a *drifted* checkout stops matching the recorded
# state and the artifact rebuilds from the pin.
_dep_tree_state_one() {
    _dep_lookup "$1" || return 1
    if [ ! -d "$DEP_DEST/.git" ]; then
        printf '%s %s -\n' "$DEP_NAME" "$DEP_COMMIT"
        return 0
    fi
    local head hash
    head="$(git -C "$DEP_DEST" rev-parse HEAD)" || return 1
    # Refresh the stat cache so content-identical files aren't rehashed
    # every call; harmless if a parallel git op holds the index lock.
    git -C "$DEP_DEST" update-index -q --refresh >/dev/null 2>&1 || true
    hash="$(git -C "$DEP_DEST" diff-index -p HEAD -- | sha1sum | cut -c1-12)"
    [ "$hash" = "da39a3ee5e6b" ] && hash='-'  # sha1("") prefix == clean
    printf '%s %s %s\n' "$DEP_NAME" "$head" "$hash"
}

# deps_tree_state <name|dest-prefix/>... -- fingerprint one line per
# dep. An arg ending in "/" selects every dep whose manifest dest
# starts with that prefix (e.g. "deps/xwayland-src/"), so callers don't
# hardcode a list that drifts when a dep is added. Never clones,
# verifies, or takes the lock.
deps_tree_state() {
    local arg name matched
    for arg in "$@"; do
        case "$arg" in
            */)
                matched=0
                while IFS= read -r name; do
                    matched=1
                    _dep_tree_state_one "$name" || return 1
                done < <(deps_names_with_dest_prefix "$arg")
                if [ "$matched" -eq 0 ]; then
                    echo "ERROR: no dep dest starts with '$arg'" >&2
                    return 1
                fi
                ;;
            *)
                _dep_tree_state_one "$arg" || return 1
                ;;
        esac
    done
}

# Once-per-process front door for the dep_ensure/dep_apply_patches hook.
# The flag must be set outside _dep_with_lock — its subshell can't export
# back to us.
_deps_verify_all_once() {
    if [ "${_DEPS_VERIFIED_ALL:-}" != "1" ]; then
        deps_verify_all || return 1
        _DEPS_VERIFIED_ALL=1
    fi
}

_dep_ensure_inner() {
    if [ ! -d "$DEP_DEST/.git" ]; then
        _deps_check_online "dep '$DEP_NAME'" || return 1
        _dep_clone || return 1
        if ! _dep_have_commit; then
            _dep_fetch_commit || {
                echo "ERROR: dep '$DEP_NAME': commit $DEP_COMMIT unreachable from origin." >&2
                return 1
            }
        fi
        # Detached checkout — we don't track upstream branches.
        git -C "$DEP_DEST" -c advice.detachedHead=false checkout --quiet "$DEP_COMMIT"
    fi
    _dep_verify_head
}

dep_ensure() {
    _dep_lookup "$1" || return 1
    _deps_verify_all_once || return 1
    _dep_with_lock _dep_ensure_inner
}

_dep_apply_patches_inner() {
    local patch_dir="$1" sed_expr="${2:-}"
    _dep_ensure_inner || return 1
    shopt -s nullglob
    local patch_files=("$patch_dir"/*.patch)
    shopt -u nullglob
    [ "${#patch_files[@]}" -gt 0 ] || return 0

    local hash sentinel
    if [ -n "$sed_expr" ]; then
        hash="$(sed "$sed_expr" "${patch_files[@]}" | sha1sum | cut -c1-12)"
    else
        hash="$(cat "${patch_files[@]}" | sha1sum | cut -c1-12)"
    fi
    sentinel="$DEP_DEST/.tawc-patches-applied-$hash"
    [ -f "$sentinel" ] && return 0

    rm -f "$DEP_DEST"/.tawc-patches-applied-*
    git -C "$DEP_DEST" reset --hard --quiet HEAD
    git -C "$DEP_DEST" clean -fdx --quiet
    local p
    for p in "${patch_files[@]}"; do
        echo "==> patch $DEP_NAME: $(basename "$p")"
        if [ -n "$sed_expr" ]; then
            sed "$sed_expr" "$p" | ( cd "$DEP_DEST" && patch -p1 --no-backup-if-mismatch )
        else
            ( cd "$DEP_DEST" && patch -p1 --no-backup-if-mismatch < "$p" >/dev/null )
        fi
    done
    touch "$sentinel"
}

dep_apply_patches() {
    _dep_lookup "$1" || return 1
    _deps_verify_all_once || return 1
    _dep_with_lock _dep_apply_patches_inner "$2" "${3:-}"
}

_dep_reset_inner() {
    if [ ! -d "$DEP_DEST/.git" ]; then
        _dep_clone || return 1
    fi
    if ! _dep_have_commit; then
        _dep_fetch_commit || {
            echo "ERROR: dep '$DEP_NAME': commit $DEP_COMMIT unreachable from origin." >&2
            return 1
        }
    fi
    local before
    before="$(git -C "$DEP_DEST" rev-parse HEAD 2>/dev/null || echo none)"
    if [ "$before" != "$DEP_COMMIT" ]; then
        echo "==> reset $DEP_NAME ${before:0:12} -> ${DEP_COMMIT:0:12}"
    fi
    git -C "$DEP_DEST" -c advice.detachedHead=false reset --quiet --hard "$DEP_COMMIT"
    if [ "$before" != "$DEP_COMMIT" ]; then
        # A pin move invalidates in-tree build dirs — configure-time
        # decisions (feature probes, versions) bake in the old commit
        # and survive `reset --hard`. Untracked WIP goes with them;
        # update-deps.sh's header warns about that.
        git -C "$DEP_DEST" clean -fdx --quiet
    fi
    # Patch sentinels (`.tawc-patches-applied-<sha>`) live in the working
    # tree as untracked files, so `reset --hard` doesn't remove them.
    # Drop any so the build's apply_patches stage re-runs.
    rm -f "$DEP_DEST"/.tawc-patches-applied*
    _dep_verify_head
}

dep_reset() {
    _dep_lookup "$1" || return 1
    _dep_with_lock _dep_reset_inner
}

# ---------------------------------------------------------------------------
# Tarball deps. A handful of deps ship as release tarballs rather than git
# repos (libmd, talloc); their version lives in the fetching script's URL,
# not deps.list. Download them through this so the bytes are pinned by
# hash the way git deps are pinned by commit.
#
#   dep_fetch_tarball <url> <sha256> <dest-file>
#
# Re-verifies an already-present file, so a truncated or tampered cached
# download fails the same way a fresh bad one does.
dep_fetch_tarball() {
    local url="$1" want="$2" dest="$3" actual
    if [ ! -f "$dest" ]; then
        _deps_check_online "tarball $url" || return 1
        echo "==> downloading $url"
        mkdir -p "$(dirname "$dest")"
        curl -fsSL -o "$dest.part" "$url"
        mv "$dest.part" "$dest"
    fi
    actual="$(sha256sum "$dest" | awk '{print $1}')"
    if [ "$actual" != "$want" ]; then
        echo "error: sha256 mismatch for $dest" >&2
        echo "  expected $want" >&2
        echo "  actual   $actual" >&2
        echo "  (delete the file to re-download, or fix the pin if the" >&2
        echo "   upstream version was bumped)" >&2
        return 1
    fi
}
