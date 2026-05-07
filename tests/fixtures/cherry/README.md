# Cherry-pick convergence fixtures

Mirrors the "limits of merging" blog
(https://github.com/paul-hammant/limits-of-merging-experiment).
Classical-SVN reference scripts live on the `svn-version` branch
of that repo
(https://github.com/paul-hammant/limits-of-merging-experiment/tree/svn-version)
— they run the same workflow against the C Subversion implementation
and pass cleanly, which means anything our port fails at the C
version doesn't is a regression we introduced.

- `baseline/` — three Sinatra-app source files at the C1 state.
- `patches/` — git format-patches for C2, C3, C4, C5.

Used by `working_copy/.tests-cherry_convergence.ae`. Driver builds a
5-rev trunk by applying C2..C5 sequentially, branches twice, runs
two cherry-pick orderings (C4→C3 vs C3→C4) followed by a sweep
merge, and asserts both branches end byte-identical.

Currently surfaces several avn bugs (see TODO.md, "Cherry-pick
convergence — bug-bait test"). Not wired into the root aggregator
— run on demand:

    aeb working_copy/.tests-cherry_convergence.ae
