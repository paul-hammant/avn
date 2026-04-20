# Aether v0.70.0 issues found during the Subversion port

Running list of bugs, limitations, and rough edges hit while porting Subversion
(`~/scm/subversion/subversion/`) to Aether. Each entry has: repro, observed,
expected, severity, workaround. Hand this to Nicolas whenever convenient.

Aether build in use: `/home/paul/scm/aether/build/ae` — reports `Aether 0.70.0`.
Platform: Linux x86_64, gcc as backend, dev mode.

---

## 1. `-> ReturnType` on user functions miscounts arguments at call sites

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

**Workaround:** omit the `-> ReturnType` annotation. Aether infers it:

```aether
g(a: int, b: int) { return a + b }   // works, returns int
```

**Impact on port:** every user function we write goes without its return
annotation. Reduces self-documentation; makes API boundaries ambiguous.

---

## 2. Cross-module `import` silently erases `ptr` parameter types to `int`

**Severity: blocker** — crashes at runtime with truncated pointers on 64-bit.

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
    r = mymod.take(s)         // crashes or wrong answer
    println(r)
}
```

**Observed:** generated C declares `static int mymod_take(int p)` — the `ptr`
type is dropped, the argument is received as a truncated `int`. On 64-bit
systems the pointer's high bits are lost, reads segfault.

**Expected:** the imported function signature carries its declared types.

**Related:** even untyped exported params (`export take(p) { ... }`) get
defaulted to `int`, not inferred from call sites. Same truncation.

**Workaround used in port:** define shared logic in a C shim (reached via
`extern`) rather than an Aether `export` function. Or inline the logic in each
caller's file. Both hurt code reuse.

**Impact on port:** our 10-library architecture (libsvn_subr, delta, fs_fs,
etc.) assumes modules can pass `ptr`s (error chains, file handles, opaque
tree nodes) to each other. Without this working, every module boundary has
to be a C shim — we effectively write C with Aether syntax on top. Significant
architectural degradation.

---

## 3. `ptr` literal `0` gets generated as `int` in cross-module call sites

Related to #2 but shows up even when callee types are correct. Passing a
literal `0` where a `ptr` is expected generates C code with a bare `0` that
gcc treats as `int` and warns on `-Wint-conversion`. May or may not crash
depending on platform (on x86_64 it sometimes works because `0` zero-extends).

**Request:** treat `0` in a `ptr` context as `(void*)0`.

---

## 4. No `__FILE__` / `__LINE__` intrinsics

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

---

## 5. Reserved-keyword collisions with common identifier names

**Severity: minor** — docs issue, mostly.

Reserved (from `compiler/parser/lexer.c`): `actor, after, as, bool, break,
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

the parser emits a `MESSAGE_KEYWORD` error with a wildly misleading column
number (points to a completely different line). Took a while to track down.

**Requests:**
- Consider narrowing the reserved set (is `message` really needed as a top-level
  keyword outside the actor/receive context? Could it be contextual?).
- Improve the error message. `Expected IDENTIFIER, got MESSAGE_KEYWORD`
  pointing at the wrong column is not helpful — list the offending name
  and suggest renaming.

---

## 6. `-v` / verbose mode: gcc warnings for generated C leak through

**Severity: polish.**

When a program has no Aether errors but the generated C has warnings (our
extern declarations vs. glibc headers, for example), those warnings dump
into the user's terminal even without `-v`. For our `error_shim.c` path we
saw `-Wint-conversion` warnings every compile, cluttering the build output.

**Requests:**
- Default to suppressing `-Wint-conversion` and similar "expected" mismatches
  that come from FFI, or at least gate them behind `-v`.
- Consider letting `aether.toml` specify extra `cflags` that apply only to
  user C sources (so `-Wno-int-conversion` etc. can be opted-in per project).

---

## 7. `extra_sources` in `[[bin]]` does not apply when building via file path

**Severity: minor** — took a while to debug.

`ae build ae/subr/test_error.ae` compiles the `.ae` and links the runtime,
but does NOT pull in `extra_sources` from the `[[bin]]` entry that points to
the same file. Only `ae run ae/subr/test_error.ae` (and probably `ae run`
with no args) picks up the per-bin `extra_sources`. `ae build <bin_name>`
also returned "File not found" — the command didn't resolve the bin name.

**Requests:**
- Document which invocations honour `[[bin]]` entries.
- Make `ae build <bin_name>` (bin-name lookup) work.
- Make `ae build <file.ae>` cross-reference `[[bin]]` entries by path so
  `extra_sources` still applies.

---

## 8. Two calls to the same extern in one comparison hang at runtime

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

**Observed:** program hangs indefinitely (we SIGTERM it). Stdout is buffered,
so even "before" never reaches the terminal.

**Workaround:** hoist each call into a variable first:

```aether
while i < 3 {
    x = string.char_at(a, i)
    y = string.char_at(b, i)
    if x != y { return 0 }
    i = i + 1
}
```

With the hoist, the same program prints `before` / `r=1` as expected.

**Possible cause:** looks like the generated C for the compound condition
does something wrong with double-evaluating the extern call, possibly
re-entering the runtime or looping on the scheduler. Needs a codegen look.

**Impact on port:** pervasive. Any byte-by-byte comparison (path walking,
checksum compare, diff) naturally wants `s1[i] != s2[i]`. We now have to
hoist every one of those.

---

## 9. Build cache doesn't invalidate on `extra_sources` C file changes

**Severity: minor, but bites debugging badly.**

Editing a C file listed in `[[bin]].extra_sources` does NOT cause `ae run`
to rebuild. The test keeps running against the stale shim. `ae cache clear`
fixes it, but it's easy to chase the wrong bug for 20 minutes first.

**Request:** hash the `extra_sources` contents (not just the `.ae` file)
when computing the cache key.

---

## 10. Block-local `{ }` scoping leaks into enclosing scope

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

**Severity: documentation / missing API.**

The only obvious idiom for "copy this raw C `char*` into an Aether-managed
string" is `string.concat("", s)`. But `string_concat` uses `strlen` on
plain-ptr inputs (via `str_len`), so copied strings can't carry embedded
NULs, and any trailing bytes past the caller's intended length leak in
until the next NUL.

Specifically, if the source buffer at `s` contains N real bytes followed
by a NUL, but the source's internal length field says N-k or N+k, strlen
reads whatever's in memory until the first NUL — could be too few, could
be too many. We caught this in the FSFS port when `buf_new` in
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
NUL-terminate when crossing a boundary (every buf_new/buf_from in our
shims now does `data[n] = '\0'`).

---

## 12. `extra_sources` array must be single-line in aether.toml

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

**Workaround in port:** we moved the recursive `rebuild_dir` into C
(ae/fs_fs/txn_shim.c) and expose a single non-recursive Aether entry point.
`string.concat("", recursive_result)` is a partial workaround at individual
call sites but doesn't actually fix the wrong C-level type.

**Request:** forward-declare recursive functions' return types from their
leaf (base-case) returns, or infer in a fixpoint pass.

---

## 14. Return-type inference poisons subsequent call sites of the same extern

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

**Severity: minor.**

---

## 16. `extern` declarations with 6 or more params drop the last param

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


```aether
if !some_fn_returning_int() { ... }
```

fails with `If condition must be boolean`. `!` doesn't implicitly coerce
`int != 0` to `bool`. Have to write `if some_fn_returning_int() == 0 { ... }`.

**Request:** either allow `!` on integer types (treat 0 as false, non-0 as
true), or document this limitation prominently. Most C/Go/Rust programmers
will reach for `!` first.

---

## Notes for Nicolas

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
