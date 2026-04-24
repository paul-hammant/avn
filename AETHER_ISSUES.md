# Aether issues found during the Subversion port

Running list of bugs, limitations, and rough edges hit while porting
Subversion (`~/scm/subversion/subversion/`) to Aether. Each entry has:
repro, observed, expected, severity, workaround, and current status
against main.

Originally filed against **Aether v0.70.0**. Tracked against
**main at v0.76.0** (2026-04-21).  Platform: Linux x86_64, gcc backend,
dev mode.

Items that were confirmed fixed on later main versions have been pruned.
Git history has the original bug reports verbatim if we need to re-read
the forensics.

## Status summary

| # | Title | Severity | Status |
|---|---|---|---|
| 4 | No `__FILE__` / `__LINE__` intrinsics | minor | ⏳ Confirmed, open |
| 7 | `extra_sources` in `[[bin]]` not applied when building via file path | minor | 📝 Deferred — three sub-bugs, see `claim_7_discussion.md` |
| 9 | Build cache doesn't invalidate on `extra_sources` changes | minor | ⏳ Not yet investigated on main |
| 11 | `string.concat("", s)` is strlen-based | missing API | ⏳ Not yet investigated (likely still present) |
| 12 | `extra_sources` array must be single-line | minor | ⏳ Known limitation in `get_extra_sources_for_bin` |
| 16 | `extern` with 6+ params drops the last | blocker | ⏳ STILL reproduces for the port's actual shape |

**Legend:** ⏳ open / needs work &middot; 📝 deferred with design notes

---

## 4. No `__FILE__` / `__LINE__` intrinsics

**STATUS: ⏳ CONFIRMED, OPEN.** `main() { println(__FILE__) }` fails with `E0300: Undefined variable '__FILE__'`. C-side stdlib uses `__FILE__`/`__LINE__` in `std/log/aether_log.h`, but there's no Aether-level equivalent.

**Severity: minor**, but painful for error handling at scale.

**Need:** svn-style error chains record the source location where each error
link was created. With no compiler intrinsics, every call site has to pass
`"file.ae"` and the literal line number as string/int:

```aether
err = error.create(ERR_IO_OPEN, "cannot open", "wc/update.ae", 423)
```

This bit-rots instantly when files are edited (line numbers shift) and
duplicates information the compiler already has.

**Request:** add `__FILE__` and `__LINE__` as either compiler intrinsics or a
macro-like mechanism that expands at the call site. C's `__FILE__`/`__LINE__`
are the obvious model.

**Fix shape (sketch):** parser-level substitution at the token site — the same rule C uses. Substituting at the call site (not the definition site) is the critical property so that a wrapper like `error.create(...)` records the caller's line.

---

## 7. `extra_sources` in `[[bin]]` does not apply when building via file path

**STATUS: 📝 DEFERRED — discussion at `claim_7_discussion.md` (repo root).** The reported single bullet is actually three separate sub-bugs, each needing a maintainer design call:

- **7a.** Absolute-path invocation doesn't match `[[bin]] path = "..."` (match logic strips `./` but not `/full/path`).
- **7b.** `ae build <bin_name>` — no name-to-path resolution exists; CLI treats the arg as a file path unconditionally.
- **7c.** `aether.toml` is looked up only in cwd — no walk-up like cargo/go/npm do.

The reporter's original "`ae build <file.ae>` does not pull in extra_sources" claim did NOT reproduce with a relative path from the project root — so they almost certainly hit one of 7a/7b/7c. Fix ordering suggested in the discussion doc: docs first, then 7a (smallest), then 7b (design choice on conflicts), then 7c (behavior change, riskiest).

**Severity: minor** — took a while to debug.

---

## 9. Build cache doesn't invalidate on `extra_sources` C file changes

**STATUS: ⏳ NOT YET INVESTIGATED** on current main.

**Severity: minor, but bites debugging badly.**

Editing a C file listed in `[[bin]].extra_sources` does NOT cause `ae run`
to rebuild. The test keeps running against the stale shim. `ae cache clear`
fixes it, but it's easy to chase the wrong bug for 20 minutes first.

**Request:** hash the `extra_sources` contents (not just the `.ae` file)
when computing the cache key.

---

## 11. `string.concat("", s)` for binary-safe copy is strlen-based

**STATUS: ⏳ NOT YET INVESTIGATED** on current main. (Likely still present — `string.concat` by its C implementation will call `strlen` unless changed, so this is design-level, not a regression.)

**Severity: documentation / missing API.**

The only obvious idiom for "copy this raw C `char*` into an Aether-managed
string" is `string.concat("", s)`. But `string_concat` uses `strlen` on
plain-ptr inputs (via `str_len`), so copied strings can't carry embedded
NULs, and any trailing bytes past the caller's intended length leak in
until the next NUL.

Specifically, if the source buffer at `s` contains N real bytes followed
by a NUL, but the source's internal length field says N-k or N+k, strlen
reads whatever's in memory until the first NUL — could be too few, could
be too many. This was caught in the FSFS port when `buf_new` in
`ae/subr/compress/shim.c` didn't NUL-terminate its output buffer: strlen
walked past the 10000-byte payload into neighbouring heap memory and
returned 10011.

**Requests:**
- Add `string.from_bytes(p: ptr, len: int) -> string` that explicitly
  respects a caller-provided length. This is the obvious missing API.
- Document that `string.concat("", p)` is strlen-based and unsafe for
  binary data, so nobody else relearns this the hard way.

**Workaround used in port:** for binary payloads, keep them in an opaque
buf handle (svnae_buf) and pass the handle through `ptr`. Only turn the
bytes into an Aether string when they're known to be text, and always
NUL-terminate when crossing a boundary (every buf_new/buf_from in the
port's shims now does `data[n] = '\0'`).

---

## 12. `extra_sources` array must be single-line in aether.toml

**STATUS: ⏳ KNOWN LIMITATION.** `get_extra_sources_for_bin` in `tools/ae.c:970` is explicit: "Only handles single-line arrays." Fix is a TOML parser upgrade, not a regression to hunt.

**Severity: minor.**

A multi-line TOML array like

```toml
extra_sources = [
    "a.c",
    "b.c",
]
```

silently fails to be picked up by `ae run <file>` — the linker then complains
about undefined symbols. Single-line `extra_sources = ["a.c", "b.c"]` works.

**Request:** fix the TOML parser to accept the multi-line form (which is
standard TOML), or emit a clear error when the `[[bin]]` entry for the
file being run has an unparseable `extra_sources`.

---

## 16. `extern` declarations with 6 or more params drop the last param

**STATUS: ⏳ STILL REPRODUCES** on v0.76.0 for externs matching the port's actual shape. Shim-snip attempt on 2026-04-21 tried to remove the packed-int workaround for `svnae_wc_db_upsert_node`; the direct 6-param extern `(db: ptr, path: string, kind: int, base_rev: int, base_sha1: string, state: int) -> int` regenerates the original failure:

```
/tmp/svnae_test_db.c:402:6: error: variable or field 'rc' declared void
/tmp/svnae_test_db.c:264:6: note: declared here
  264 | void svnae_wc_db_upsert_node(void*, const char*, int, int, const char*);
```

Two separate symptoms visible:
  - 6th param (`state: int`) silently dropped from the emitted forward-declaration.
  - `-> int` return annotation dropped too; declaration is `void`.

An earlier "does not reproduce" triage was done with an isolated extern that happened not to trigger the bug. The shape that still breaks is: 6 params, mixed ptr/string/int, with a `-> int` return annotation, imported at the top of a `main()`-bearing `.ae` file. The packed-int workaround (`ks = (state << 4) | kind`) has been kept in place.

**Severity: blocker for wide APIs.**

**Workaround used in port:** pack two ints into one (e.g., `state << 4 |
kind`) or split into two C functions.

**Request:** fix extern param parsing to accept any arity. If there's a
hard limit in the runtime, raise it or document it; the current
behaviour is silent data loss.

---

## #17 — `extra_sources` silently truncated at 2 KiB (ae 0.89.0)

Hit during round 30 (fs_fs rep-store port attempt). Adding one more
`_generated.c` to the `[[bin]] extra_sources` line for the
aether-svnserver binary — total 2271 chars on one logical line —
caused the link command to be cut mid-filename, with the linker
reporting `cannot find ae/svnserver/handler_copy_generat: No such
file or directory`. Subtracting 40 chars from the line brought the
build back.

**Where the limit lives:** `tools/ae.c` has

```c
char toml_extra[2048] = "";
get_extra_sources_for_bin(file, toml_extra, sizeof(toml_extra));
```

at the `build_gcc_cmd` call site, with a 2 KiB buffer. The
**parse-time** buffer was bumped to 8 KiB in v0.85 (changelog entry
fixes `fgets` silently dropping lines past 1 KiB); the assembly-time
`toml_extra[2048]` wasn't.

**Failure mode:** *silent* — no warning at parse time; the linker
error talks about a truncated filename (`handler_copy_generat`) that
clearly used to be a real path, but nothing points at `extra_sources`
as the culprit.

**Severity:** medium. Every non-trivial project hits this the moment
`extra_sources` spans enough shims. svnserver's line had 63 entries.

**Request:** (a) bump the `toml_extra` buffer to match the parser's
8 KiB (or better, make it grow dynamically), and (b) when
`get_extra_sources_for_bin` truncates, emit a clear diagnostic
(`aether.toml: extra_sources exceeds NNNN-byte assembly buffer —
split the line or raise AETHER_TOML_EXTRA_BUF`) instead of letting
the linker see a mangled command.

**Workaround used in port:** inline the affected Aether module into
a sibling that's already on the link list, so no new `_generated.c`
path is added. Done previously for `ae/svnserver/blob_build.ae`
(round 23) — the cost starts to hurt the "one Aether module per
concern" design once it happens repeatedly.

---

## Postscript — triage history

First triage pass against v0.74.0 / v0.75.0, then again against v0.76.0
(2026-04-21). Ten issues were confirmed fixed or not-reproducible across
those passes:

- **#1** (`-> ReturnType` arg miscount) — FIXED by PR #190.
- **#2 / #3** (ptr param erasure) — no longer reproduced; the port's original workaround of avoiding `ptr` params across module boundaries is no longer needed.
- **#5** (reserved keyword error message) — FIXED by PR #192. The contextual-keyword idea for `message` specifically was declined.
- **#6** (gcc warnings leak) — PR #192 made `[build] cflags` apply to `ae run` as well as `ae build`, so projects can opt-out per-toml. Default-suppress was declined.
- **#8** (two calls in one comparison hang) — FIXED by PR #192. Was a parser trailing-block issue, not extern-specific.
- **#10** (sibling block scope leak) — FIXED by PR #194.
- **#13 / #14** (recursive string return / subsequent-call poisoning) — did not reproduce in isolated repros; likely downstream effects of #1. Not fully unwound in the port yet — `txn_shim.c`'s `rebuild_dir_c` is a 330-line candidate port we've held off on.
- **#15** (`!` on non-bool) — does not reproduce on v0.76.0.

Net effect on port framing: the "type-system cluster" story from the
v0.70.0 report has evaporated. What's left is small-surface ergonomics
(#4, #11) and toml/build-system edges (#7, #9, #12), plus one real
extern-parser bug (#16) whose fix unlocks a handful of packed-int
workarounds.
