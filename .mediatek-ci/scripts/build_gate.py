#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""MediaTek U-Boot build gate: build every commit on mediatek-staging that
isn't already upstream (and isn't from mediatek-test-support itself).

One entry point for local runs and GitLab CI. It:
  1. resolves the set of commits to build: everything reachable from the
     target ref but not from upstream main/next, not from
     mediatek-test-support, and (when known) not from the last verified
     state of this branch,
  2. runs buildman across that set for the configured board scope,
  3. writes a human-readable summary artifact,
and exits non-zero as soon as a commit fails to build (with WERROR=1, on
warnings too), leaving later commits in the set unbuilt.

There is no persisted "already built" record: the "last verified state"
above comes entirely from git/GitLab's own ref history (CI_COMMIT_BEFORE_SHA
for an ordinary fast-forward push, CI_MERGE_REQUEST_DIFF_BASE_SHA for an MR
pipeline). When neither applies (new branch, force-push, scheduled or manual
run) the full "not upstream, not test-support" set is built -- which is also
the correct behavior the very first time this CI runs.

Configuration is via environment variables:
  UBOOT_SRC               U-Boot tree to build (default: git toplevel of CWD)
  MTK_TARGET_REF          the tip to build up to (default: $CI_COMMIT_SHA, or
                          HEAD if unset -- for local runs)
  MTK_UPSTREAM_REMOTE     remote holding upstream main/next (default:
                          mtk-upstream; use "origin" for local testing where
                          origin already is the mainline U-Boot repo)
  MTK_UPSTREAM_REFS       space-separated upstream branch names to exclude
                          (default: "main next")
  MTK_TEST_SUPPORT_REMOTE remote holding mediatek-test-support (default:
                          origin)
  MTK_TEST_SUPPORT_REF    branch name to exclude (default:
                          mediatek-test-support)
  MTK_BUILDMAN_TERMS      space-separated buildman board-selection terms,
                          passed as bare positional args (matched against
                          each board's target/arch/cpu/board/vendor/soc/
                          cfg_name -- see `buildman -H`), not --boards
                          (default: "mediatek mt7628")
  CI_MERGE_REQUEST_DIFF_BASE_SHA / CI_COMMIT_BEFORE_SHA  set by GitLab; used
                          automatically as an extra "known good" exclusion
  WERROR=1                treat warnings as errors (buildman -E)
  OUT_DIR                 buildman output dir (default: $TMPDIR/mtk-build)
  SUMMARY_FILE            where to write the summary (default:
                          <UBOOT_SRC>/build-summary.txt)
"""

import os
import subprocess
import sys
from pathlib import Path

BUILD_BRANCH = "__mtk_ci_build"


def log(msg):
    print(f">> {msg}", file=sys.stderr, flush=True)


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def git(src, *args, check=True):
    """Run git in `src`, capturing output. Dies on failure when check=True."""
    cp = subprocess.run(["git", "-C", str(src), *args], text=True, capture_output=True)
    if check and cp.returncode != 0:
        die(f"git {' '.join(args)} failed: {cp.stderr.strip()}")
    return cp


def buildman(src, *args, stream=False):
    """Invoke the tree's buildman. stream=True inherits stdout/stderr (live log)."""
    cmd = [sys.executable, "tools/buildman/buildman", *args]
    if stream:
        return subprocess.run(cmd, cwd=str(src))
    return subprocess.run(cmd, cwd=str(src), capture_output=True, text=True)


def rev_exists(src, ref):
    return git(src, "rev-parse", "-q", "--verify", ref, check=False).returncode == 0


def resolve_upstream_excludes(src):
    """Upstream refs whose history should NOT be built. Mandatory: die if
    one isn't fetched, since silently building against the wrong base would
    be worse than failing loudly."""
    upstream_remote = os.environ.get("MTK_UPSTREAM_REMOTE", "mtk-upstream")
    upstream_refs = os.environ.get("MTK_UPSTREAM_REFS", "main next").split()

    # MTK_UPSTREAM_REMOTE="" addresses a local branch directly (e.g. for
    # local testing against a branch that hasn't been pushed anywhere yet).
    excludes = [f"{upstream_remote}/{b}" if upstream_remote else b for b in upstream_refs]

    missing = [r for r in excludes if not rev_exists(src, r)]
    if missing:
        die(f"upstream exclusion ref(s) not found (fetch them first): {', '.join(missing)}")
    return excludes


def resolve_test_support_excludes(src, target):
    """Refs/commits whose history should NOT be built because they're from
    mediatek-test-support itself, not new MediaTek work: its own commits
    only reach mediatek-staging via a merge commit.

    Two independent checks, either being enough on its own:
      - the mediatek-test-support branch ref, best-effort (not fatal if
        missing or behind -- pushing it out of sync with mediatek-staging
        used to make its own commits get built standalone, checked out
        against test-support's own stale lineage).
      - every "Merge branch '<test-support-ref>'" commit found directly in
        `target`'s own history, excluding that merge's second parent. This
        is what actually closes the gap above: it's derived purely from
        `target`'s own DAG, which is already complete the moment `target`
        itself is pushed, so it can never lag the way a separately-pushed
        branch ref can.
    """
    ts_remote = os.environ.get("MTK_TEST_SUPPORT_REMOTE", "origin")
    ts_ref = os.environ.get("MTK_TEST_SUPPORT_REF", "mediatek-test-support")

    excludes = []
    ref = f"{ts_remote}/{ts_ref}" if ts_remote else ts_ref
    if rev_exists(src, ref):
        excludes.append(ref)
    else:
        log(f"note: {ref} not found -- relying on merge-commit detection below")

    merges = git(src, "log", target, "--merges",
                 f"--grep=^Merge branch '{ts_ref}'", "--format=%H",
                 check=False).stdout.split()
    for merge_sha in merges:
        second_parent = git(src, "rev-parse", f"{merge_sha}^2", check=False)
        if second_parent.returncode == 0:
            excludes.append(second_parent.stdout.strip())

    return excludes


def resolve_known_good(src):
    """A ref known to already be built, on top of the standing exclusions:
    the merge-request diff base for an MR pipeline, else the previous branch
    tip for a true fast-forward push, else None (new branch / force-push /
    scheduled / manual run -- build the full exclusion-based set)."""
    mr_base = os.environ.get("CI_MERGE_REQUEST_DIFF_BASE_SHA", "")
    if mr_base and set(mr_base) != {"0"} and rev_exists(src, mr_base):
        log(f"merge request: treating {mr_base[:12]} as already built")
        return mr_base

    before = os.environ.get("CI_COMMIT_BEFORE_SHA", "")
    if before and set(before) != {"0"} and rev_exists(src, before):
        target = os.environ.get("MTK_TARGET_REF") or os.environ.get("CI_COMMIT_SHA", "HEAD")
        if git(src, "merge-base", "--is-ancestor", before, target, check=False).returncode == 0:
            log(f"fast-forward push: treating {before[:12]} as already built")
            return before
        log(f"push is not a fast-forward of {before[:12]} (force-push/rebase) -- ignoring it")

    return None


def resolve_to_build(src):
    """Oldest-first list of commit SHAs to build."""
    target = os.environ.get("MTK_TARGET_REF") or os.environ.get("CI_COMMIT_SHA", "HEAD")
    excludes = resolve_upstream_excludes(src) + resolve_test_support_excludes(src, target)
    known_good = resolve_known_good(src)
    if known_good:
        excludes.append(known_good)

    args = ["rev-list", "--reverse", target, "--not", *excludes]
    return git(src, *args).stdout.split()


def is_contiguous(src, shas):
    """True iff `shas` is exactly the gap-free history between shas[0]^ and
    shas[-1] -- i.e. buildman can cover it with a single -c <count> batch."""
    if not shas:
        return False
    cp = git(src, "rev-list", "--reverse", f"{shas[0]}^..{shas[-1]}", check=False)
    if cp.returncode != 0:
        return False
    return cp.stdout.split() == shas


def run_batch(src, base_sha, count, boards, out_dir, werror, summary_file):
    """Build `count` commit(s) of real history ending at (and including)
    base_sha, append a human-readable summary, and report the exit status.

    The one place that knows buildman's actual -b/-c contract: -b <ref> -c
    <n> walks plain `git log` (not rev-list/first-parent) for n commits back
    from <ref>, so a non-contiguous commit set can't be handed to buildman
    directly -- only ever call this with a SHA + count that really is a
    contiguous run of history ending there (count=1 always is)."""
    git(src, "branch", "-f", BUILD_BRANCH, base_sha)
    try:
        # -M (--allow-missing): fake out missing external blobs (e.g.
        # mt7621_stage_sram.bin) instead of treating them as a build error --
        # CI will never have MediaTek's proprietary binaries.
        # -W (--ignore-warnings): without it, buildman still exits 101 (not
        # 0) whenever -M had to fake a blob, which build_gate.py would
        # otherwise treat as a build failure. Per buildman.rst, defaulting
        # both on for every run is the documented, generally-safe setting.
        # -N (--no-subdirs): without it, buildman nests output under
        # <out_dir>/<branch-name>/ instead of <out_dir> directly -- keeps the
        # output layout predictable regardless of BUILD_BRANCH's name.
        flags = ["-o", out_dir, "-b", BUILD_BRANCH, "-c", str(count), "-M", "-W", "-N", *boards]
        if werror:
            flags.append("-E")
        log("running: buildman " + " ".join(flags))
        ret = buildman(src, *flags, stream=True).returncode

        summ = buildman(src, "-o", out_dir, "-b", BUILD_BRANCH, "-c", str(count), "-N", *boards, "-se")
        with open(summary_file, "a") as f:
            f.write(summ.stdout)
        sys.stdout.write(summ.stdout)
    finally:
        git(src, "branch", "-D", BUILD_BRANCH, check=False)
    return ret


def chunk_contiguous(src, to_build):
    """Split `to_build` (oldest-first) into maximal runs, each exactly the
    gap-free history between its own endpoints -- i.e. each is coverable by
    one buildman -c <count> batch. A long-lived integration branch like
    mediatek-staging has merge commits scattered through it (upstream tag
    merges, mediatek-test-support merges), which break contiguity at those
    points -- but only there; treating the whole set as all-or-nothing (one
    batch or 100% one-by-one) throws away a batch-sized speedup for every
    long contiguous run in between."""
    chunks = []
    i = 0
    n = len(to_build)
    while i < n:
        j = i + 1
        while j < n and is_contiguous(src, to_build[i:j + 1]):
            j += 1
        chunks.append(to_build[i:j])
        i = j
    return chunks


def build(src, to_build, boards, out_dir, werror, summary_file):
    """Build every commit in `to_build` (oldest-first). Stops at the first
    failure; everything before it already built clean."""
    Path(summary_file).write_text("")

    done = 0
    total = len(to_build)
    for chunk in chunk_contiguous(src, to_build):
        if len(chunk) > 1:
            ret = run_batch(src, chunk[-1], len(chunk), boards, out_dir, werror, summary_file)
            if ret == 0:
                done += len(chunk)
                log(f"OK: {len(chunk)} commit(s) built clean "
                    f"({chunk[0][:12]}..{chunk[-1][:12]}) ({done}/{total})")
                continue
            log(f"batch of {len(chunk)} commit(s) failed (buildman exit {ret}) -- "
                f"retrying one at a time to pinpoint the break")

        # Granular: either a genuinely isolated commit (chunk of 1, e.g. a
        # merge commit), or a batch that just failed above -- build one at a
        # time so a failure is pinpointed to the exact breaking commit.
        for sha in chunk:
            ret = run_batch(src, sha, 1, boards, out_dir, werror, summary_file)
            if ret != 0:
                log(f"FAILED at {sha[:12]} ({done + 1}/{total}) -- see {summary_file}")
                return ret
            done += 1
            log(f"OK: {sha[:12]} built clean ({done}/{total})")
    return 0


def main():
    # buildman/patman read $USER (patchstream compares commit authors to
    # "$USER@"); CI containers often leave it unset, which crashes with
    # "None + '@'". Default it.
    os.environ.setdefault("USER", "mtk-ci")

    src = Path(os.environ.get("UBOOT_SRC")
               or git(Path.cwd(), "rev-parse", "--show-toplevel").stdout.strip())
    if not (src / "tools/buildman/buildman").exists():
        die(f"not a U-Boot tree (no tools/buildman/buildman): {src}")

    boards = os.environ.get("MTK_BUILDMAN_TERMS", "mediatek mt7628").split()
    out_dir = os.environ.get("OUT_DIR", str(Path(os.environ.get("TMPDIR", "/tmp")) / "mtk-build"))
    summary_file = os.environ.get("SUMMARY_FILE", str(src / "build-summary.txt"))
    werror = os.environ.get("WERROR", "0") == "1"

    to_build = resolve_to_build(src)
    if not to_build:
        log("no commits above the upstream/test-support/known-good base -- nothing to build.")
        return 0

    log(f"boards: {' '.join(boards)}")
    log(f"building {len(to_build)} commit(s), oldest first: "
        f"{to_build[0][:12]}..{to_build[-1][:12]}")

    return build(src, to_build, boards, out_dir, werror, summary_file)


if __name__ == "__main__":
    sys.exit(main())
