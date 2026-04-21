# Aether issues found during the Subversion port

Running list of bugs, limitations, and rough edges hit while porting Subversion
(`~/scm/subversion/subversion/`) to Aether. Each entry has: repro, observed,
expected, severity, workaround, and current status against main.

Originally filed against **Aether v0.70.0**. Status lines triaged against
**main at v0.76.0** (2026-04-21). Platform: Linux x86_64, gcc backend, dev mode.

## Status summary

| # | Title | Severity | Status |
|---|---|---|---|
| 1 | `-> ReturnType` on user functions miscounts arguments | blocker | ✅ FIXED (PR #190, v0.75.0) |
| 2 | Cross-module `import` erases `ptr` to `int` | blocker | ❌ Does not reproduce on v0.75.0 |
| 3 | `ptr` literal `0` generated as `int` | minor | ❌ Does not reproduce on v0.75.0 |
| 4 | No `__FILE__` / `__LINE__` intrinsics | minor | ⏳ Confirmed, open |
| 5 | Reserved-keyword collisions with common names | minor | ✅ Error message fixed (PR #192); contextual-keyword idea open |
| 6 | gcc warnings leak through without `-v` | polish | ✅ `[build] cflags` now apply to `ae run` (PR #192); default-suppress declined |
| 7 | `extra_sources` in `[[bin]]` not applied when building via file path | minor | 📝 Deferred — three sub-bugs, see `claim_7_discussion.md` |
| 8 | Two calls in one comparison hang at runtime | blocker | ✅ FIXED (PR #192) — was not extern-specific |
| 9 | Build cache doesn't invalidate on `extra_sources` changes | minor | ⏳ Not yet investigated on main |
| 10 | Block-local `{ }` scoping leaks | minor | ✅ FIXED (PR #194) |
| 11 | `string.concat("", s)` is strlen-based | missing API | ⏳ Not yet investigated (likely still present) |
| 12 | `extra_sources` array must be single-line | minor | ⏳ Known limitation in `get_extra_sources_for_bin` |
| 13 | Return-type inference breaks for recursive string fns | blocker | ❌ Does not reproduce on v0.76.0 |
| 14 | Return-type inference poisons subsequent extern call sites | blocker | ❌ Does not reproduce on v0.76.0 |
| 15 | `!` only works on already-boolean values | minor | ❌ Does not reproduce on v0.76.0 |
| 16 | `extern` with 6+ params drops the last | blocker | ❌ Does not reproduce on v0.76.0 |

**Legend:** ✅ fixed on main &middot; ❌ does not reproduce &middot; ⏳ open / needs work &middot; 📝 deferred with design notes

---

## 1. `-> ReturnType` on user functions miscounts arguments at call sites

**STATUS: ✅ FIXED** on main (PR #190, v0.75.0). Parser now distinguishes `-> ReturnType { body }` from the Erlang-style `-> expr` / `-> { stmts }` arrow bodies when the token after `->` is a type keyword or an identifier followed by a non-struct-literal `{`.

**Severity: blocker** — any function signature with an explicit return type
fails to typecheck calls correctly.

**Repro** (standalone, no imports):

```aether
g(a: int, b: int) -> int { return a + b }
main() {
    r = g(1, 2)
    println(r)
}
```

**Observed:**

```
error[E0200]: Function 'g' expects 1 argument(s), got 2
```

The declared arity is 2 but the typechecker reports 1. Consistent off-by-one
regardless of how many typed params there are (`f(a,b,c,d,e) -> int` is
reported as "expects 4").

**Expected:** program prints `3`.

**Workaround (no longer needed):** omit the `-> ReturnType` annotation. Aether infers it:

```aether
g(a: int, b: int) { return a + b }   // works, returns int
```

---

## 2. Cross-module `import` silently erases `ptr` parameter types to `int`

**STATUS: ❌ DOES NOT REPRODUCE** on v0.74.0 or v0.75.0. The minimal repro as quoted generates correct C: `static int mymod_take(void*);` and the call site casts `0` through `(void*)(intptr_t)(0)`. Runs cleanly, prints the pointer-null branch, no segfault.

Possibilities:
- Reporter was on an older version where this genuinely broke.
- They hit a different bug and misattributed it to ptr-param handling.
- Their repro had something else going on (different import shape, different call-site types, something platform-specific).

Worth re-filing with the exact aetherc version + generated C if it still bites.

**Severity as filed: blocker** — crashes at runtime with truncated pointers on 64-bit.

**Repro:**

```aether
// ae/subr/mymod/module.ae
export take(p: ptr) {
    if p == 0 { return 0 }
    return 1
}
```

```aether
// main.ae
import mymod
extern strdup(s: string) -> ptr
main() {
    s = strdup("hi")
    r = mymod.take(s)         // claimed crashes or wrong answer
    println(r)
}
```

**Originally observed:** generated C declares `static int mymod_take(int p)` — the `ptr`
type is dropped, the argument is received as a truncated `int`.

**Currently observed on v0.75.0:** `static int mymod_take(void*)`. Correct.

**Impact on port (as filed):** 10-library architecture assumes modules can pass `ptr`s (error chains, file handles, opaque tree nodes) to each other.

---

## 3. `ptr` literal `0` gets generated as `int` in cross-module call sites

**STATUS: ❌ DOES NOT REPRODUCE** on v0.75.0. `mymod.take(0)` emits `mymod_take((void*)(intptr_t)(0))` — cast through `intptr_t` then to `void*`. Compiles clean under `-Werror=int-conversion`. Either already fixed upstream or never reproduced as stated.

Related to #2 but claimed to show up even when callee types are correct. Passing a
literal `0` where a `ptr` is expected was said to generate C code with a bare `0` that
gcc treats as `int` and warns on `-Wint-conversion`.

**Original request:** treat `0` in a `ptr` context as `(void*)0`. Already the case.

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

## 5. Reserved-keyword collisions with common identifier names

**STATUS: ✅ ERROR MESSAGE FIXED** on PR #192. The error now names the offending keyword and suggests a rename: `'message' is a reserved keyword and cannot be used as an identifier; rename it (e.g. 'message_' or 'msg')`. Applies to `expect_token(TOKEN_IDENTIFIER)` failures (extern/function params, struct fields, etc.) and `parse_block`'s "unexpected statement head" fallback (local declarations).

The structural request — making `message` a contextual keyword — is NOT done; that's a larger change with regression risk across the codebase. The original claim of a "wildly misleading column number" didn't reproduce in testing: column pointed directly at `message` in the extern case and one char past it in the local-assignment case.

**Severity: minor** — docs issue, mostly.

**Reserved (from `compiler/parser/lexer.c`):** `actor, after, as, bool, break,
builder, callback, case, const, continue, default, defer, else, except,
export, extern, false, float, for, func, hide, if, import, in, int, let,
long, main, make, match, message, Message, module, null, print, ptr,
receive, reply, return, seal, self, send, spawn, spawn_actor, state, string,
struct, switch, true, var, when, while`.

`message` in particular is common domain vocabulary for anything that
deals with errors, logs, network protocols, or queues. When we declare

```aether
extern svnae_error_create(code: int, message: string, ...)
```

the parser previously emitted `Expected IDENTIFIER, got MESSAGE_KEYWORD` — now a named-keyword error.

**Remaining request:** narrow the reserved set (is `message` really needed as a
top-level keyword outside the actor/receive context? Could it be contextual?).

---

## 6. gcc warnings for generated C leak through without `-v`

**STATUS: ✅ PARTIAL FIX** on PR #192. Second request done: `[build] cflags = "-Wno-int-conversion"` in `aether.toml` now applies to both `ae build` AND `ae run` (it was previously gated behind `optimize`, so only release builds picked it up). Projects can now drop a `-Wno-int-conversion` into their toml and the warnings go away without touching the compiler defaults.

First request (make warning-suppression the default in aetherc itself) was **declined**: suppressing `-Wint-conversion` project-wide would mask real pointer-truncation bugs. The in-scope second fix is the better design — visible by default, opt-out per project — matching the standard gcc/clang convention.

**Severity: polish.**

When a program has no Aether errors but the generated C has warnings (e.g.
extern declarations vs. glibc headers), those warnings dump into the user's
terminal even without `-v`.

**Original requests:**
- Default to suppressing `-Wint-conversion` and similar "expected" mismatches
  that come from FFI, or at least gate them behind `-v`. (Declined.)
- Let `aether.toml` specify extra `cflags` that apply only to user C sources
  (so `-Wno-int-conversion` etc. can be opted-in per project). (Done — and
  cflags now apply to both `ae build` and `ae run`.)

---

## 7. `extra_sources` in `[[bin]]` does not apply when building via file path

**STATUS: 📝 DEFERRED — discussion at `claim_7_discussion.md` (repo root).** The reported single bullet is actually three separate sub-bugs, each needing a maintainer design call:

- **7a.** Absolute-path invocation doesn't match `[[bin]] path = "..."` (match logic strips `./` but not `/full/path`).
- **7b.** `ae build <bin_name>` — no name-to-path resolution exists; CLI treats the arg as a file path unconditionally.
- **7c.** `aether.toml` is looked up only in cwd — no walk-up like cargo/go/npm do.

The reporter's original "`ae build <file.ae>` does not pull in extra_sources" claim did NOT reproduce with a relative path from the project root — so they almost certainly hit one of 7a/7b/7c. Fix ordering suggested in the discussion doc: docs first, then 7a (smallest), then 7b (design choice on conflicts), then 7c (behavior change, riskiest).

**Severity: minor** — took a while to debug.

**Originally reported:**
`ae build ae/subr/test_error.ae` compiles the `.ae` and links the runtime,
but does NOT pull in `extra_sources` from the `[[bin]]` entry that points to
the same file. Only `ae run ae/subr/test_error.ae` (and probably `ae run`
with no args) picks up the per-bin `extra_sources`. `ae build <bin_name>`
also returned "File not found".

---

## 8. Two calls in one comparison hang at runtime

**STATUS: ✅ FIXED** on PR #192. The hang is **not extern-specific** — any two function calls on either side of `!=` / `==` inside an if/while/for/match condition reproduces it. Root cause: the parser's trailing-block rule (`func(args) { body }` → closure argument) ate the `{` that starts the if/while body as a trailing closure on the rightmost call in the condition. `return 0` got silently dropped and `i = i + 1` moved inside the if — infinite loop.

**Fix:** new `Parser::in_condition` flag raised while parsing if/while/for/match conditions; inside the flag, `func(args) { ... }` is parsed without the trailing block so the `{` stays with the statement parser. Explicit trailing-block forms (`callback { }`, `|params| { }`) still work since the preceding keyword/pipe disambiguates them. The reporter's "double-evaluating the extern" hypothesis was incorrect.

**Severity: blocker** — silently broken, no compile error.

**Repro:**

```aether
import std.string

f(a: string, b: string) {
    i = 0
    while i < 3 {
        if string.char_at(a, i) != string.char_at(b, i) { return 0 }
        i = i + 1
    }
    return 1
}

main() {
    println("before")
    r = f("abc", "abc")
    println("r=${r}")
}
```

**Originally observed:** program hangs indefinitely; even `"before"` never reached the terminal (buffered stdout).

**Workaround (no longer needed):** hoist each call into a variable first.

**Impact on port (as filed):** pervasive. Any byte-by-byte comparison (path walking,
checksum compare, diff) naturally wants `s1[i] != s2[i]`.

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

## 10. Block-local `{ }` scoping leaks into enclosing scope

**STATUS: ✅ FIXED** on PR #194 (2026-04-21). The if/else path in `codegen_stmt.c` already save/restored `declared_var_count` across branches; the bare AST_BLOCK path didn't. Two sibling blocks sharing a local name now each emit their own declaration. Root cause was a pure codegen scope-tracking gap (not the wrong-error-message variant the original report suggested): previously the second block emitted `src = "b";` with no declaration because `gen->declared_var_count` tracked the first block's `src` as still-declared even after its C scope closed; gcc errored with `'src' undeclared`.

**Severity: minor** — surprising, worth documenting.

```aether
main() {
    { src = "a"; ... }   // scope 1
    { src = "b"; ... }   // scope 2 — error: `src` redeclared
}
```

Each brace-block's locals live in the enclosing function's namespace.
Two sibling blocks that each bind the same local name get a
"redeclaration" error from the C compiler.

**Workaround:** extract each block into its own function.

**Request:** either give `{ }` lexical scope, or error at the Aether
level with a clearer message than a gcc "undeclared identifier" in
generated code (which points into a temp file users never see).

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

## 13. Return-type inference breaks for recursive functions returning string

**STATUS: ❌ DOES NOT REPRODUCE** on v0.76.0. Tested both with and without an explicit `-> string` annotation. The recursive call's return value is correctly typed `const char*` in the generated C at every call site; no int/pointer confusion. Almost certainly resolved by PR #190 (`-> ReturnType` parsing fix — claim #1). If the reporter still hits this, a minimal repro against v0.76.0 is needed — the original shape doesn't bite any more.

**Severity: blocker for any recursive algorithm that builds string output.**

A recursive user function whose return type is string (via `write_blob_text`
and similar) fails to type-check. Aether picks `int` for the not-yet-seen
recursive call, then the generated C tries to pass the int through a
`const char*` parameter. End result: compile warnings everywhere, runtime
segfault.

**Repro:**

```aether
rebuild(path: string) {
    if some_cond() {
        return rebuild(child_path)   // Aether picks int here
    }
    return "leaf"
}

main() {
    r = rebuild("/")
    println(r)
}
```

**Workaround in port:** moved the recursive `rebuild_dir` into C
(`ae/fs_fs/txn_shim.c`) and exposed a single non-recursive Aether entry point.

**Request:** forward-declare recursive functions' return types from their
leaf (base-case) returns, or infer in a fixpoint pass.

---

## 14. Return-type inference poisons subsequent call sites of the same extern

**STATUS: ❌ DOES NOT REPRODUCE** on v0.76.0. The repro I ran — two `svnae_count(repo)` calls with a recursive `string`-returning user function between them — both return the correct int (11), and the generated C declares `int a = svnae_count(...);` and `int b = svnae_count(...);` at both call sites. No drift, no poisoning. Likely resolved by PR #190. Worth flagging to the reporter: the "wrap in `0 + call()`" workaround is no longer needed.

**Severity: blocker.**

```aether
extern svnae_count(repo: string) -> int

main() {
    a = svnae_count(repo)    // prints 11 — int
    call_something_else()    // returns string
    b = svnae_count(repo)    // ??? prints empty/0; the type at THIS call
                             //     got coerced to something that drops
                             //     the int return
}
```

The first call works. Something inside `call_something_else` (in our case
`commit_txn` which routes through a recursive `string`-returning extern)
makes the second call to `svnae_count` produce a wrong value at the
interpolation site.

**Workaround:** wrap the call in integer arithmetic: `b = 0 + svnae_count(repo)`.
That forces Aether to generate the int-valued temporary and the int value
flows correctly.

**Request:** extern signatures are declared once — they should always carry
their declared types at every call site, regardless of what other functions
have been inferred in between.

---

## 15. `!` unary-negation only works on already-boolean values

**STATUS: ❌ DOES NOT REPRODUCE** on v0.76.0. `if !some_cond()` where `some_cond()` returns 0 correctly takes the true branch. Either fixed upstream or the original repro was more fragile than the minimal form. If a specific shape still fails, a targeted repro would help.

**Severity: minor.**

```aether
if !some_fn_returning_int() { ... }
```

fails with `If condition must be boolean`. `!` doesn't implicitly coerce
`int != 0` to `bool`. Have to write `if some_fn_returning_int() == 0 { ... }`.

**Request:** either allow `!` on integer types (treat 0 as false, non-0 as
true), or document this limitation prominently. Most C/Go/Rust programmers
will reach for `!` first.

---

## 16. `extern` declarations with 6 or more params drop the last param

**STATUS: ❌ DOES NOT REPRODUCE** on v0.76.0. Tested the reporter's exact 6-param signature: `extern my_fn(a: ptr, b: string, c: int, d: int, e: string, f: int) -> int`. Generated C declaration has all six params, calls with six args compile and run correctly (probe returned the 6th arg value, 99). No arity drop.

**Severity: blocker for wide APIs.**

**Repro:**

```aether
extern my_fn(a: ptr, b: string, c: int, d: int, e: string, f: int) -> int
```

**Observed:** the generated C forward-declaration has only 5 params:
`int my_fn(void*, const char*, int, int, const char*);`. The 6th
parameter is silently dropped at Aether's extern parsing stage. Calls
with 6 arguments then fail with "too many arguments" from gcc.

**Workaround used in port:** pack two ints into one (e.g., `state << 4 |
kind`) or split into two C functions.

**Request:** fix extern param parsing to accept any arity. If there's a
hard limit in the runtime, raise it or document it; the current
behaviour is silent data loss.

---

## Notes for Nicolas

> **Editor's note (2026-04-21):** the claims below about a "type-system cluster"
> are from the original report against v0.70.0. Re-triaging against v0.76.0
> finds that #1 (the parser bug for `-> ReturnType`) was the root, and #13
> / #14 / #16 no longer reproduce once it's fixed. The cluster is much
> smaller than described. See postscript at the bottom for the current
> picture.

After ~11,500 lines of Aether + ~4,000 lines of C shims, 30 test suites,
and a working svn client/server pair, the picture of which bugs hurt most:

**The type-system cluster is the whole problem.** Issues #1, #2, #13, #14,
#16 are all the same root cause: Aether's type inference for user-defined
functions, recursive calls, and extern declarations with explicit return
types doesn't hold the types correctly through all phases of the compiler.
Each manifests differently, but the effect is the same — any non-trivial
Aether code that involves pointer handoffs across functions or modules ends
up miscompiled, with the generated C casting pointers to ints and then
back, which usually segfaults at runtime rather than failing to compile.

The concrete cost: **we've written all the heavy logic in C** (~4,000 lines
of shim code) and used Aether as a CLI / orchestration layer. That's a
survivable shape for this port, but it's not what Aether is marketed as.
A language that claims to be a systems language needs its typed cross-module
boundary to carry pointers correctly. Fixing the type cluster would let us
move most of the shim code to Aether in a follow-up pass.

**Second tier — #4 (no `__FILE__` / `__LINE__` intrinsics).** Pervasive
cost because we want source locations in every error-chain link. Today
we pass them as string + int args at every call site, which bit-rots on
every edit.

**Ergonomic but mattered in practice:**

- #5 (`message` keyword): the `MESSAGE_KEYWORD` parse error pointing at
  the wrong column cost a non-trivial debug cycle on first encounter.
- #8 (double extern call in one expression hangs): silent infinite loop.
  Easy to work around once you know, but it's a compiler bug that should
  just not happen.
- #9 (`extra_sources` cache): you change a `.c` file, build output doesn't
  update, you chase a bug that's already fixed. Cache-key by content hash
  of all listed sources, not just the `.ae`.
- #15 (`!` on non-bool): every other C-family language does coerce. This
  one catches newcomers immediately.

**Fine as-is, just needs docs:**

#3, #6, #7, #10, #11, #12 — all documented workarounds in the port, and
none of them cost more than a few minutes once you've hit them the first
time. Worth a "known surprises" page on aetherlang.org.

**Not shown here but worth mentioning:** Aether's ergonomic wins are
real. The `${expr}` string interpolation is lovely. Actor syntax is clean.
The Go-style `(value, err)` tuple returns read well. The toml-driven
project layout with `ae run` / `ae build` is simple enough that new
contributors understood it immediately. When the type system cooperates
(simple scalar-return functions over ints and strings) the code is
pleasant to write. The language has a voice. It's just not yet robust
enough at the pointer boundary to carry 300KLoC of systems code by itself.

— The svn-aether port team, after 34 commits.

---

## Postscript — triage against v0.76.0 (2026-04-21)

Second pass. When I re-ran every remaining claim on v0.76.0, most of the "type-system cluster" evaporated:

- **#1 FIXED** (PR #190). The `-> ReturnType` parser bug.
- **#2, #3 DO NOT REPRODUCE** — the pointer-erasure stories were already OK on v0.74.0.
- **#5, #6 (partial), #8 FIXED** (PR #192). Reserved-keyword error messages, `ae run` cflags, and the trailing-block-in-condition hang.
- **#10 FIXED** (PR #194). Sibling bare-block scope leak.
- **#13, #14, #15, #16 DO NOT REPRODUCE on v0.76.0.** All four were plausibly downstream effects of #1's parser bug; with #1 gone, none of them bite. Worth flagging this to the reporter: their port workarounds (inline recursion in C, `0 + call()` int-coercion trick, six-param arity splits) can probably be unwound.

What's left:

- **#4 (`__FILE__` / `__LINE__`)** — confirmed open, scoped, highest remaining ergonomic win.
- **#7 (extra_sources path resolution)** — three sub-bugs, design notes in `claim_7_discussion.md`.
- **#9 (build cache on `.c` edits)** — confirmed open from the description (cache key omits `extra_sources`).
- **#11 (`string.concat("", p)` strlen-based)** — design/API, missing `string.from_bytes(p, len)`.
- **#12 (multi-line `extra_sources` array)** — known limitation in `tools/ae.c`.

Of these, #4 is the most valuable single fix, and #9 + #12 are both small. #11 needs an API design call. #7 needs a maintainer design call.

The "type-system cluster" framing in the original report doesn't hold any more. The remaining issues are ergonomic / build-system issues, not correctness bugs. Which is a very different story to take back to the Subversion-port team.

— annotations added while triaging against main at v0.76.0.
