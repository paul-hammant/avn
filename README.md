# avn

A clean-sheet reimplementation of [Apache Subversion] in the [Aether
systems language]. The goal isn't a mechanical C→Aether translation —
we read the avn C tree as a *reference for behaviour contracts* and
wrote our own on-disk format, wire protocol, and algorithms.

The reference avn C code still lives at `libsvn_*/` in this repo,
read-only. The port lives at the repo root in per-area dirs (`client/`, `delta/`, `repo_storage/`, `working_copy/`, etc). The old Apache-avn README is
preserved at [`README.apache-svn-upstream`](./README.apache-svn-upstream).

[Apache Subversion]: https://subversion.apache.org/
[Aether systems language]: https://aetherlang.org/

## Status

- **57 commits**, each phase reviewable in isolation.
- **47 test suites**, ~507 assertions, all green.
- **~14,400 lines** (Aether + C shims + shell drivers).
- Daily-driver workflow end-to-end: `checkout → edit → status → diff →
  add → rm → commit → update → branch → merge`.

Full phase map and feature matrix: **[PORT_STATUS.md](./PORT_STATUS.md)**.

## Binaries we ship

| Binary | Role |
|---|---|
| `avn` | Client CLI: `checkout`, `status`, `add`, `rm`, `cp`, `mv`, `commit`, `update`, `revert`, `diff`, `log`, `cat`, `ls`, `info`, `propset`/`get`/`del`/`list`, `merge`, `branch create`/`list`, `switch`, `blame`, `verify` |
| `avnserver` | Read/write HTTP server. JSON for metadata, base64 for bytes over JSON, raw bytes for cat. |
| `avnadmin` | Repository admin: `create`, `dump`, `load` (our own portable dump format; not reference-avn compatible by design). |
| `avn-seed` | Test-only helper that populates a repo with a known tree across three commits. |

## Quickstart

Build and test driven by [aetherBuild]'s `aeb`. Run `aeb --init` once to
populate `.aeb/lib/`. After that, one command builds every binary
(regenerating `_generated.c` files as needed via aetherc) and runs the
full 32-test suite.

[aetherBuild]: https://github.com/paul-hammant/aetherBuild

```bash
# Build every binary AND run the 32-test suite
aeb

# Build a single binary
aeb avn         # just avn — output at target/avn/bin/avn

# Outputs land under target/<dir>/bin/<name>
target/avnadmin/bin/avnadmin create /srv/repo
target/svnserver/bin/avn-seed   /srv/repo        # three-commit known tree
target/svnserver/bin/avnserver demo /srv/repo 8080 &
target/avn/bin/avn checkout http://localhost:8080/demo wc
cd wc
echo "hi" >> README
../target/avn/bin/avn commit --author alice --log "tweak"
```

### Generated sources

Some `.c` files at the repo root in per-area dirs (`client/`, `delta/`, `repo_storage/`, `working_copy/`, etc) are produced by `aetherc --emit=lib` from
paired `.ae` sources (grep for `_generated.c`). They're **not checked
into git** — aeb's `aether.program(b) { regen(...) }` setter
regenerates them on demand. Never hand-edit a `_generated.c`; the next
regen blows your changes away. Edit the `.ae` source instead.

## What's intentionally different from reference avn

The port shares avn's commands, data model, and user-facing semantics,
but diverges freely on the plumbing where doing so pays off.

### Storage

- **Content-addressable rep store** at `$repo/reps/aa/bb/<sha>.rep`
  with `Z` (zlib) or `R` (raw) header byte. SHA-1 by default, SHA-256
  selectable at repo-create time; format file records the choice.
- **Per-repo pluggable hash:** `avnadmin create PATH --algos sha256`.
  Server advertises via `GET /info`; clients adapt at checkout time
  and persist the algo in `wc.db`. Golden list enforced at create
  time — you can't open a repo whose hash your build doesn't support.
- **Multi-algo secondary hashes** (Phase 7.5): add SHA-256 to an
  existing SHA-1 repo so you can migrate gradually. `avn verify
  --secondaries` cross-checks both.
- **Merkle verification with per-user redaction** (Phase 7.1): a
  restricted user's recomputed root hash matches what the server
  reports, even though the tree they see has denied subtrees
  replaced by opaque `{kind: "hidden", sha: ...}` entries.
- **rep-cache.db** is SQLite; also hosts `path_rev` (Phase 8.1) for
  O(touched-revs) queries, and `rep_cache_sec` for secondary hashes.
- **Branches** (Phase 8.1+): each branch has its own tree root
  (git-like), its own head file at `$repo/branches/<name>/head`, and
  its own revs fanout at `$repo/branches/<name>/revs/aa/bb/NNNNNN`.
  Rev numbers are per-repo, not per-branch. Design rationale:
  [`branching_connnundrum.md`](./branching_connnundrum.md).

### Wire protocol

- **REST + JSON + base64** over plain HTTP. No WebDAV, no DeltaV, no
  `ra_svn` / `ra_serf` / `ra_local` multiplexing. Endpoints fit on a
  screen:

  ```
  GET    /repos/{r}/info
  GET    /repos/{r}/rev/{N}/info
  GET    /repos/{r}/rev/{N}/cat/<path>
  GET    /repos/{r}/rev/{N}/list/<path>
  GET    /repos/{r}/rev/{N}/paths
  GET    /repos/{r}/rev/{N}/props/<path>
  GET    /repos/{r}/path/<p>                    # HEAD content
  PUT    /repos/{r}/path/<p>                    # single-file commit
  DELETE /repos/{r}/path/<p>                    # single-delete commit
  POST   /repos/{r}/commit                      # bulk commit
  POST   /repos/{r}/copy                        # server-side cp
  POST   /repos/{r}/branches/<NAME>/create      # create branch
  ```

- **URL grammar:** `http://host/repo/BRANCH[;PATH]` — `;` separates
  branch from path. Legacy form `http://host/repo[/PATH]` still works
  and implies `main`.
- **Optimistic concurrency** via `Svn-Based-On: <sha>` header, not
  revision-based locking.
- **ACL-aware responses:** denied subtrees are indistinguishable from
  "doesn't exist" at the HTTP layer.

### Dump format

- Our own portable format (Phase 12). Round-trips through `avnadmin
  dump` / `avnadmin load`. Not byte-compatible with reference avn's
  dumpfile format — if you need to migrate *into* this port, you're
  on your own for now.

### ACLs and branches

- **ACL lines** (`+alice`, `-eve`) stored at per-path granularity,
  anchored to each rev. No `svn:mergeinfo`-style property hackery.
- **avn cp refuses unless RW-everywhere** (Phase 7.7): you can't
  server-copy a subtree you don't have full RW access to, closing
  the "branch off a locked area to escape its ACLs" gap.
- **Branches are first-class** (Phase 8.1+). Each branch has an
  include-glob spec; paths outside the spec are rejected at commit
  time. Cross-branch `avn cp` is refused. Super-user bypasses.

### Working copy

- **Single `wc.db`** (SQLite) per WC. Pre-1.7-style entries files
  are not supported — clean-sheet means clean-sheet.
- **Pristine store** is content-addressable under `.svn/pristine/`,
  matching the repo's hash algo.

### What we inherited unchanged

- Command names and semantics. `avn status` means what you'd expect,
  including `?` / `M` / `A` / `D` / `R` / conflict markers.
- Three-way merge with svn-style conflict markers in working files.
- `svn:ignore` read by `status`.
- `avn log -v` per-rev path list.
- `svn:mergeinfo` range arithmetic (adjacent ranges collapse;
  reverse ranges cancel forward ranges of the same rev).

### Not implemented yet

- Real authentication beyond the `X-Svnae-Superuser:` token stub
- TLS (deferred; plain HTTP today)
- Hooks (pre/post-commit scripts)
- Locks (`avn lock`/`unlock`)
- `svn:externals`
- `libmagic` replacement (extension-based mime stub when needed)

## Where the code lives

```
util/           foundations — error, path, checksum, compress, io,
                sqlite, utf8proc bindings. Each is a C shim + .ae
                wrapper + a test.
delta/          svndiff1 encode/decode, xdelta match-finder.
repo_storage/   content-addressable blob store, revisions, txn,
                server-side copy, commit finalisation.
repos/          query API (log, cat, list, info) on top of repo_storage.
client/         REST client (libcurl + cjson).
svnserver/      HTTP server wrapping repos/, per-route handlers,
                commit/copy POST endpoints.
working_copy/   working copy: db, pristine, checkout, status,
                mutate (add/rm), commit, update, revert, diff,
                cp/mv, props, merge.
avn/            the avn CLI.
avnadmin/       avnadmin CLI (create/dump/load).
ffi/            third-party FFI bindings (openssl).
```

Each module has one or two C `shim.c` files (the real implementation)
and an Aether wrapper/driver. Tests are either in-language `.ae` or
`.sh` drivers that exercise the whole stack through built binaries.

## Documentation

- **[PORT_STATUS.md](./PORT_STATUS.md)** — phase map, feature matrix, commit-by-commit progress.
- **[branching_connnundrum.md](./branching_connnundrum.md)** — branching design (Option B, include-only specs, Piper-scale target).
- **[AETHER_ISSUES.md](./AETHER_ISSUES.md)** — running list of Aether bugs/rough edges found during the port, triaged against the current Aether main.

## Licensing

Aether sources: Apache License 2.0. Header on each file:

```
Copyright 2026 Paul Hammant (portions).
Portions copyright Apache Subversion project contributors (2001-2026).
```

The `libsvn_*/` C tree retains its original Apache-2.0 headers verbatim.
