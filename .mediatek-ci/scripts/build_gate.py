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


def resolve_exclude_refs(src):
    """Refs whose history should NOT be built: upstream main/next, and
    mediatek-test-support itself (its commits only reach mediatek-staging via
    a merge commit and aren't "new MediaTek work")."""
    upstream_remote = os.environ.get("MTK_UPSTREAM_REMOTE", "mtk-upstream")
    upstream_refs = os.environ.get("MTK_UPSTREAM_REFS", "main next").split()
    ts_remote = os.environ.get("MTK_TEST_SUPPORT_REMOTE", "origin")
    ts_ref = os.environ.get("MTK_TEST_SUPPORT_REF", "mediatek-test-support")

    # MTK_*_REMOTE="" addresses a local branch directly (e.g. for local
    # testing against a branch that hasn't been pushed anywhere yet).
    excludes = [f"{upstream_remote}/{b}" if upstream_remote else b for b in upstream_refs]
    excludes.append(f"{ts_remote}/{ts_ref}" if ts_remote else ts_ref)

    missing = [r for r in excludes if not rev_exists(src, r)]
    if missing:
        die(f"exclusion ref(s) not found (fetch them first): {', '.join(missing)}")
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
    excludes = resolve_exclude_refs(src)
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
        flags = ["-o", out_dir, "-b", BUILD_BRANCH, "-c", str(count), "-M", *boards]
        if werror:
            flags.append("-E")
        log("running: buildman " + " ".join(flags))
        ret = buildman(src, *flags, stream=True).returncode

        summ = buildman(src, "-o", out_dir, "-b", BUILD_BRANCH, "-c", str(count), *boards, "-se")
        with open(summary_file, "a") as f:
            f.write(summ.stdout)
        sys.stdout.write(summ.stdout)
    finally:
        git(src, "branch", "-D", BUILD_BRANCH, check=False)
    return ret


def build(src, to_build, boards, out_dir, werror, summary_file):
    """Build every commit in `to_build` (oldest-first). Stops at the first
    failure; everything before it already built clean."""
    Path(summary_file).write_text("")

    if is_contiguous(src, to_build):
        ret = run_batch(src, to_build[-1], len(to_build), boards, out_dir, werror, summary_file)
        if ret == 0:
            log(f"OK: all {len(to_build)} commit(s) built clean")
        else:
            log(f"FAILED (buildman exit {ret}) -- see {summary_file}")
        return ret

    # Granular fallback: build one commit at a time so a failure is pinpointed
    # to the exact breaking commit (also the path for a set that spans a
    # merge, since that can't be a single contiguous batch).
    for i, sha in enumerate(to_build, 1):
        ret = run_batch(src, sha, 1, boards, out_dir, werror, summary_file)
        if ret != 0:
            log(f"FAILED at {sha[:12]} ({i}/{len(to_build)}) -- see {summary_file}")
            return ret
        log(f"OK: {sha[:12]} built clean ({i}/{len(to_build)})")
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
