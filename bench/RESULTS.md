# avn bench: compress vs no-compress, 5000-commit run

**Headline:** server-side zlib at default level costs ~3 ms per
commit (+4%) and saves 24% on disk *on intentionally-incompressible
random text*. On real-world content the disk savings would be larger,
the CPU cost wouldn't move much. **Keep compression on.**

## Run

- **Date:** 2026-05-10 (post-migration re-run; original 2026-05-09)
- **Tool:** `bench/bench_avn.py` (with and without `--no-compress`)
- **Workload:** 5000 commits × 100 KB of random alphanumeric text per
  commit, all into one growing directory (`f/<i>.txt`), one client,
  one thread, `--parent-sha` chain CAS so each commit is one
  HTTP round-trip
- **avn build:** post-Round 309 (commit `e4a2e0c` + Phase-7 cleanup
  `f9a58f5`) under aetherc 0.143.0 *plus* the unreleased local fix
  for the struct-field-string-escape bug filed at
  `aether/struct_field_string_escape.md`. Pre-fix the bench couldn't
  complete a single commit (`{"edits":[{}]}` POST body); post-fix
  the run is healthy.
- **Box:** Linux x86_64, 16 GB RAM, single SSD
- **Server:** loopback HTTP, AVN_NO_COMPRESS env switches between
  modes (R285's bench escape hatch)

## Results

| metric                              | `--no-compress` |     default (zlib) |     delta |
|-------------------------------------|----------------:|-------------------:|----------:|
| commits completed                   |       5000/5000 |          5000/5000 |         — |
| wall time                           |           405 s |              422 s |     +4.2% |
| mean throughput                     | 12.3 commits/s  |     11.8 commits/s |     −4.1% |
| per-commit (mean, batches 3–100)    |     **80.5 ms** |        **83.7 ms** | **+4.0%** |
| repo size on disk                   |          494 MB |             375 MB |  **−24%** |
| final avnserver RSS                 |          3.36 GB |             3.70 GB |      +10% |
| per-commit retention (RSS / count)  |       0.67 MB/c |          0.74 MB/c |    +0.07× |

## Migration + leak-fix delta (R287 → R310)

| metric             | R287 nocomp | R309 nocomp | R310 nocomp | R287 comp | R309 comp | R310 comp |
|--------------------|------------:|------------:|------------:|----------:|----------:|----------:|
| wall time          |       385 s |       405 s |       389 s |     393 s |     422 s |     414 s |
| per-commit         |     77.1 ms |     80.5 ms |     77.8 ms |   78.6 ms |   83.7 ms |   82.8 ms |
| repo size on disk  |      493 MB |      494 MB |      494 MB |    375 MB |    375 MB |    375 MB |
| final avnserver RSS|     3.30 GB |     3.36 GB | **2.76 GB** |   3.60 GB |   3.70 GB | **2.73 GB**|
| MB-per-commit      |        0.66 |        0.67 |    **0.57** |      0.72 |      0.74 |   **0.56** |

**R287 → R309**: migration alone is performance-neutral within ~5%
(throughput drift; disk identical; RSS within noise).

**R309 → R310**: explicit `string_free` calls on three tuple-
destructured / non-tracked AetherStrings in the commit-handling
hot path (`avnserver/module.ae`'s `txn_add_b64_` → `raw, err`,
`commit_from_body` → `body, b64, berr`). Workaround for the
[`further-bug-fix4.md`](../../aether/further-bug-fix4.md) tuple-
destructure heap-tracker gap that survives 0.143.0.

- **−18% RSS no-compress** (3.36 → 2.76 GB), **−26% RSS compress**
  (3.70 → 2.73 GB). Per-commit retention drops to ~0.57 MB on
  both modes (was diverging — compress used to retain more because
  zlib allocations stacked on top of the leaked payload buffers).
- **+4% wall-time win** on the no-compress side (less malloc
  pressure, fewer page faults late in run); compress essentially
  flat (zlib already dominates the steady-state cost).
- The remaining ~0.57 MB/commit retention is from allocations
  outside this hot path (json AST nodes that `json.free` doesn't
  fully reclaim, intermediate strings in handlers other than
  commit, txn-internal buffers, etc.). A continuing ratchet.

Total trip from migration start: comparable wall time, **17–25%
RSS reduction**, identical on-disk format. The migration shipped
with a perf bonus.

Per-commit time is constant across all batches in both runs (no
algorithmic growth — verified after the heap-tracker codegen fix
trail in `aether/bug_repo.md` → `further-bug-fix3.md`).

## What the numbers actually say

**Compression cost is small in this codepath.** 1.55 ms per commit on
a 100 KB payload with single-threaded zlib at default level. Most of
the 77 ms steady-state per-commit time is JSON parse, base64 decode,
sha-1 of the content, dir-blob rebuild, and a sqlite path_rev insert
— compression is a single-digit-percent slice.

**Compression savings are bigger than you'd expect on random text.**
The bench's input is `random.choices(letters + digits + " \n", k=N)`
— a 65-character alphabet, ~6 bits of entropy per byte. Default zlib
captures roughly two of those wasted bits, hence ~24% on disk. On
real-world source code or prose (entropy 4–5 bits/byte) zlib will
typically take 50–70%.

**RSS is bounded and linear.** Both runs grow linearly at
~0.66–0.72 MB per commit — a residual non-architectural leak
(predominantly the b64-decoded `raw` bytes that don't pass through
the heap-tracker because they come out of a tuple destructure; see
`aether/further-bug-fix4.md`). For comparison, the same bench on
aetherc 0.135.0 (pre the architectural codegen fix) hit 14 GB RSS at
commit ~245 and hung the box. The 5000-commit run is now safe to do
unattended.

## Methodology notes

- The 100 KB random alphanumeric content is the worst case for
  compression — anything more structured will compress more.
  Treat the 24% number as a floor, not a typical figure.
- One client, one thread, no contention. Real workloads with multiple
  concurrent clients would amortise the per-commit fixed costs (sha,
  dir-blob rebuild, sqlite insert) better than the per-commit
  variable cost (compression scales with content size, the others
  scale with directory width).
- The bench writes one growing wide directory (`f/`). Per-commit
  rebuild_dir cost scales with that directory's size — that was the
  reason the architectural leak (now fixed) showed as O(N²).
- AVN_NO_COMPRESS=1 only bypasses zlib at the rep_store layer.
  Everything else (sha, base64 over the wire, JSON envelope, sqlite,
  rev-blob format) is identical. The delta is genuinely just the
  compression pass.
- `avg_commit_secs` excludes the first two batches because cold-start
  effects (sqlite first-write, mmap warmup, JIT-style allocator
  growth) skew them downward by ~10 ms. Steady state is what the
  comparison is measuring.

## Reproducing

```sh
# Empty repo + warm caches
rm -rf ~/avn_bench_repo bench/bench-*compress*

# Compress run (default)
python3 bench/bench_avn.py
# Outputs: bench/bench-compress.log
#          bench/bench-compress_timings.csv  (per-50-batch CSV)
#          bench/bench-compress_rss.csv      (1 Hz RSS sampler)
#          bench/bench-compress_server.log   (avnserver stdout)

# No-compress run
rm -rf ~/avn_bench_repo
python3 bench/bench_avn.py --no-compress
# Outputs analogous bench/bench-nocompress_*.{log,csv}
```

Each run is ~6.5 minutes wall-clock on a modest box; both fit
comfortably in 16 GB RAM.
