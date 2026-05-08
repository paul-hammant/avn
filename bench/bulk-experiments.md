# Bulk write experiments

Empirical sweeps to characterise avn's commit pipeline under load. Each
experiment records what was measured, the configuration, the headline
numbers, and any follow-ups it surfaced.

Hardware (all experiments unless noted):

- Linux laptop, kernel 6.17 (host: paul-oldnuc)
- Storage: 128 GB MicroSD class 10 mounted at `/media/paul/eeee2/`
  (filesystem: ext4, no special mount opts, default 4 KiB blocks)
- avnserver + avn CLI built from this checkout (Round 285 onward
  for `AVN_NO_COMPRESS` env hook)

Workload pattern (all experiments unless noted):

- 20,000 sequential commits via `avn commit URL --add-file PATH=CONTENT`
- Each commit adds a unique 100 KiB blob (deterministic seed `0xA1`,
  alphabet = `[A-Za-z0-9 \n]`, ~64 distinct chars)
- Single-author, single-client, no concurrency, no merges
- Measurement: per-500-commit batches; latency = `wall(batch end) -
  wall(batch start)` divided by 500

Repository layout per run: `/media/paul/eeee2/avn_bench_repo`
(wiped between runs).

## Experiments

### E1. Baseline — compression on (default) — INVALID

**Run command:** `python3 bench/bench_avn.py`
**Date:** 2026-05-07
**Output:** `bench-compress_timings.csv`, `bench-compress.log`

**⚠ Bug discovered post-run:** every commit landed an empty tree. The
script used `--add-file f/{i}.txt=...`, but pre-R285 stateless
`avn commit --add-file` silently dropped any file whose parent
directory didn't exist. There was no `--mkdir f` in the script and
the parent never got created, so the file edit was unanchored and
the server's tree-rebuild walked past it. Each commit produced a
rev blob + branch pointer + empty root tree — no content stored.

So **E1's numbers measure "empty commit overhead"**, not 100K-content
commits. The ~125 ms steady-state latency reflects rev-metadata
write + branch-pointer write + ~3× SD-card fsync. Compression
never fired because no content blobs were ever encoded.

R285 fixes the dropped-files bug: `cmd_commit` now auto-emits
`mkdir` ops for parent dirs of every `--add-file` path. Re-run is
needed to get real numbers.

| Metric                | Value             |
| --------------------- | ----------------- |
| Total wall-clock      | 2,568 s (~43 min) |
| Mean throughput       | 7.8 commits/sec   |
| Steady-state latency  | 122–132 ms/commit |
| Cold-start latency    | 88 ms/commit (batch 1) |
| Peak warm-up latency  | 165 ms/commit (batch 4) |
| Final repo size (du)  | 309 MiB           |
| `reps/` (content)     | 147 MiB           |
| `revs/` (rev blobs)   | 79 MiB (mostly 4 KiB-block waste; each rev blob is a few hundred bytes) |
| `branches/` (pointers)| 80 MiB (same waste shape) |
| `rep-cache.db`        | 3.1 MiB (linear with commit count) |
| File count            | 60,010            |

**Observations:**

- **No degradation cliff.** Latency per commit is essentially flat
  from batch 5 (commit 2,500) onward — no quadratic blow-up as the
  repo grows past 20K commits.
- **Warm-up shape is non-monotonic.** 88 ms (cold) → 165 ms (peak) →
  125 ms (steady). Something hits a threshold around commits 1500–2000,
  slows things down for one batch, then steady-state caching wins.
  Prime suspect: `rep-cache.db` page-cache eviction (tracked as
  Q-PERF-2 below).
- **Filesystem block waste is the silent cost.** ~159 MiB of the
  309 MiB on disk is 4 KiB-block padding around tiny `revs/` and
  `branches/` files. Each rev blob is a few hundred bytes; each
  per-branch pointer is 41 bytes (a SHA + newline). At scale, more
  than half the disk footprint is rounding, not data.
- **Compression on uniform random ASCII is surprisingly good.**
  Random over a 64-char alphabet, 100 KiB → ~7.4 KiB stored
  (≈13× ratio). LZ77 captures the chance repetitions; Huffman
  exploits the small alphabet's character-frequency skew.

### E2. Compression off — `AVN_NO_COMPRESS=1` — ALSO INVALID

**Run command:** `python3 bench/bench_avn.py --no-compress`
**Date:** 2026-05-07
**Output:** `bench-nocompress_timings.csv`, `bench-nocompress.log`

**Same bug as E1.** Every commit empty. Numbers nearly identical to
E1 (130 ms/commit, 309 MiB on disk) because both runs were doing
the same thing — empty rev-blob writes — and never touched the
zlib path.

Confirmed the env hook itself works (probe with a 2 KiB
flat-path commit: blob first byte is `R` with `AVN_NO_COMPRESS=1`,
`Z` without). The hook just never fired in E2 because no content
ever reached `rep_encode_blob`.

Re-run needed post-R285 to get real comparison.

### E3 / E4 — TODO: re-run E1/E2 after R285 fix

Re-run baseline (compress) and no-compress, this time with
auto-mkdir actually storing the 100 KiB content blobs.

Predictions (for E3 — compression on, post-fix):

- `reps/` should grow to roughly **1.4–1.5 GiB** (100 KiB random
  ASCII over 64-char alphabet compresses to ~77 KiB at zlib level
  6 — the real ratio is closer to 0.77, not the 0.07 the previous
  empty-commit numbers misled me into expecting).
- Per-commit latency rises by 30–80 ms over E1's 125 ms — the
  100 KiB write to `reps/` (preceded by zlib.deflate) wasn't part
  of the empty-commit path.
- `revs/` and `branches/` stay at 79–80 MiB (no change — same
  rev count, same per-rev-blob size).
- Total `du` ~1.7 GiB.

E4 (no-compress) prediction:

- `reps/` ~2.0 GiB (raw 100 KiB × 20K).
- Per-commit latency drops by the zlib cost — probably
  10–20 ms savings per commit, depending on whether the SD-card
  sync floor dominates.
- Total `du` ~2.2 GiB.

## Open follow-ups

### Q-PERF-1. Pack `revs/` and `branches/` to reclaim block waste.

Each per-rev metadata file and per-branch pointer is its own ext4 inode
costing 4 KiB minimum, but the actual data is well under 1 KiB. At 20K
commits that's 159 MiB of pure waste. Classical SVN's fs_fs has a
`pack` command that bundles old revs into a single file; avn could do
the same. On a fresh repo (no migration concern) packing could be the
default from the start.

Verdict pending E2 numbers — if the no-compression run shows the same
waste shape, packing's a clean win regardless of what's in `reps/`.

### Q-PERF-2. Investigate the warm-up bump (88 → 165 → 125 ms).

E1 showed latency climbing for the first 2K commits, peaking at batch
4, then settling. Nothing obvious in the workload triggers this — the
server's been running steadily. Suspects in priority order:

1. **rep-cache.db SQLite page cache** evicting once it crosses some
   threshold; subsequent inserts churn the page cache before the OS
   buffer-cache catches up.
2. **The zlib pass** allocating + freeing buffers in a way that fragments
   memory. E2 would discriminate this — if `--no-compress` flattens the
   bump, this is the cause.
3. **ext4 directory hash splits** as `revs/` grows past some node-fanout
   threshold (default htree starts splitting around 12K entries).

Cheapest probe: re-run E1 instrumented to print rep-cache.db size at
each batch boundary.

## How to run a new experiment

1. Add a section above with name `### EN. ...`, run command, predicted
   numbers, and a placeholder for output files.
2. Run the script (or a tweaked version).
3. Backfill the actual numbers, keeping predictions intact for later
   review.
4. If the experiment surfaces a follow-up, file it under "Open
   follow-ups" with a probe-cost estimate and what would resolve it.
