<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# MediaTek U-Boot CI: build gate

GitLab CI build gate for the MediaTek U-Boot custodian tree. Builds every
commit on `mediatek-staging` that isn't already upstream, so a series can't
break compilation or `git bisect`. Compile-only for now: no flashing or
hardware testing yet (that's future work).

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
- `<known-good-sha>` is added when available: `CI_MERGE_REQUEST_DIFF_BASE_SHA`
  for an MR pipeline, or `CI_COMMIT_BEFORE_SHA` for an ordinary fast-forward
  push (verified with `merge-base --is-ancestor` first). This is what avoids
  rebuilding commits already verified by a previous pipeline run -- **there
  is no persisted "already built" record**; GitLab's own per-push ref
  history serves as the record. When neither applies (new branch,
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
