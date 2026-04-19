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

## 8. `!` unary-negation only works on already-boolean values

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

## Notes for Nicolas

The Aether-Subversion port is feasible and we're pushing through the above.
Of the list, **#2 (cross-module `ptr` type erasure) is the only one that
threatens the architecture** — without it we can't have clean inter-library
APIs and have to fall back to C-shim-per-boundary. If that one gets fixed,
everything else is ergonomics.

`__FILE__`/`__LINE__` (#4) is the next most valuable because error-handling
is pervasive.

Everything else is workaroundable in place.
