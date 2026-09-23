"""``/bin/rm`` as a reference: remmy must leave exactly what rm leaves."""

from __future__ import annotations

import os
import subprocess
from collections.abc import Callable
from pathlib import Path

import fstree
from harness import RUN_TIMEOUT, run_remmy

RM = "/bin/rm"

Recipe = Callable[[Path], object]


def _shape(root: Path) -> dict[str, tuple[str, str | None]]:
    """Identity-free view of a tree: path -> (kind, content digest)."""
    return {k: (e.kind, e.digest) for k, e in fstree.snapshot_dir(root).items()}


def _restore_modes(root: Path) -> None:
    for _, dirnames, _, dirfd in os.fwalk(root, follow_symlinks=False):
        for d in dirnames:
            os.chmod(d, 0o755, dir_fd=dirfd, follow_symlinks=False)


def compare_with_rm(remmy_bin: Path, root: Path, recipe: Recipe, args: list[str], threads: int = 4) -> None:
    """Build ``recipe`` twice under ``root``, run each tool, compare outcomes.

    The surviving paths, their types and contents must match, and both tools
    must agree on success versus failure and on whether they complained.
    """
    ours, theirs = root / "remmy", root / "rm"
    for side in (ours, theirs):
        side.mkdir()
        recipe(side)

    mine = run_remmy(remmy_bin, args, cwd=ours, threads=threads)
    ref = subprocess.run(
        [RM, *args],
        cwd=theirs,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=RUN_TIMEOUT,
        check=False,
    )

    # Permissions are part of some recipes; open up before inspecting.
    _restore_modes(ours)
    _restore_modes(theirs)
    # One assert listing every mismatch: this also runs under Hypothesis, which
    # needs a raised failure (soft checks would hide it from the shrinker).
    mismatches = []
    ours_shape, theirs_shape = _shape(ours), _shape(theirs)
    if ours_shape != theirs_shape:
        only_ours = sorted(set(ours_shape) - set(theirs_shape))
        only_theirs = sorted(set(theirs_shape) - set(ours_shape))
        changed = sorted(k for k in set(ours_shape) & set(theirs_shape) if ours_shape[k] != theirs_shape[k])
        mismatches.append(
            f"leftovers differ: only remmy kept {only_ours}, only rm kept {only_theirs}, differ {changed}"
        )
    if (mine.returncode == 0) != (ref.returncode == 0):
        mismatches.append(f"exit status: remmy {mine.returncode}, rm {ref.returncode}")
    if bool(mine.stderr) != bool(ref.stderr):
        mismatches.append("only one of them wrote to stderr")
    assert not mismatches, "\n".join(mismatches) + f"\n{mine}\nrm stderr: {ref.stderr!r}"
