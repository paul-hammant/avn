# Branching conundrum — design notes

A rethink of branches/tags/sparse-checkout, inspired by git's root-only
branches and Perforce's view/client specs. This document is a design
draft, not a ratified plan — numbered open questions at the bottom
need answers before implementation starts.

## The proposal, as I heard it

**Today (classic svn layout, which we inherited):**

- Branches = server-side `cp` of a subtree (e.g. `/trunk/src` →
  `/branches/feat/src`). They live *inside the tree*.
  `svn cp URL/trunk URL/branches/foo` makes a branch.
- Tags = same mechanism, different directory.
- Sparse checkout = property-based (`svn:ignore`, selective `--depth`).
- URL = `http://host/repo/sub/dir`.

**What the change would look like:**

- No more `/trunk`, `/branches/*`, `/tags/*` directories. The repo has
  one tree.
- A named first-class **branch** is a *view* over that tree, defined
  by a **branch spec** — ordered include/exclude path patterns
  (Perforce-style). The spec says "branch X sees these paths mapped
  from trunk."
- A **client spec** is a further subsetting used at checkout/update —
  same grammar, applied on top of whatever branch you're on.
- Checkout URL syntax: `https://example/branchname;path/to/resource`
  where `;` separates the branch name from an optional starting
  subpath.
- **Merge** between branches uses the branch specs to decide which
  paths correspond. A systematic merge from trunk to branch "feat"
  runs through feat's spec to know which trunk paths map where.
- **Tags** = snapshot of `(branch-spec + content) @ rev`.

## The forked-road decision: one tree or one-tree-per-branch?

**Option A — flat namespace, branches are pure views.**

The server stores a single "trunk" tree; branches are purely
view-level — no physical branch tree exists. A commit on branch
`feat` modifies the shared tree, but only at paths the `feat` spec
includes. Merging trunk→feat is "apply trunk's changes to the
shared tree that feat sees."

- **Pros:** dead simple storage. Branch creation is free (register a
  spec and done). Perforce-style.
- **Cons:** two commits to "different branches" can't touch
  overlapping paths without confusing each other. Branches aren't
  independent histories — they're filtered lenses on one history.
  Makes real isolation (the thing most users want branches for)
  impossible.

**Option B — each branch has its own tree root, git-like.**

Each branch has its own physical Merkle tree, indexed by branch
name. The spec defines *what paths from a base branch the new branch
inherits at creation*. After creation, they diverge independently.
Merging moves content between two independent trees.

- **Pros:** real independence. Branch creation is O(size-of-
  included-subtree) in rep-sharing terms, which with our content-
  addressed store is mostly free. Merging becomes a three-way merge
  between two tree roots — exactly what our Phase 5.12/5.16 merge
  machinery already does.
- **Cons:** more metadata per rev (which branch's tree moved).
  Branch registry + per-branch head pointers. Slightly more surface.

I *think* you want Option B. Your framing was git-like branching;
your *tooling* was Perforce-style specs. The specs define what a
branch INHERITS at creation and what a later SYSTEMATIC MERGE
considers, not how the branch's tree is stored at runtime.

## How the existing codebase maps onto Option B

We already have everything needed except the branch registry:

- **rev blob** currently has `root: <sha>`. We'd need `branch:
  <name>` and the root becomes "this branch's root at this rev."
- **multi-branch** uses per-branch head files in
  `$repo/branches/<name>/head`. Not a registry blob — see the
  "Unlimited history scale" section below for why.
- **commits** already target a tree — they just need a `branch`
  field in the JSON telling the server which branch head to
  advance.
- **WC** already remembers its base URL (`svnae_wc_switch` from
  Phase 5.15). Add a `branch` info-row.
- **Checkout URL** today is `http://host/repo`. New form would be
  `http://host/repo/BRANCH[;PATH]`. Empty branch = `main`.

## Branch specs: how complex

Perforce's `View` rules are ordered include/exclude:

```
+//depot/main/... //my-ws/...
-//depot/main/docs/... //my-ws/docs/...
```

Later rules override earlier. It's tricky but learnable.

A simpler model: **include-only**, no excludes. A branch spec is a
list of path globs the branch sees. If you want to carve out a
subdir, you list every path *except* that subdir.

Your line: "I wonder if that's too complicated in retrospect."
Agreed. YAGNI — do include-only first, revisit if a real use case
needs excludes. If someone hits "I want everything except `docs/`,"
we add that as Phase 8.6 when it's concrete.

## URL grammar with `;`

- `https://host/repo/main` — checkout branch `main`, root.
- `https://host/repo/main;src/main.c` — blame/cat/etc. at `src/main.c`
  on branch `main`.
- `https://host/repo/feat;README.md` — same for branch `feat`.
- `https://host/repo/main;src/main.c@42` — pegged at rev 42.

Rules:

- `;` separates branch from in-branch path. First `;` wins.
  Semicolons in paths after the first `;` are literal.
- Branch name is required in every URL. No implicit default — we
  could allow it (`https://host/repo` = `main`) but making it
  explicit avoids foot-guns.
- `@` still pegs revs, unchanged from reference svn.

## What "on a branch" means at WC level

The WC stores `branch=<name>` in `.svn/wc.db`'s info table alongside
`url`, `base_url`, `repo`. Every operation (update, commit, status)
reads this and targets the right branch's head on the server.

`svn switch NEWBRANCH;path` is the existing Phase 5.15 pipeline —
just targeting a different branch's tree instead of a different
URL's tree.

## What "doesn't include" means at checkout

If branch `feat`'s spec includes only `src/` and `docs/`, the WC
materialized by `svn co http://host/repo/feat` has only those two
subtrees. `foo/` (present in `main`) doesn't exist in the WC.

Commit on `feat` that writes to `foo/bar.txt` → 403, "path outside
branch spec." The RW-everywhere rule from Phase 7.7 now has a
parallel: branch-spec-everywhere.

## Tags

A tag is a snapshot of `(branch, branch-spec, head-rev)` at creation
time. Stored in a tags registry (shape like branches, but immutable
once written). `svn tag NAME BRANCH` records the current state.
`svn co http://host/repo/TAGNAME;path` works like a branch but the
head never moves.

## Merge across branches

`svn merge --from A --to B` walks A's and B's trees concurrently,
using A's spec to filter what's considered. For each path present
in both A and B's view, find divergences and run 3-way merge.
For paths present only in A (included by A's spec, excluded from B's
or never created), it's an add on B. For paths only in B, untouched.

The existing Phase 5.12/5.16 merge code already does 3-way on two
tree sha pairs. The new piece is "walk A's tree through A's spec as
a filter."

## Migration from existing repos

We drop reference-svn compatibility for tree layouts that used
`/trunk`, `/branches/*`, `/tags/*` as conventions. Explicit
divergence, consistent with earlier phases.

- Existing repos in-flight: still load (they have a rev blob with a
  `root:` but no `branch:`). Treated as "one branch called `main`
  containing the whole old tree." Users can then carve out branch
  specs from `main` as they want.
- `svnadmin load` on a reference-svn-format dump: same deal. The
  load produces a repo with one branch `main` containing the
  literal tree the dump described, including any `/trunk`,
  `/branches`, `/tags` dirs as ordinary dirs. No automatic magic.
- `svnadmin create` creates a fresh repo with just a `main` branch,
  empty tree.

## Unlimited history scale

User constraint: scale to Piper-class (80TB) with no slowdown as
history grows. That reshapes some of the storage choices below.

**Enemies of scale in a VCS server:**

1. *O(history) per operation.* Any query that walks all revs is
   disqualifying at 10M-rev scale.
2. *Single files that grow without bound.* A rev-index whose size
   is linear in revs, a branches registry rewritten on every
   commit, a rep-cache.db with billions of rows.
3. *Reparse/reload on every request.* Loading the whole object
   graph into memory is a non-starter.
4. *Sync points.* Global locks serialising all writers.

**What our existing design already handles:**

- Content-addressed rep store with two-level fanout:
  `$repo/reps/aa/bb/<sha>.rep`. Blob lookup is O(1) ext4
  directory entry, O(log N) sqlite lookup in rep-cache.db. ✓
- Rep-sharing: identical content shares storage across revs and
  branches. The biggest win for scale — a 10-year-old file that
  never changed uses one blob slot, not 10,000. ✓
- Commit finalise is O(depth-of-tree), not O(history). Only dirs
  on the edit path re-hash. ✓

**What would hit walls without the changes below:**

- `$repo/revs/NNNNNN` — one file per rev in one directory. At
  10M revs that's 10M files in one dir; `readdir` degrades.
- `svnae_repos_log` walks every rev into an in-memory array.
  Disaster at 10M revs.
- `svn blame` walks rev 1..N looking for revs that touched a
  path. Needs a secondary index — `path_rev(branch, path, rev)`
  — to be O(touched-revs), not O(total-revs).
- One monolithic branches-registry blob rewritten on every commit
  means every commit touches a file whose size scales with
  branch count.

**Scale-aware decisions baked into Phase 8.1:**

1. **Per-branch head files, not a registry blob.**

   ```
   $repo/branches/main/head        ← latest rev#, tree-sha, spec-sha
   $repo/branches/feat/head
   $repo/branches/feat/spec        ← static once branch exists (unless amended)
   ```

   Each commit writes only its own branch's `head` file
   atomically. No per-commit rewrite of a registry that grows
   with branch count. Branch listing is `ls $repo/branches/` —
   O(branches), not O(commits). Tags: same pattern under
   `$repo/tags/`, but `head` files are immutable once written.

2. **Per-branch rev pointers with two-level fanout.**

   ```
   $repo/branches/main/revs/00/00/000042
   $repo/branches/feat/revs/00/00/000042
   ```

   At 10M revs per branch under two-level fanout (~256×256 sub-
   dirs) that's ~150 files per leaf dir. Fine for any modern
   filesystem.

3. **Path-rev index for blame-class queries.**

   New table in rep-cache.db:

   ```sql
   CREATE TABLE path_rev (
       branch TEXT NOT NULL,
       path   TEXT NOT NULL,
       rev    INTEGER NOT NULL,
       PRIMARY KEY (branch, path, rev)
   );
   CREATE INDEX path_rev_lookup ON path_rev (branch, path);
   ```

   On every commit, for every path the commit touches, insert
   one row. Blame, log-of-a-path, and "who last modified X" all
   become index scans instead of history walks. This is the trick
   Piper and Perforce both use: keep a secondary index for the
   query shape you want to be O(touched), not O(total).

   Built in to Phase 8.1 from day one — retrofitting against a
   10M-rev repo is an hours-long batch job we don't want to run.

4. **Paginated log endpoint.**

   New endpoint:

   ```
   GET /repos/{r}/log?branch=feat&limit=100&before=5000
   ```

   Server returns a single page. Client walks by chunks. The
   existing `/log` endpoint stays as-is but internally caps at
   10k revs (fails loud if there are more) — no breaking change
   since user explicitly accepted no back-compat.

5. **Keep the door open for horizontal scale.**

   Not a shipping feature, but the design should not preclude it.
   Content-addressed storage is inherently shardable (rep blobs
   can live behind any `get(sha)` service — local FS today, S3 or
   a cluster later). Per-branch metadata is independent, so
   sharding by branch is possible. Rep-cache.db could be swapped
   for Postgres via the same interface.

**What this doesn't fix:**

- Checkout of a 1TB tree is still O(tree size). Use a client
  spec (Phase 8.3) to carve out a workable subset — it's a
  feature, not a bug.
- `svn log` across all branches across all time is inherently
  O(branches × revs). Paginate and paginate hard.
- `svn verify` walks the whole tree; at 80TB that's hours. Not
  sped up in 8.x; add `--from-rev N` for resumable later.

## Proposed phase breakdown

Big enough to be a family of phases, not one commit.

**Phase 8.1 — Branch infrastructure + default `main` + path-rev index**
- Per-branch head files: `$repo/branches/<name>/head` stores
  `rev=N tree=<sha> spec=<sha>` (latest for that branch).
- Per-branch rev pointers with two-level fanout:
  `$repo/branches/<name>/revs/aa/bb/NNNNNN`.
- `svnadmin create` initialises `$repo/branches/main/head` with
  rev 0, empty tree, empty spec.
- Rev blob gains `branch:` field identifying which branch this
  rev advanced.
- Commits target a specific branch via new `"branch"` field in
  /commit JSON, defaulting to `main`.
- Path-rev index table in rep-cache.db; populated on every
  commit.
- Checkout/cat/list URLs accept `branchname;...` syntax;
  `branchname` alone = branch's root.
- `svn co http://host/repo/main` checks out the `main` branch.
  Bare `http://host/repo` is now an error (explicit branch
  required) — fork-divergence, per user direction no back-compat
  expected.
- The URL changes are the big surface; existing code that treats
  the URL as `http://host/repo/path` grows a branch-parsing step.

**Phase 8.2 — Branch creation + specs (include-only)**
- `svn branch create NAME --from BASE --include PATH [--include PATH...]`
  — registers a new branch. Initial tree is BASE's tree filtered
  through the include globs (content-addressed copy; rep-sharing
  gives us that free).
- Branch spec stored as its own blob, referenced from the branch
  registry entry.
- Commits on a branch are validated against the spec (paths outside
  the include set → 403).
- `svn branches` lists all.
- `svn switch branchname;path` moves the WC to a branch.

**Phase 8.3 — Client spec**
- `svn checkout --client-spec FILE URL` filters further. Written to
  `.svn/client_spec`.
- `svn up` respects it.
- `svn client-spec [edit]` to manage.

**Phase 8.4 — Systematic cross-branch merge**
- `svn merge --from BRANCH_A --to BRANCH_B`.
- Walks the union of both specs, uses Merkle diff to find
  differences, runs 3-way where they overlap.
- Conflict resolution reuses Phase 5.13.

**Phase 8.5 — Tags**
- Tags are named snapshots in a new registry.
- `svn tag NAME BRANCH[@rev]` records (branch-spec-at-rev,
  tree-sha-at-rev).
- Immutable; attempts to commit against a tag → 403.

## Answered questions

1. **Option B** — each branch has its own tree root. ✓
2. **Include-only specs** for 8.2. Excludes deferred. ✓
3. **Default branch name** = `main`. ✓
4. **Path-rev index built into 8.1** (not a later phase). ✓
5. **Paginate the log breakingly** — no back-compat constraint. ✓
6. **Design should allow horizontal scaling later** (don't lock
   out sharding / alternative storage). ✓
7. **Checkout of very large trees** — developer is expected to
   supply a client spec. ✓

## Open questions that gate implementation

8. **No implicit branch in URL** — require `;` if you want to name
   a subpath. `https://host/repo/main` is a branch ref; to reach a
   file inside you write `https://host/repo/main;src/foo.c`. Or do
   we allow `https://host/repo/main/src/foo.c`? Grammar choice. I
   lean toward *requiring* `;` to make the branch/path boundary
   unambiguous in parsers — otherwise `svn co http://host/repo/feat`
   is ambiguous (is `feat` a branch or a subdir of `main`?).

9. **Commit branch inference.** When the WC says `branch=feat`, does
   `svn commit` unconditionally commit to `feat`, or does it look at
   the URL on each file? I think: the WC carries the branch, every
   commit from that WC targets that branch, full stop. Switching
   branches requires `svn switch`.

10. **Existing tests.** We have 44 suites currently, many of which
    use the old `/trunk`, `/branches/*`, `/tags/*` layout in their
    seed data. These would continue to work (the layout is just
    directories in `main`'s tree now) but the *semantics* change —
    e.g. `test_branch.sh` exercises `svn cp URL/src URL/src-branch`
    which is now "copy a dir on the same branch," not "make a
    branch." That's still a valid operation (server-side copy is
    still a thing for refactoring), but we should rename the test
    to `test_dir_cp.sh` or similar, and write new tests for the real
    branch machinery.

11. **Phase ordering.** Is five phases the right granularity, or
    should I collapse 8.1+8.2 (can't really ship one without the
    other — 8.1 without branch creation is useless)? Or split 8.1
    further?

12. **Branch-spec grammar specifics.** My suggestion: simple glob
    patterns like `.gitignore`/shell glob. `src/**`, `docs/**/*.md`,
    `README.md`. Later we can add excludes with `!prefix`. For now
    every pattern is an include.

13. **What happens to `svn cp` on-branch?** `svn cp URL1 URL2` where
    both URLs are within branch `main` — that's still a dir-copy
    operation, Phase 7.7's RW-everywhere rule still applies, the
    result is committed to `main`. Fine. The *cross-branch* case
    (`svn cp feat;X main;Y`) is new territory — probably just
    "forbidden in 8.1; use `svn merge --from feat --to main` for
    that."

14. **Per-commit branch — what stops a writer from committing to
    whatever branch they feel like?** Phase 7.2's write-ACL applies
    per path; we now also need a branch-level permission model.
    Options:
    - Branch-level ACLs (same rule grammar, applied to branch as a
      whole).
    - Piggyback on path ACLs: committing to branch `feat` requires
      write on every path the commit touches, where those paths
      live in feat's view. This is what Phase 7.2 already does,
      just through a different lens.
    - Reserve `svn branch create` as a super-user operation.

    I'd lean on the existing path-ACL system: branch-level is
    over-engineering. Creating a branch = super-user by default.

## What I'd do if you just said "go"

If you don't answer the open questions, my read is: Option B,
include-only specs, default `main`, `;` required to name a
path-in-branch, WC-carries-branch, phases as outlined. I'd
implement Phase 8.1 first as the proof-of-shape (branch registry,
URL grammar, commit to named branch, WC tracks branch). If that
lands cleanly, the rest follows — each is ~1 shim + 1 CLI
subcommand + 1 test.

Start with 8.1 and reconvene before 8.2 so you can see the URL
grammar and commit shape before we commit to it.

## What I'm not sure about

**The cognitive load on users.** We're introducing: branch names,
branch specs, client specs, the `;` URL syntax, and new merge
semantics. That's a lot of surface. Reference svn + Phase 5.15's
`svn switch` got away with a single URL-as-identifier model. Now
every URL needs a branch disambiguation.

**Whether branch specs are the right abstraction at all.** Perforce
uses them because their depot is one big namespace and you need
some way to carve out a workspace. In our Option-B world, a branch
already IS an independent tree — why do we need a spec? The answer
is *branch creation* — the spec describes what slice of the base
branch the new branch starts with. After creation, the spec arguably
isn't needed anymore; the branch is just a tree with its own head.

But then systematic merge *does* need the specs, to know which
paths map across branches. So specs are "rules for how this branch
was born and how it should be related back to its parent." That's
coherent.

Client specs are a separate concern — they're just sparse-checkout
configuration, applied WC-side. We could ship them as a totally
separate feature from the branch overhaul.
