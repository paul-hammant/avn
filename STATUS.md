# svn-aether port — status

A clean-sheet reimplementation of Apache Subversion in the Aether
systems language. Not a mechanical C → Aether translation — we read
the svn C tree as a *reference for behaviour contracts* and wrote
our own on-disk formats, wire protocol, and algorithms. The reference
svn C code still lives at `subversion/libsvn_*` in this repo, read-only.

Plan document: `../svn-to-aether.md`
Aether bug/feature feedback: `AETHER_ISSUES.md`

## Headline

- **41 commits.** Each phase is its own commit, reviewable in isolation.
- **33 test suites** (added `test_merge_reverse.sh`), ~339 assertions, all green.
  Mix of in-language `.ae` tests and end-to-end shell harnesses that
  spin up a real HTTP server and drive it with curl and the built
  `svn` CLI.
- **~14,400 lines** total (Aether + C shims + shell test drivers).
- Daily-driver workflow works end to end: checkout → edit → status
  → diff → add → rm → commit → update → branch → merge.

## Binaries we ship

| Binary | What it does |
|---|---|
| `svn` | Client CLI: `checkout`, `status`, `add`, `rm`, `cp`, `mv`, `commit`, `update`, `revert`, `diff`, `log`, `cat`, `ls`, `info`, `propset`/`get`/`del`/`list`, `merge` |
| `aether-svnserver` | Read/write HTTP server. JSON for metadata, base64 for bytes over JSON, raw bytes for cat. |
| `svnadmin` | Repository admin: `create`, `dump`, `load` (our own portable dump format; not reference-svn compatible by design). |
| `svnae-seed` | Test-only helper that populates a repo with a known tree across three commits. |

## Running the stack

```bash
ae=/home/paul/scm/aether/build/ae

# Build what you need, or run the full test sweep:
for f in ae/svn/main.ae ae/svnserver/main.ae ae/svnserver/seed.ae \
         ae/svnadmin/main.ae; do
    "$ae" build "$f" -o "/tmp/$(basename "${f%.ae}")"
done

# Or from the project root:
/tmp/svnadmin create /srv/repo
/tmp/main demo /srv/repo 8080 &     # that's aether-svnserver
/tmp/main checkout http://localhost:8080/demo wc
```

Run all 30 test suites: a bash one-liner at the end of this doc.

## Phase map

Named after the plan's Phase N. Plan: `../svn-to-aether.md`.

| Phase | Title | Status | Commit |
|------:|:------|:-------|:-------|
| 0.1 | FFI smoke tests (sqlite, zlib, utf8proc, openssl) | ✅ | `d39e669` |
| 0.2 | Error chain with file/line | ✅ | `ba4a69c` |
| 0.3 | Path handling (relpath + dirent) | ✅ | `da5c0a6` |
| 0.4 | Checksums + zlib + atomic I/O | ✅ | `f729696`, `dd22308` |
| 0.5 | SQLite binding | ✅ | `1de5530` |
| 1-lite | utf8proc NFC binding | ✅ | `1be7911` |
| 2 | svndiff1 encode/decode + xdelta round-trip | ✅ | `6b8f0d1`, `879f576` |
| 3.1 | FSFS content-addressable blob store | ✅ | `a9648ee` |
| 3.2 | Revisions + rep-sharing across revisions | ✅ | `f1aebfe` |
| 3.3 | Nested dirs + txn API | ✅ | `df9d77c` |
| 4 | libsvn_repos query API (log, cat, list, info) | ✅ | `9d032af` |
| 6 | aether-svnserver (read-only HTTP) | ✅ | `c6cef15` |
| 6.5 | POST /commit (read/write loop closed) | ✅ | `dfd6567` |
| 7 | libsvn_ra (Aether-callable REST client) | ✅ | `b411afa` |
| 10 (stateless subset) | svn CLI | ✅ | `f90f31d` |
| 5.1 | wc.db metadata store | ✅ | `f882fb6` |
| 5.2 | Pristine store | ✅ | `bb865f1` |
| 5.3 | Checkout | ✅ | `69ec5bc` |
| 5.4 | Status | ✅ | `f009280` |
| 5.5 | add/rm | ✅ | `b1f7c79` |
| 5.6 | WC-backed commit | ✅ | `211ba67` |
| 5.7 | svn update | ✅ | `5aa005e` |
| 5.8 | svn revert + svn diff | ✅ | `1ffafb8` |
| 5.9 | svn cp / mv (WC-local) | ✅ | `b6f2741` |
| 5.10 | WC-side properties | ✅ | `2c37fc4` |
| 5.11 | svn:ignore filter for status | ✅ | `6202fb5` |
| 5.12a | Branches (server-side copy) | ✅ | `94c06f0` |
| 5.12b | svn merge + svn:mergeinfo | ✅ | `b04433c` |
| 5.13 | 3-way conflict resolution + `svn resolve` | ✅ | (prev commit) |
| 5.14 | Server-side properties (propset round-trips through commit/checkout/update) | ✅ | (prev commit) |
| 5.15 | `svn switch` — relocate WC to a different branch URL, reusing update pipeline | ✅ | (prev commit) |
| 5.16 | `svn merge -c N` cherry-pick + reverse merge (`-r A:B` with A>B or `-c -N`) | ✅ | (this commit) |
| 12 | svnadmin create/dump/load | ✅ | `52380a5` |

## Phases not yet done (from the plan)

| Phase | Title | Why deferred |
|------:|:------|:-------------|
| 0 item 5 | TLS transport (HTTPS) | Deferred per user direction. Server is plain HTTP today. |
| 8 (auth) | Basic/Bearer auth over TLS | Same — deferred. |
| 11 | libmagic replacement | Not on critical path; stub with extension-based mime detection when needed. |

## Feature matrix — reference svn vs svn-aether

Implemented:

- `checkout`, `update`, `switch`, `status`, `commit` (both URL-flag stateless and WC-backed)
- `add`, `rm`, `cp`, `mv`, `revert`, `diff`
- `log`, `cat`, `ls`, `info` (all over the wire)
- `propset`/`get`/`del`/`list` (WC-local)
- `svn:ignore` read by status
- Server-side copy (`svn cp URL URL`) — full-subtree rep-sharing
- `svn merge` — forward rev-range (`-r A:B`, A<B), reverse (A>B), and
  cherry-pick (`-c N` or `-c -N` for reverse). Classic `URL@A:B` form
  still supported. Always uses 3-way merge so cherry-pick onto a tree
  the source never saw does the right thing.
- `svn:mergeinfo` recorded (append-only; reverse ranges written with
  leading '-'). Range arithmetic / cancellation still deferred.
- `svnadmin create/dump/load` with portable dump round-trip

Not implemented (items the plan calls out or that reference svn has):

- HTTPS / TLS transport
- Authentication + authz
- Mergeinfo range arithmetic (cancel forward/reverse of same range)
- `svn log --verbose` (per-rev path changes)
- `svn blame`
- Hooks (pre/post-commit scripts)
- Path-based authz
- Locks (`svn lock`/`unlock`)
- `svn externals` (svn:externals property pulling sub-WCs)
- `svn cleanup` (WC crash recovery)
- Prop-delete propagation on update (prop-add/change works; prop-remove
  does not currently remove the WC-local entry — deferred)
- Pre-1.7 WC compatibility (intentionally dropped)
- Reference-svn dump-format compatibility (intentionally dropped;
  we have our own portable format)

## Where the code lives

```
ae/
  subr/           foundations — error, path, checksum, compress, io,
                  sqlite, utf8proc bindings. Each is a C shim + .ae
                  wrapper + a test.
  delta/          svndiff1 encode/decode, xdelta match-finder.
  fs_fs/          content-addressable blob store, revisions, txn,
                  server-side copy, commit finalisation.
  repos/          query API (log, cat, list, info) on top of fs_fs.
  ra/             REST client (libcurl + cjson).
  svnserver/      HTTP server wrapping repos/, plus per-route handlers
                  and the commit/copy POST endpoints.
  wc/             working copy: db, pristine, checkout, status,
                  mutate (add/rm), commit, update, revert, diff,
                  cp/mv, props, merge.
  svn/            the svn CLI.
  svnadmin/       svnadmin CLI (create/dump/load).
```

Each module has one or two C `shim.c` files (the real implementation)
and an Aether wrapper/driver. Tests are either in-language `.ae` or
`.sh` drivers that exercise the whole stack through built binaries.

## Design choices the plan gave us, and we kept

- **Clean-sheet storage:** not byte-compatible with reference svn's
  FSFS. Our `$repo/reps/aa/bb/<sha1>.rep` layout is simpler and uses
  content-addressable SHA-1 keys directly.
- **Single RA, no plugin loader:** one wire protocol, no `ra_serf` /
  `ra_svn` / `ra_local` multiplexing.
- **REST + JSON + base64:** no WebDAV/DeltaV. Our five endpoints fit
  on one screen of text.
- **System libraries via FFI:** libsqlite3, libz, libutf8proc, libssl,
  libcrypto, libcurl, libcjson. No vendored sources.
- **APR as reference only:** the `apr/` directory at the repo root is
  a read-only copy of the C portability layer we're *replacing* with
  Aether-native code + FFI. We never linked against it.

## Aether feedback

See `AETHER_ISSUES.md`. 16 numbered issues with repros, severity, and
workarounds. TL;DR: the type-system cluster (#1, #2, #13, #14, #16)
caused us to write ~4k lines of C shims where Aether logic would have
been idiomatic. Fixing that cluster unlocks rewriting most of the shim
code in Aether. Everything else is ergonomics.

## Running all tests

```bash
cd /home/paul/scm/subversion/subversion

AE=/home/paul/scm/aether/build/ae

# In-language .ae suites
for t in error path utf8 checksum compress io sqlite svndiff xdelta \
         repo revisions txn repos; do
  case $t in
    svndiff|xdelta) f=ae/delta/test_$t.ae ;;
    repo|revisions|txn) f=ae/fs_fs/test_$t.ae ;;
    repos) f=ae/repos/test_$t.ae ;;
    *) f=ae/subr/test_$t.ae ;;
  esac
  printf '%-27s ' "test_$t"
  "$AE" run "$f" 2>/dev/null | tail -1
done
for t in wc_db wc_pristine; do
  printf '%-27s ' "test_$t"
  "$AE" run "ae/wc/test_${t#wc_}.ae" 2>/dev/null | tail -1
done

# End-to-end shell suites (spin up a real server, hit it with curl + svn CLI)
for t in server ra svn wc_checkout wc_status wc_mutate wc_commit \
         wc_update wc_revert_diff wc_cp_mv wc_props wc_ignore \
         wc_branch wc_merge svnadmin; do
  printf '%-27s ' "test_$t"
  case $t in
    server)   s=ae/svnserver/test_server.sh ;;
    ra)       s=ae/ra/test_ra.sh ;;
    svn)      s=ae/svn/test_svn.sh ;;
    wc_*)     s=ae/wc/test_${t#wc_}.sh ;;
    svnadmin) s=ae/svnadmin/test_svnadmin.sh ;;
  esac
  "$s" >/dev/null 2>&1 && echo "test_$t: OK" || echo "test_$t: FAIL"
done
```

Expected: 30 lines, every one ending in `OK`.
