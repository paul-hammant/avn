#!/usr/bin/env python3
# Bench avnserver: 20,000 commits, 100K random text each, on /media/paul/eeee2/.
#
# Two output files (in this script's directory, suffixed by run tag):
#   bench-{tag}.log           wall-clock progress log
#   bench-{tag}_timings.csv   per-500-commit timings: batch_n, commits,
#                             batch_secs, avg_commit_secs, total_secs,
#                             repo_size_bytes
#
# Where {tag} is `compress` (default) or `nocompress` (--no-compress flag).
#
# `--no-compress` exports AVN_NO_COMPRESS=1 into the avnserver's env, which
# the rep_store uses to skip the zlib pass entirely (R285). Comparing the
# two CSVs isolates server-side compression cost in the commit latency.
#
# The script wipes /media/paul/eeee2/avn_bench_repo (per run), creates a
# fresh repo, starts avnserver on port 9990 in background, fires the
# commits in series, records per-500-batch timings, stops the server.
#
# Designed to run unattended. No tail-following needed — check the timings
# CSV any time to see progress.

import argparse
import os
import random
import re
import string
import subprocess
import sys
import time
from pathlib import Path

# R286 — `avn commit` success line shape. We grep the new rev_sha out
# and thread it as --parent-sha on the next commit, so each commit is
# a single round-trip (no /info GET).
SHA_RE = re.compile(r"\(sha: ([a-f0-9]{40})\)")

REPO_DIR     = Path("/media/paul/eeee2/avn_bench_repo")
PORT         = 9990
REPO_NAME    = "bench"
URL          = f"http://127.0.0.1:{PORT}/{REPO_NAME}"
TOTAL        = 20000
BATCH_SIZE   = 500
TEXT_SIZE    = 100 * 1024
AUTHOR       = "bench"

ROOT         = Path(__file__).resolve().parent.parent
ADMIN        = ROOT / "target/avnadmin/bin/avnadmin"
SERVER       = ROOT / "target/avnserver/bin/avnserver"
AVN          = ROOT / "target/avn/bin/avn"

OUT_DIR      = Path(__file__).resolve().parent

# Filled in by main() once we've parsed --no-compress.
LOG_FILE     = OUT_DIR / "bench.log"
TIMINGS_FILE = OUT_DIR / "bench_timings.csv"
SERVER_LOG   = OUT_DIR / "bench_server.log"


def log(msg: str) -> None:
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}\n"
    sys.stdout.write(line)
    sys.stdout.flush()
    with open(LOG_FILE, "a") as f:
        f.write(line)


def random_text(n: int, rng: random.Random) -> str:
    # ASCII letters/digits/space/newline — printable, no NUL bytes (which
    # would break exec argv passing). Random.choices is k=N so we get
    # exactly the byte count we asked for.
    alphabet = string.ascii_letters + string.digits + " \n"
    return "".join(rng.choices(alphabet, k=n))


def repo_bytes(path: Path) -> int:
    total = 0
    for p in path.rglob("*"):
        try:
            if p.is_file():
                total += p.stat().st_size
        except OSError:
            pass
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--no-compress",
        action="store_true",
        help="Run avnserver with AVN_NO_COMPRESS=1; rep blobs stored "
             "raw (no zlib). Output files get the 'nocompress' tag.",
    )
    args = parser.parse_args()
    tag = "nocompress" if args.no_compress else "compress"

    global LOG_FILE, TIMINGS_FILE, SERVER_LOG
    LOG_FILE     = OUT_DIR / f"bench-{tag}.log"
    TIMINGS_FILE = OUT_DIR / f"bench-{tag}_timings.csv"
    SERVER_LOG   = OUT_DIR / f"bench-{tag}_server.log"

    for binp in (ADMIN, SERVER, AVN):
        if not binp.exists():
            print(f"binary missing: {binp}", file=sys.stderr)
            return 2

    if not REPO_DIR.parent.exists():
        print(f"mount missing: {REPO_DIR.parent}", file=sys.stderr)
        return 2

    LOG_FILE.write_text("")
    SERVER_LOG.write_text("")
    with open(TIMINGS_FILE, "w") as tf:
        tf.write("batch_n,commits,batch_secs,avg_commit_secs,total_secs,repo_size_bytes\n")
        tf.flush()

    if REPO_DIR.exists():
        log(f"removing existing {REPO_DIR}")
        subprocess.run(["rm", "-rf", str(REPO_DIR)], check=True)

    log(f"run tag: {tag}")
    log(f"creating repo at {REPO_DIR}")
    subprocess.run([str(ADMIN), "create", str(REPO_DIR)], check=True)

    server_env = os.environ.copy()
    if args.no_compress:
        server_env["AVN_NO_COMPRESS"] = "1"
        log("AVN_NO_COMPRESS=1 (server-side zlib disabled)")

    log(f"starting avnserver on port {PORT}")
    srv_log = open(SERVER_LOG, "w")
    server = subprocess.Popen(
        [str(SERVER), REPO_NAME, str(REPO_DIR), str(PORT)],
        stdout=srv_log,
        stderr=subprocess.STDOUT,
        env=server_env,
    )
    time.sleep(2)
    if server.poll() is not None:
        srv_log.close()
        log(f"server died on startup; see {SERVER_LOG}")
        return 1

    rng = random.Random(0xA1)
    wall_start  = time.time()
    batch_start = wall_start
    last_i      = 0
    last_sha    = ""    # R286 chain CAS: empty for first commit (empty
                        # repo); subsequent commits pass the previous
                        # response's rev_sha as --parent-sha, eliminating
                        # the /info round-trip avn would otherwise issue.

    try:
        log(f"starting {TOTAL} commits × {TEXT_SIZE}B random text, batches of {BATCH_SIZE}")
        for i in range(1, TOTAL + 1):
            content   = random_text(TEXT_SIZE, rng)
            file_path = f"f/{i}.txt"
            argv = [
                str(AVN), "commit", URL,
                "--author", AUTHOR,
                "--log", f"r{i}: random",
                "--add-file", f"{file_path}={content}",
            ]
            if last_sha:
                argv.extend(["--parent-sha", last_sha])
            r = subprocess.run(argv, capture_output=True, timeout=120)
            if r.returncode != 0:
                tail = (r.stderr or r.stdout or b"")[-300:]
                log(f"COMMIT FAIL at i={i}: rc={r.returncode}: {tail!r}")
                break
            # Pull rev_sha out of "Committed revision N (sha: <40hex>)"
            # for the next commit's --parent-sha. If the line shape
            # changes (no sha), bail loudly — running blind would
            # silently downgrade to /info-per-commit.
            m = SHA_RE.search((r.stdout or b"").decode("utf-8", "replace"))
            if not m:
                log(f"COMMIT OUTPUT MISSING SHA at i={i}: {r.stdout[:200]!r}")
                break
            last_sha = m.group(1)
            last_i   = i

            if i % BATCH_SIZE == 0:
                now         = time.time()
                batch_secs  = now - batch_start
                total_secs  = now - wall_start
                avg         = batch_secs / BATCH_SIZE
                size        = repo_bytes(REPO_DIR)
                batch_n     = i // BATCH_SIZE
                with open(TIMINGS_FILE, "a") as tf:
                    tf.write(
                        f"{batch_n},{i},{batch_secs:.2f},{avg:.4f},"
                        f"{total_secs:.2f},{size}\n"
                    )
                    tf.flush()
                log(
                    f"batch {batch_n}: {i}/{TOTAL} commits, "
                    f"batch={batch_secs:.1f}s, avg={avg*1000:.0f}ms, "
                    f"repo={size // (1024 * 1024)}MB, total={total_secs:.0f}s"
                )
                batch_start = now

        elapsed = time.time() - wall_start
        log(f"DONE. {last_i} commits in {elapsed:.0f}s "
            f"({last_i / elapsed:.1f} commits/sec mean)")

    finally:
        log("stopping avnserver")
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        srv_log.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
