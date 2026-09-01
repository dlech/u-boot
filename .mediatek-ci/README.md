<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# MediaTek U-Boot CI: build gate

GitLab CI build gate for the MediaTek U-Boot custodian tree. Builds every
commit on `mediatek-staging` that isn't already upstream, so a series can't
break compilation or `git bisect`, plus a few whole-tree checks alongside it
(see "Checks beyond the build"). No flashing or hardware testing yet (that's
future work). A manual run can gate
`mediatek-for-next`/`mediatek-for-main` the same way, without those branches
carrying any of this -- see "Building a branch other than the pipeline's own
ref" below.

## Layout

```
.mediatek-ci/gitlab-ci.yml  the GitLab CI pipeline definition
.mediatek-ci/scripts/build_gate.py   the build gate itself
```

Everything here is a new, self-contained file, kept together under one
top-level directory. Nothing under `mediatek-test-support` (where this
lives) ever modifies a file that also exists upstream, so merging/rebasing
this branch against upstream `main`/`next` never conflicts.

Deploy by setting this project's Settings -> CI/CD -> General pipelines ->
"CI/CD configuration file" to `.mediatek-ci/gitlab-ci.yml` (GitLab
accepts a path, not just a root filename).

## How the commit set is computed

Buildman has no way to build an arbitrary, non-contiguous set of commits (it
only builds `-b <ref> -c <count>`, i.e. a plain `git log` walk back from one
ref). So `build_gate.py` first computes the exact set of commits that need
building with:

```sh
git rev-list --reverse <target> --not \
    <upstream-remote>/main <upstream-remote>/next \
    origin/mediatek-test-support \
    [<known-good-sha>]
```

- `<target>` is `$CI_COMMIT_SHA` (or `HEAD` for local runs).
- `mediatek-test-support` is excluded because its own commits only reach
  `mediatek-staging` via a merge commit -- they aren't new MediaTek work and
  shouldn't be built as such.
- `<known-good-sha>` is added when available: `MTK_KNOWN_GOOD` if it was
  passed by hand, else `CI_MERGE_REQUEST_DIFF_BASE_SHA` for an MR pipeline,
  else `CI_COMMIT_BEFORE_SHA` for an ordinary fast-forward push (each
  verified with `merge-base --is-ancestor` first). This is what avoids
  rebuilding commits already verified by a previous pipeline run -- **there
  is no persisted "already built" record**; GitLab's own per-push ref
  history serves as the record. When none applies (new branch,
  force-push, scheduled or manual run), the full "not upstream, not
  test-support" set is built, which is also the correct behavior the very
  first time this CI runs.

The one accepted gap: if a pipeline run is skipped or fails outright for one
push, the next *ordinary* push's `CI_COMMIT_BEFORE_SHA` reflects the actual
current ref state, not "the last state that was actually verified" -- so
that push's commits could go unbuilt until a later event (a rebase, a
manual/scheduled run) recomputes the full set. This was a deliberate
tradeoff to avoid needing a state-tracking branch/repo and a push-back
credential.

Because `mediatek-staging` gets rebased periodically, a rebase mints new
SHAs for otherwise-unchanged commits, which makes the whole backlog look new
and triggers a full rebuild. That's accepted as correct-but-costly rather
than trying to match rebased commits by content.

## Board scope

No hand-maintained board-list file. Buildman selects boards by bare
positional terms (not `--boards`, which needs an exact target-name match) --
these regex-match against each board's `target/arch/cpu/board/vendor/soc/
cfg_name` properties, which come from `boards.cfg` (in turn derived from each
board's `SYS_VENDOR`/`SYS_SOC` Kconfig and `MAINTAINERS`).

```sh
tools/buildman/buildman mediatek mt7628 -n
```

selects exactly 37 boards: all `Vendor=mediatek` ARM boards (Genio EVKs, RFB
boards, bpir2/bpir3, unielec, pumpkin, evk, mt8512/8516/8518, mt7629_rfb) plus
the third-party MIPS boards on the `mt7628` SoC (`gardena-smart-gateway-mt7688`,
`linkit-smart-7688`, `vocore2`) that build the custodian-maintained `mtmips`
driver code even though their own `Vendor` tag differs -- matching "every
defconfig that includes code we maintain, and not more than that" per
`MAINTAINERS`' `ARM MEDIATEK` and `MIPS MEDIATEK` sections. `mt7620`/`mt7621`/
`mt7629` need no extra term: every board on those SoCs is already
`Vendor=mediatek`.

Configurable via the `MTK_BUILDMAN_TERMS` CI/CD variable (default
`"mediatek mt7628"`) -- widening or narrowing scope is a variable change, not
a file edit. Recheck the board count if defconfigs are added/removed.

## Missing external blobs

Some boards (e.g. `mt7621_rfb`) reference proprietary blobs (`mt7621_stage_sram.bin`)
CI will never have. Buildman is always run with `-M` (`--allow-missing`, fakes
the blob instead of failing) and `-W` (`--ignore-warnings`, needed because `-M`
alone still exits 101, which `build_gate.py` would otherwise treat as a build
failure) -- this is buildman's own documented "generally safe to default on"
combination (see `buildman.rst`, "Support for binary blobs"), not something
conditional.

## Worktree checkout overhead (tried caching, reverted)

Building N boards in one buildman invocation makes buildman create N
per-thread git worktrees up front (`<OUT_DIR>/.bm-work/<n>/`, one full checked
-out copy of the source tree each), which shows up as ~30 sequential
"Checking out worktree" steps (~90s) before any compilation starts. Caching
that directory across pipeline runs (via GitLab's `cache:`) was tried, but
reverted: caching means archiving/uploading N full source-tree checkouts every
run, which cost far more time than the checkout it saved.

Upstream's own `.gitlab-ci.yml` avoids this differently: it defines one CI job
*per board* (~50 of them), each running `buildman -w` (`--work-in-output`),
which is hard-restricted to exactly one board and one commit
(`control.py`: *"-w can only be used with a single board/commit"*) and,
because there's only ever one thing being built, skips the per-thread worktree
machinery entirely -- GitLab parallelizes across jobs instead of buildman
parallelizing across worktrees. Adopting that here would mean splitting this
one job into one-per-board, which is a bigger restructure than this project
has taken on so far; the ~90s checkout cost is accepted for now.

## Batching contiguous commits

A force-push/rebase of `mediatek-staging` invalidates `CI_COMMIT_BEFORE_SHA`
(see "How the commit set is computed" above), so the *entire* un-upstreamed
stack has to rebuild -- which can be dozens of commits. Building each one in
its own buildman invocation (`-c 1`) was the original design, on the
(mistaken) assumption that a set spanning a merge commit had to be
all-or-nothing: either the whole set is one gap-free run buildman can batch
with a single `-c <count>`, or every commit goes one at a time. In practice a
long-lived integration branch has merge commits scattered through it
(upstream tag merges, `mediatek-test-support` merges), so the *whole* set
almost never qualifies as gap-free even though most of it, between those
merge points, is one straightforward linear run.

`build()` now calls `chunk_contiguous()` to split the commit set into maximal
gap-free runs first, and batches each run with one buildman call; a run only
falls back to one-commit-at-a-time if the batch itself fails (to pinpoint the
exact break) or if it's a genuine singleton (typically a merge commit itself).
This changes nothing about which commits get built or in what order -- it's
purely fewer, larger buildman invocations for the same result. Confirmed
against the real `mediatek-staging` history: a 44-commit rebuild chunked into
just 2 batches (a 1-commit merge and one 43-commit run), instead of 44
separate invocations.

The job also carries a `timeout: 2h` as a safety net in case a rebuild is
still large enough to need it.

## Boards added mid-range

buildman generates its board list (`boards.cfg`) once, from whatever commit
is checked out in the top-level tree it's invoked from -- not per-commit.
Since `build_gate.py` always invokes it from the tip being validated, a board
whose defconfig was added by a commit still in `to_build` is on that list for
every earlier commit too, even ones that predate the file existing. buildman
doesn't skip a board it can't find a defconfig for -- it just runs
`make <target>_defconfig` and that hard-fails ("No such file or directory"),
breaking the whole invocation for unrelated boards and commits.

`run_batch()` guards against this directly: before each buildman call it
compares the defconfigs present at the tip against the defconfigs present at
the oldest commit that call is about to build (`missing_boards()`), and
passes any board that doesn't exist yet to buildman's `-x`. That board is
skipped for that invocation rather than crashing it. The gap this leaves --
that specific board goes unbuilt for the commit(s) in *this* push that add
it -- is accepted: from the next push onward the board exists at every
commit in range and builds normally like any other.

## Checks beyond the build

Three more jobs run in parallel with the build (`needs: []`, so they don't
wait for it). The point of all of them is that a MediaTek series shouldn't be
what breaks a check upstream already runs, or arrive with review comments a
script could have caught.

| job | gates? | what |
| --- | --- | --- |
| `mediatek tree checks` | yes | `buildman --maintainer-check` (every defconfig has a MAINTAINERS entry) and upstream's "no `#define CONFIG_*` outside Kconfig" grep |
| `mediatek checkpatch` | no | `scripts/checkpatch.pl -g` per commit in the gate's commit set |
| `mediatek dtbs_check` | no | each `OF_UPSTREAM` board's devicetree validated against `dts/upstream/Bindings` |

`mediatek tree checks` gates because both of its checks are clean on our tree
right now, so a failure can only be something this branch introduced. They
inspect the tip only -- not every commit -- and neither builds anything, so
the job needs no upstream fetch (`MTK_FETCH_RANGE_REFS: "0"`) and finishes in
about a minute.

The other two are `allow_failure: true` and there to be read, not obeyed.
checkpatch runs `--strict` here (see `.checkpatch.conf`) and not every message
it raises is worth acting on. It iterates `build_gate.py --list-commits`,
which prints the same commit set the gate builds instead of building it --
same exclusions, same known-good, so the two jobs can never disagree about
which commits are "ours". Merge commits are skipped.

Both of these produce far more output than anyone wants scrolling past, so
each puts its detail in a [GitLab collapsed
section](https://docs.gitlab.com/ci/jobs/job_logs/#custom-collapsible-sections)
-- one per commit for checkpatch, one for the whole finding list for
dtbs_check. Everything is in the job log; neither needs an artifact.

### dtbs_check

`make dtbs_check` taken apart, for two reasons: the board set stays
`MTK_BUILDMAN_TERMS` (the same scope as the build gate) rather than a
hand-kept list that would drift, and buildman builds all of those boards in
parallel instead of one sequential `make` per board. `make dt_binding_check`
builds `processed-schema.json` once, `buildman -k` leaves each board's
`u-boot.dtb` behind (`dts/dt.dtb` is a build-tree intermediate and doesn't
survive), and one `dt-validate` covers the lot. About four minutes end to end.

What the check is for is the `*-u-boot.dtsi` we write on top of a devicetree
that upstream bindings actually describe. A board with an in-tree devicetree
and no bindings has nothing to be validated against, so dt-validate reports
essentially its whole DT as unrecognised -- that is the difference between 43
findings and 1480.

So the board set is `CONFIG_OF_UPSTREAM=y` (today `mt7629_rfb`,
`mt8365_evk`, `mt8370_genio_510_evk`, `mt8390_genio_700_evk` and the two
`mt8395_genio_1200_evk*`) plus anything named in
`MTK_DTBS_CHECK_EXTRA_BOARDS`. That variable exists for a board mid-upstream:
its devicetree is still in-tree, so it isn't `OF_UPSTREAM`, but its bindings
are already under `dts/upstream/Bindings` -- possibly only because an
`MTK TEST:` commit on this branch put them there, which is the case where
checking them against what we ship matters most.

`mt8366_genio_360_evk` is listed there already, ahead of its defconfig: a
board named before it exists costs one "was not built" warning per run and
nothing else, and this way the check is live the moment the defconfig lands.
Skipped boards are likewise named in the job log, so nothing is silently
dropped either way.

All the boards are still built, because whether a board is `OF_UPSTREAM` is
only knowable from a configured tree -- the filter reads each board's
`.config` after the build.

It reports instead of gating for two reasons. `dt-validate` exits 0 no matter
what it finds, so there is nothing to gate on without a checked-in baseline to
diff against -- and an exact-line baseline would churn, because the messages
embed phandle numbers that shift whenever a node moves. And the 43 findings
that remain are all upstream Linux DT problems rather than anything U-Boot
added (`mediatek,mt6359` `#sound-dai-cells`, `mt8188-scp-dual` `reg-names`,
`mt8188-tphy`'s compatible list, `mt8195` jpeg/iommu node names, and 24 on
`mt7629_rfb`), so a gate would start red.

The per-board counts print uncollapsed for a quick look; the findings
themselves follow in the collapsed section.

## Building a branch other than the pipeline's own ref

`mediatek-for-next` and `mediatek-for-main` are what gets sent upstream, so
they carry neither the `MTK TEST:` commits nor this directory -- merging
`mediatek-test-support` into them just to get CI would put the CI itself in
the pull request. But GitLab reads a branch pipeline's config from that
branch's own tree, so a branch without `.mediatek-ci/` can't host a pipeline
at all.

Naming a target branch resolves that: the pipeline runs on a ref that *does*
have `.mediatek-ci/` and contributes nothing but that directory, while
everything actually built comes from the target branch. The job stashes
`.mediatek-ci/` outside the worktree, fetches the target branch (same project
-- all these branches live in one repo), checks it out detached, and runs the
stashed `build_gate.py` against it. Buildman, its generated board list, and
every built commit then come from the target branch's tree; nothing is merged
and the built tree is byte-identical to what goes upstream.

Build > Pipelines > Run pipeline, with:

```
ref:            mediatek-test-support
Target branch:  mediatek-for-next   (or mediatek-for-main)
```

"Target branch" is a *pipeline input* (`spec:inputs` at the top of
`gitlab-ci.yml`), so the Run pipeline form offers it by name as a dropdown --
nothing to remember. It just feeds the `MTK_TARGET_BRANCH` variable, so
setting that variable by hand still works and still wins, which is what the
local invocation below and any non-UI trigger use.

The standing exclusions need no adjustment for these branches: upstream
`main`/`next` is the right base for both, and the `mediatek-test-support`
exclusion is a harmless no-op there because none of its commits are
reachable.

`CI_COMMIT_BEFORE_SHA`/`CI_MERGE_REQUEST_DIFF_BASE_SHA` describe the
pipeline's own ref, not the target, so the job unsets them -- which means
every run rebuilds the branch's whole un-upstreamed stack. That is cheap
enough in practice: the for-\* branches are linear, so `chunk_contiguous()`
collapses the entire stack into a single buildman batch. Fill in the "Known
good" input (`MTK_KNOWN_GOOD`) to skip everything up to a commit an earlier
run already built clean.

## Running locally

```sh
MTK_UPSTREAM_REMOTE=origin \
MTK_TEST_SUPPORT_REF=mediatek-test-support \
python3 .mediatek-ci/scripts/build_gate.py
```

`MTK_UPSTREAM_REMOTE=origin` works for a local checkout where `origin` is
already the mainline U-Boot repo (as opposed to CI, where a second remote is
added because `origin` there is the custodian project). See
`build_gate.py`'s module docstring for the full list of environment
variables.

`MTK_TARGET_REF` picks the tip to build, so a local checkout can gate another
branch without checking it out -- the local equivalent of
`MTK_TARGET_BRANCH` above, except that buildman and its board list come from
the current worktree rather than from the target:

```sh
MTK_UPSTREAM_REMOTE=origin \
MTK_TARGET_REF=mediatek-for-next \
python3 .mediatek-ci/scripts/build_gate.py
```
