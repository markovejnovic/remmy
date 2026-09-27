"""``/bin/rm`` as a reference: remmy must leave exactly what rm leaves."""

import os
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from subprocess import DEVNULL

import fstree
from harness import RUN_TIMEOUT, run_remmy

RM = "/bin/rm"

# GNU rm rmdirs a directory it cannot read, so an empty one goes; remmy, like BSD rm, reports it.
RM_REMOVES_UNREADABLE_EMPTY_DIRS = sys.platform == "linux"

Recipe = Callable[[Path], object]


def _shape(root: Path) -> dict[str, tuple[str, str | None]]:
    # Permissions are part of some recipes; open up before inspecting.
    for _, dirnames, _, dirfd in os.fwalk(root, follow_symlinks=False):
        for d in dirnames:
            fstree.chmod_nofollow(d, 0o755, dir_fd=dirfd)
    return {k: (e.kind, e.digest) for k, e in fstree.snapshot_dir(root).items()}


def compare_with_rm(remmy_bin: Path, root: Path, recipe: Recipe, args: list[str], threads: int = 4) -> None:
    """Build ``recipe`` twice, run each tool; leftovers, success, and whether stderr is empty must match."""
    ours, theirs = root / "remmy", root / "rm"
    for side in (ours, theirs):
        side.mkdir()
        recipe(side)
    mine = run_remmy(remmy_bin, args, cwd=ours, threads=threads)
    ref = subprocess.run([RM, *args], cwd=theirs, stdin=DEVNULL, capture_output=True, timeout=RUN_TIMEOUT, check=False)

    # One raised assert (not soft checks) so Hypothesis's shrinker sees the failure.
    mismatches = []
    a, b = _shape(ours), _shape(theirs)
    if a != b:
        changed = sorted(k for k in a.keys() & b.keys() if a[k] != b[k])
        only_a, only_b = sorted(a.keys() - b.keys()), sorted(b.keys() - a.keys())
        mismatches.append(f"leftovers differ: only remmy kept {only_a}, only rm kept {only_b}, differ {changed}")
    if (mine.returncode == 0) != (ref.returncode == 0):
        mismatches.append(f"exit status: remmy {mine.returncode}, rm {ref.returncode}")
    if bool(mine.stderr) != bool(ref.stderr):
        mismatches.append("only one of them wrote to stderr")
    assert not mismatches, "\n".join(mismatches) + f"\n{mine}\nrm stderr: {ref.stderr!r}"
