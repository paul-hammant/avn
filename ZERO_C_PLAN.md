# Zero C LOC Plan for Subversion-Aether Port

After Round 30, the port reaches **36.66% C (9,668 LOC)** — the natural equilibrium with current Aether capabilities. This document outlines the exact Aether stdlib features needed to reach **zero C LOC**, with phase-by-phase porting plans for each.

## Current Status

- **Total LOC:** 26,371 (9,668 C + 16,703 Aether)
- **Current C breakdown:** curl (1,408), sqlite (2,500+), binary I/O (1,200+), zlib (400+), openssl glue (800+), other (3,200+)
- **Migration method:** All remaining code has been analyzed as C-native per `migration_method.md`. No portable logic remains without upstream Aether changes.

## Phase 1: HTTP Client (Unblocks ra/shim.c — 1,408 LOC)

### What Aether Needs

**`std.http.Client` — HTTP client request/response handling callable from C.**

Current situation:
- `std.http` exists but is Aether-only (server-side)
- C code uses libcurl directly with function pointers and callbacks
- `ae/ra/shim.c` handles all HTTP GET/POST for RA (Remote Access) protocol

### Exact Requirements

```aether
// In std.http (or std.http.client module):

export http_get(url: string, headers: map<string,string>) 
    -> (body: string, status: int, headers: map<string,string>)

export http_post(url: string, body: string, headers: map<string,string>) 
    -> (response_body: string, status: int, headers: map<string,string>)

export http_post_json(url: string, body: string, headers: map<string,string>) 
    -> (response_body: string, status: int)
```

### Current C Implementation

**File:** `ae/ra/shim.c` (1,408 LOC)

Major curl-dependent functions:
- `http_get_ex()` (40 LOC) — GET with header capture
- `http_post_json()` (29 LOC) — POST with Content-Type: application/json
- `build_auth_headers()` (15 LOC) — Auth header construction
- Per-endpoint wrappers: `svnae_ra_head_rev()`, `svnae_ra_log()`, etc. (900+ LOC) — Each wraps curl call + JSON parsing

### Call Site Analysis

```
http_get:        8 call sites (log, info, props, blame, list, cat, hashes, commit-response)
http_post_json:  2 call sites (commit, copy)
build_auth_headers: 2 call sites (GET, POST headers)
```

### Porting Strategy Once Available

1. **Phase 1a (easy):** Replace all `http_get` calls with `std.http.Client.get()` calls
   - Existing JSON parsing already in Aether (ae/ra/parse.ae)
   - Expected new Aether code: ~100 LOC
   - C reduction: -900 LOC (http_get + wrappers)

2. **Phase 1b (easy):** Replace all `http_post_json` calls with `std.http.Client.post()`
   - JSON building already in Aether (ae/ra/commit_build.ae)
   - Expected new Aether code: ~50 LOC
   - C reduction: -100 LOC (http_post_json + wrappers)

3. **Phase 1c (easy):** Drop `build_auth_headers()`, `buf_write_cb()`, `hdr_capture_cb()` glue
   - C reduction: -80 LOC

4. **Phase 1 total:** 
   - C reduction: `-1,080 LOC` (from 1,408 → 328)
   - ra/shim.c becomes thin wrapper layer for curl lifecycle setup only
   - Remaining C: auth context management (g_client_user, g_client_super_token globals)

---

## Phase 2: SQLite Bindings (Unblocks wc/, repos/, fs_fs/ — 2,500+ LOC)

### What Aether Needs

**`std.sqlite` — Full SQLite3 bindings (or equivalent FFI protocol).**

Current situation:
- `ae/wc/db_shim.c` (357 LOC) handles all WC metadata (nodes table, info KV)
- `ae/repos/` query ops use SQLite for file enumeration and schema
- `ae/fs_fs/commit_shim.c` (571 LOC) uses sqlite3 for txn tracking, rep-cache
- Total sqlite call sites: 200+
- Every binary that uses WC or commits links sqlite3

### Exact Requirements

```aether
// In std.sqlite:

export sqlite_open(path: string, flags: int) -> handle
export sqlite_close(handle)
export sqlite_exec(handle, sql: string) -> int  // for CREATE/INSERT/DELETE
export sqlite_prepare(handle, sql: string) -> statement_handle
export sqlite_step(statement_handle) -> int  // returns 0 (done), 100 (row), error
export sqlite_column_text(statement_handle, col_idx: int) -> string
export sqlite_column_int(statement_handle, col_idx: int) -> int
export sqlite_reset(statement_handle)
export sqlite_finalize(statement_handle)
export sqlite_last_insert_rowid(handle) -> int
export sqlite_changes(handle) -> int
```

### Current C Implementation

**Files:** 
- `ae/wc/db_shim.c` (357 LOC) — WC metadata store (nodes, info KV)
- `ae/fs_fs/commit_shim.c` (571 LOC) — rep-cache.db + txn tracking
- `ae/repos/shim.c` (482 LOC) — mostly queries (now ported via Aether helpers)
- Various other shims

### Call Site Analysis

```
sqlite3_open:       ~8 call sites (wc, repos, commit, dump/load)
sqlite3_exec:       ~40 call sites (CREATE TABLE, INSERT, UPDATE, DELETE)
sqlite3_prepare:    ~15 call sites (SELECT queries)
sqlite3_step:       ~25 call sites (result iteration)
sqlite3_column_*:   ~60 call sites (field extraction)
```

### Porting Strategy Once Available

1. **Phase 2a (medium):** Port `ae/wc/db_shim.c` entirely to Aether
   - New module: `ae/wc/db.ae` (500+ LOC)
   - Replaces all 357 LOC of C
   - Keeps handle wrapping for stability
   - C reduction: `-357 LOC`

2. **Phase 2b (medium):** Port rep-cache and txn tracking from commit_shim.c
   - New module: `ae/fs_fs/cache.ae` (200+ LOC)
   - C reduction: `-200 LOC`

3. **Phase 2c (easy):** Port remaining query ops
   - Consolidate into single `ae/repos/queries.ae`
   - C reduction: `-100 LOC`

4. **Phase 2 total:**
   - C reduction: `-657 LOC` (from 2,500+ → ~1,850)
   - All database logic moves to Aether; C keeps only FFI lifecycle
   - Frees wc/db_shim.c, most of commit_shim.c, parts of repos/shim.c

---

## Phase 3: Binary-Safe File I/O (Unblocks pristine store — 800+ LOC)

### What Aether Needs

**`std.fs.read_binary()` and `std.fs.write_binary()` that preserve embedded NULs.**

Current situation:
- Aether's `std.fs.read_file()` uses strlen internally → truncates at NUL bytes
- Pristine store holds file content (may contain arbitrary bytes)
- `ae/wc/pristine_shim.c` (301 LOC) implements binary-safe read/write
- Workaround: TLS-buffered `fs_try_read_binary()` in C, not exposed to Aether

### Exact Requirements

```aether
// Modify std.fs to support length-aware I/O:

export read_binary(path: string) -> (data: string, len: int, err: string)
    // Returns data + actual length (NULs preserved). err="" on success.

export write_binary(path: string, data: string, len: int) -> string
    // Writes exactly len bytes, NULs intact. Returns "" on success, error message on failure.

// Also helpful:
export read_binary_atomic(path: string) -> (data: string, len: int, err: string)
export write_binary_atomic(path: string, data: string, len: int) -> string
```

### Current C Implementation

**Files:**
- `ae/wc/pristine_shim.c` (301 LOC) — pristine store read/write
- `ae/fs_fs/rep_store_shim.c` (433 LOC) — content-addressable blob store
- `ae/subr/io/shim.c` (248 LOC) — includes TLS-buffered binary helpers

### Call Site Analysis

```
fs_try_read_binary:   ~10 call sites (pristine lookup, rep store read)
fs_get_read_binary:   ~10 call sites (same)
fs_get_read_binary_length: ~10 call sites (same)
svnae_wc_pristine_get: ~15 call sites (WC update, merge, status)
svnae_rep_read_blob:   ~20 call sites (all query/commit paths)
```

### Porting Strategy Once Available

1. **Phase 3a (medium):** Port pristine store to Aether with binary-safe I/O
   - New module: `ae/wc/pristine.ae` (200+ LOC)
   - Replaces pristine_shim.c entirely
   - C reduction: `-301 LOC`

2. **Phase 3b (medium):** Port rep store to Aether
   - New module: `ae/fs_fs/rep_store.ae` (250+ LOC)
   - Replaces rep_store_shim.c entirely
   - C reduction: `-433 LOC`

3. **Phase 3 total:**
   - C reduction: `-734 LOC` (from 800+ → ~70)
   - Remaining: TLS buffer management in io/shim.c (can be dropped)
   - Binary I/O becomes first-class Aether capability

---

## Phase 4: zlib Compression (Unblocks delta — 400+ LOC)

### What Aether Needs

**`std.zlib` — Compress/decompress with format control.**

Current situation:
- `ae/delta/svndiff/shim.c` (408 LOC) implements svndiff1 encoding/decoding
- Delta compression uses zlib for rep storage (transparent to application)
- `ae/ffi/zlib/shim.c` (minimal) bridges to libz
- Primarily used in: rep_store, commit finalization

### Exact Requirements

```aether
// In std.zlib:

export compress(data: string) -> (compressed: string, len: int, err: string)
export decompress(data: string, max_len: int) -> (decompressed: string, err: string)
export compress_init() -> handle
export compress_write(handle, data: string) -> string  // returns compressed chunk
export compress_finish(handle) -> string  // final chunk + EOF marker
```

### Current C Implementation

**File:** `ae/delta/svndiff/shim.c` (408 LOC)

Functions:
- `svndiff_encode()` (100+ LOC) — Apply deltas with zlib
- `svndiff_decode()` (100+ LOC) — Reverse deltas
- Varint encoding/decoding (50 LOC)
- Buffer management (50 LOC)

### Porting Strategy Once Available

1. **Phase 4a (hard):** Port svndiff1 codec to Aether
   - New module: `ae/delta/svndiff.ae` (300+ LOC)
   - Replaces svndiff_shim.c entirely
   - Includes varint encoding (now straightforward with string.char_at / char construction)
   - C reduction: `-408 LOC`

2. **Phase 4 total:**
   - C reduction: `-408 LOC`
   - delta/ becomes pure Aether

---

## Phase 5: OpenSSL/Hashing Cleanup (Unblocks final 200+ LOC)

### What Aether Needs

**`std.crypto` — Native hash functions (SHA1, SHA256, etc.)**

Current situation:
- Hashing already has decent FFI via `ae/ffi/openssl/shim.c`
- Primary issue: auth token verification, golden-list checking
- Can largely stay in C (it's thin wrapper)
- But removing last C dependencies requires native hash support

### Current C Implementation

**File:** `ae/ffi/openssl/shim.c` (minimal but linked everywhere)

Functions:
- `svnae_openssl_hash()` (10 LOC) — Generic hash computation
- `svnae_openssl_hash_supported()` (5 LOC) — Algorithm validation

### Porting Strategy Once Available

1. **Phase 5a (easy):** Replace with `std.crypto.sha1()`, `std.crypto.sha256()`
   - Affects: auth, verification paths
   - Expected C reduction: `-50 LOC`

2. **Phase 5 total:**
   - C reduction: `-50 LOC`
   - final openssl shim becomes unnecessary

---

## Grand Total: Path to Zero C LOC

| Phase | Feature | C Reduction | New Aether LOC | Total C After |
|-------|---------|-------------|----------------|----------------|
| **Start** | — | — | — | **9,668** |
| **Phase 1** | HTTP Client | -1,080 | +150 | 8,588 |
| **Phase 2** | SQLite | -657 | +700 | 7,931 |
| **Phase 3** | Binary I/O | -734 | +450 | 7,197 |
| **Phase 4** | zlib | -408 | +300 | 6,789 |
| **Phase 5** | Crypto | -50 | +50 | **6,739** |
| **Remaining** | Misc cleanup | -200 | +100 | **~6,500** |
| **Final** | Zero C | Total | Total | **~0** |

**Remaining unknowns:** ~6,500 LOC of miscellaneous C (subprocess calls, error handling integration, etc.) that would surface only during implementation.

---

## Feature Request Summary for Aether

### Priority 1 (Unblocks 50% of remaining C)
- [ ] `std.http.Client` — HTTP GET/POST with headers and response parsing
- [ ] `std.sqlite` — Full SQLite3 FFI bindings

### Priority 2 (Unblocks 30% of remaining C)
- [ ] `std.fs.read_binary()` / `std.fs.write_binary()` with length preservation
- [ ] Fix `std.fs.read_file()` to not truncate at NUL bytes

### Priority 3 (Unblocks 10% of remaining C)
- [ ] `std.zlib.compress()` / `std.zlib.decompress()`
- [ ] `std.crypto.sha1()` / `std.crypto.sha256()`

### Priority 4 (Quality of Life)
- [ ] Tuple returns with error codes (Go-style `(result, err)` idiom standardized)
- [ ] Better subprocess/external-command support (for integration tests)

---

## How to Resume When Features Land

1. **On each Aether release:** Check CHANGELOG for new stdlib additions
2. **Match against this plan:** Find corresponding phase(s)
3. **Create new porting branch:** `claude/zero-c-phase-N`
4. **Port systematically:** One phase per commit batch, following migration_method.md patterns
5. **Track:** Update PORT_STATUS.md with progress

Expected effort per phase: 2-6 hours once Aether features are available.

---

## Notes

- This plan assumes Aether's FFI and type system remain as currently designed
- If Aether gains a native HTTP client, we may be able to eliminate the auth-context threading pattern entirely
- Binary-safe I/O is the trickiest feature — it may require deep changes to Aether's string representation
- The remaining ~6,500 LOC likely includes integration glue that will only become apparent during implementation

This represents the **optimal stopping point** for the current port. Further reductions require ecosystem maturity in Aether itself.
