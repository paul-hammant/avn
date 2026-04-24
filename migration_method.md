# Migration method — C → Aether

This is the working method we've used to drive a C codebase (roughly
26k LOC of hand-written source) from ~100% C to under 37% C while
keeping every test green on every commit. It's written after the fact
from the patterns that actually repeated, not from the plan we started
with. Most of the technique didn't exist in round 1; each round taught
the next.

It's specific enough to follow in another project without re-deriving
the same lessons, and honest about the places where "keep pushing"
stopped being valuable.


## Ground rules

1. **Tests stay green on every commit.** Non-negotiable. A round that
   leaves tests red is rolled back before it's committed, not
   "fixed up in the next one." When a port goes sideways the bisect
   evidence must point at exactly one commit.

2. **One port per commit, where possible.** A commit either does a
   targeted knot-cut (one structural change) or a bounded leaf sweep
   (one pattern, many sites). Never both. Reviewable in isolation.

3. **MDOLD: Most-Depended-On, Least-Depending first.** When two
   functions could move, pick the one with more callers and fewer
   dependencies. That function's port unlocks all its callers'
   future ports; the other direction gets stuck. `fs_fs/rep_store`
   and `ae/repos/*` moved before `svnserver` and `svn`.

4. **The C code is a reference, not a target.** We're reading the
   original svn C to learn the behaviour contract — not to
   translate line-by-line. An idiomatic Aether implementation is
   the goal. Line-count growth on the Aether side is often fine;
   line-count reduction on the C side is what we're actually
   measuring.

5. **Only port what has a port target.** If a function is inherently
   about sqlite, curl, zlib, fnmatch, or binary fd I/O, leave it in
   C. The porting objective isn't "0% C"; it's "move the logic that
   Aether can carry without new runtime."


## The round structure

Each round is roughly:

1. **Audit.** List the largest remaining C shims by LOC.
2. **Classify each function.** Pure text / path / data-structure
   logic → portable. sqlite / curl / zlib / fork / binary I/O →
   stays C. Mixed → decompose.
3. **Pick one target.** Either a big knot (a single structural
   change) or a small pattern sweep (many call sites, one
   mechanical rewrite).
4. **Port.** Write the Aether side. Add the C-side glue (usually
   an accessor wrapper). Delete the old C.
5. **Build.** Clean compile is the first gate.
6. **Run the full suite.** 32 tests, ~60 seconds. Every round.
7. **Commit.** Message explains *why*, not just *what* — what shape
   the knot had and how the new shape differs.
8. **Update the status doc** if the round was substantial.

"Skipping the suite because it was a tiny change" breaks rule 1.
Don't.


## The patterns we actually used

These are listed in rough order of how often they come up. Most of
them were invented by round 21 and reused with small variations in
every subsequent round.

### 1. The packed-handle pattern

This is the single biggest thing we figured out. It replaces
"C struct-of-arrays with per-field accessors" with "Aether produces
a packed string, C holds it as the handle."

**The knot shape.** Many C functions returned a handle like:

```c
struct svnae_log { struct log_entry *entries; int n; };
// entries[i] = {rev, author, date, msg} — four strdup'd strings
```

An Aether side built the same content, usually via a `_joined`
parallel-strings protocol (Aether can't pass a `char **` across FFI),
and the C side reparsed + strdup'd into the struct. Multiple
near-identical tokeniser loops ended up sprinkled around.

**The replacement.** Have Aether produce a packed string:

```
"<N>\x02<f0>\x01<f1>\x01...\x02<f0>\x01<f1>\x01...\x02..."
```

— `\x02` separates records, `\x01` separates fields within a record.
The C handle collapses to:

```c
struct svnae_log { char *packed; int n; struct pin_list pins; };
```

Per-field accessors become:

```c
const char *svnae_ra_log_author(struct svnae_ra_log *lg, int i) {
    if (!lg || i < 0 || i >= lg->n) return "";
    return pin_str(&lg->pins, aether_ra_log_author(lg->packed, i));
}
```

Where `aether_ra_log_author` is an Aether function that walks the
packed string on demand. The `pin_list` is a per-handle free-list of
strdup'd copies of returned strings, drained when the handle frees.
This is the stable-pointer contract callers always had — without it,
a caller that holds one accessor's return across another accessor
call on the same handle gets corrupt data.

**Why pin-per-handle and not TLS.** A per-accessor TLS buffer looked
attractive (one allocation, overwrite on next call). It breaks under
recursion: if `walk(prefix)` calls `list_name(L, i)` to get `name`,
then recurses with `prefix = name`, the recursive walk's own
`list_name` call clobbers the outer `name`. Per-handle pin lists
survive because each handle owns its own.

**Why packed strings and not AetherString*.** Aether's string runtime
has reference counting but the accessors often return
`string_substring` results — bare `malloc`'d buffers, not refcounted.
Trying to free or adopt them across the FFI crashes. The pin list
sidesteps the question entirely by strdup'ing once.

**Reuse across callers.** The same `\x02`/`\x01` record shape gets
used by multiple call sites. Server-side log (`ae/repos/log.ae`),
server-side paths-changed, server-side blame — all of them reuse
the same `aether_ra_log_*` / `aether_ra_paths_*` / `aether_ra_blame_*`
accessors from `ae/ra/packed.ae`. One set of walkers, many producers.


### 2. Drop the split-to-rejoin round trip

**The knot shape.** Aether builds a `\n`-joined string of parallel
values (because it can't pass a `char **` across FFI). A C wrapper
splits that into a `char **` array. A C function consumes the array.
Sometimes — we found one of these — the C function then *rejoins*
the array into `\n`-joined form to pass to an Aether library it
calls.

**The fix.** Have the target Aether or C function accept the joined
form directly. Drop the splitter and (if present) the rejoiner.

Round 23 cut four `svnserver_build_*_joined` wrappers (~85 LOC C) +
four `svnae_build_*_blob` C functions (~100 LOC) this way. Round 25
cut `svnserver_branch_create_globs` and its matching rejoin inside
`svnae_branch_create` — a single-layer split-to-rejoin that was the
silliest example.


### 3. Inline the trivial C-side trampoline

**The knot shape.** A static C function that does nothing but
forward to `aether_*` with the same arguments:

```c
static int mkdir_p(const char *path) {
    return aether_io_mkdir_p(path) == 0 ? 0 : -1;
}
static int head_rev(const char *repo) {
    return aether_repos_head_rev(repo);
}
```

**The fix.** Delete the wrapper, rewrite callers to call the
`aether_*` function directly.

Sanity check before each one: count call sites. One or two — always
inline. A dozen or more — the short alias earns its keep;
`write_atomic` in `fs_fs/commit_shim.c` has 12 uses, inlining would
be churn.

Also check the type conversion actually does something. `aether_*`
functions return 0 on success and 1 on failure; many wrappers
converted to 0/-1. Callers always test `!= 0` so the wrapper was
pure renaming.


### 4. Collapse chains of trivial wrappers

A specific case of pattern 3 we saw more than once:

```c
int svnserver_acl_allows(...)       → calls acl_allows
static int acl_allows(...)           → calls acl_allows_mode
static int acl_allows_mode(...)      → calls aether_acl_allows_mode
```

Three layers of renaming. Flatten to one layer:

```c
int svnserver_acl_allows(...) {
    return aether_acl_allows_mode(repo, rev, user, path, 0);
}
```


### 5. Drop the struct + accessors when there's exactly one caller

Occasionally a small struct + accessor family exists because it looks
like library shape, but only one caller consumes it. The
struct+accessors + free function + forward decls weigh 40+ LOC. The
single caller's needs often fit in a tuple or direct return.

Don't do this speculatively. Only when you're looking at a dead
decl sweep and notice "this is only called from one place."


## Tooling specific to the C/Aether boundary

- **Every Aether lib file gets a `_generated.c`** produced by
  `aetherc --emit=lib`, listed in `aether.toml`'s `extra_sources`,
  linked into every binary that uses it. Makefile.regen per
  directory keeps the recipes local.

- **The gcc link command has an argv length limit around 2 KB.**
  Not really gcc's limit — the aether build tool's own
  `char toml_extra[2048]` assembly buffer silently truncates
  `extra_sources` when their concatenated length exceeds 2 KiB,
  which in turn hands gcc a mangled path like `handler_copy_generat`
  instead of `handler_copy_generated.c`. We've hit this twice:
  round 23 (commit_parse blob builders) and round 30 (fs_fs
  rep_store port). Mitigations that actually work: (a) inline
  the contents into an existing `_generated.c` path that's
  already on the link line, or (b) consolidate two or more
  small `.ae` modules into one. Filing as AETHER_ISSUES.md #17
  — the parse-side buffer was bumped to 8 KiB in v0.85 but the
  assembly-side buffer wasn't. Until that's fixed, new
  `_generated.c` additions to svnserver in particular have a
  real cost.

- **`match`, `state`, `after`, `message`, `ptr` are reserved
  keywords.** Renaming pattern: `match_arr`, `st`, `tail`, `msg`.

- **`string.copy(s)` is the snapshot idiom** for C TLS returns and
  for detaching from strings whose producer might reallocate.
  Applies whenever you're about to make another Aether call whose
  runtime might mutate the string you still hold.

- **`std.intarr` for DP tables and packed (offset, length) pairs.**
  Use `intarr_get_unchecked` / `intarr_set_unchecked` in validated
  inner loops, the `_raw` or wrapper forms at boundaries.

- **Ensure a single-character string via `substring(s, i, i+1)`.**
  There's no `string.from_char(int)` in std.string today; substring
  is the cheapest path for encoding a known `\x01`/`\x02`-delimited
  action char back into a string.

- **If you reach into `AetherString` directly from C, match the
  real layout byte-for-byte.** The struct in `aether_string.h` is:

  ```c
  typedef struct AetherString {
      unsigned int magic;     // 4 bytes
      int          ref_count; // 4 bytes
      size_t       length;    // 8 bytes on 64-bit
      size_t       capacity;  // 8 bytes on 64-bit
      char        *data;
  } AetherString;
  ```

  A first draft that used `int32_t length` silently mis-read the
  length field (really at offset 8, not offset 4), picked up random
  bytes from inside `capacity`, and fed a bogus byte count into
  `memcpy` — crash on a plausible-looking (but garbage) pointer
  like `sdata=0x5`. Symptoms look like memory corruption upstream
  of where the real bug is. Prefer using `string_length(s)` +
  `string_char_at(s, i)` when you can; when you genuinely need
  to reach into the struct, `#include "aether_string.h"` rather
  than re-declaring the layout.

- **New stdlib capabilities sometimes land after you need them —
  keep an eye on the Aether `CHANGELOG`.** We held off on porting
  the WC pristine store until `std.cryptography` (0.88) and
  `std.zlib` (current) existed upstream; once they did, ~200
  LOC of C came out in a single round. The lesson the other
  way: if a target depends on missing stdlib, park it with a
  note and check back, don't work around — we wasted a day once
  before realising an upstream feature was a week out.


## Measuring

We track C LOC as `find ae -name "*.c" -not -name "*_generated.c" |
xargs wc -l` — hand-written only. Aether LOC is `find ae -name
"*.ae" | xargs wc -l`. The generated `.c` files are gitignored and
not counted on either side.

A single LOC-in-Aether is not equal to a single LOC-in-C — Aether is
chatty (`string.concat` everywhere, explicit indexing). So the ratio
matters, not the totals. On this port we went from roughly 100% C
pre-port to **36.81% C** at round 28, across ~26k LOC total. The
remaining ~37% is genuinely native C work (curl lifecycle, sqlite,
zlib, binary fd streams).


## When the round doesn't pay off

Not every candidate is worth doing. Some we looked at and shelved:

- **Small leaves.** Sweeping 20 LOC for a 0.1pp gain isn't
  productive unless it's part of a larger pass. Save for a
  clean-up round.

- **Mostly-C-native functions.** If a function is 80% sqlite calls
  and 20% text assembly, the text can move but the function stays.
  Often the 20% isn't worth splitting into a separate Aether entry.

- **Stream I/O.** `svnae_svnadmin_load` reads a dump file one line
  at a time via raw `read(2)`. Aether's `std.fs` doesn't expose an
  incremental line reader. The function stays C.

- **Binary content.** ~~Anywhere we hold arbitrary bytes with
  embedded NULs...~~ This changed mid-port. `fs.read_binary`
  (0.82) / `fs.write_binary` (0.86) plus `string_new_with_length`
  preserve embedded NULs through an AetherString's explicit
  length field. `std.zlib` (0.90) and `std.cryptography` (0.88)
  follow the same convention. So a round that looked impossible
  (pristine store, SHA + zlib + binary files) was clean after
  the stdlib caught up. Worth re-checking the changelog every
  few rounds — capabilities arrive faster than you'd expect.


## Round dependencies in practice

The order we did things mattered more than we initially thought.
The patterns above needed each other:

- The packed-handle pattern (round 21) depended on `std.intarr`
  existing (added mid-port in Aether 0.83).
- Rounds 22, 26, 28 all reused `ae/ra/packed.ae`'s accessors —
  none of them would have been clean ports without round 21's
  infrastructure.
- Round 25 (drop the globs rejoin) depended on already having
  `aether_filter_dir` accept a joined string, which was done in
  an earlier round we don't remember precisely but before ~20.

Lesson: **build the reusable infrastructure first, even if its
first use case isn't the biggest ticket item**. The `packed.ae`
accessors were modest on their own (round 21 was 1507 → 1445
LOC in ra/shim.c, a single-file shrink). But they made rounds 22,
26, 28 each cleanly possible.


## What we'd change starting over

- **Start with a packed-string convention earlier.** We spent
  rounds 2–20 using ad-hoc per-call schemes (`\n`-joined strings
  for some things, custom packed formats for others, C structs
  everywhere). A single convention — the `\x02`/`\x01` packed
  records — applies everywhere and would have saved multiple
  re-refactors.

- **Write the status doc *as* each round lands.** Gaps between
  "round 7" and "round 18" in `PORT_STATUS.md` make it harder to
  bisect our own history. The doc is telemetry; write each entry
  when the commit is fresh.

- **Don't chase the last percent.** At ~36% C the remaining shims
  are genuinely C work. Trying to drive it to "as low as possible"
  would start producing hostile ports — Aether code that's longer,
  slower, and harder to read than the C it replaces. Stop when
  the ports stop being clean.
