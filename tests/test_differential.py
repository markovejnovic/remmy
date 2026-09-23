"""Differential tests: remmy must leave exactly what ``/bin/rm`` leaves.

Hand-picked scenarios; ``test_properties`` covers generated ones.
"""

from __future__ import annotations

import os
from pathlib import Path

import fstree
import pytest
from fstree import Hardlink, Special, Symlink
from oracle import RM, Recipe, compare_with_rm

pytestmark = pytest.mark.differential


def _locked(path: str, mode: int, spec: fstree.Spec) -> Recipe:
    def recipe(root: Path) -> None:
        fstree.build(root, spec)
        os.chmod(root / path, mode)

    return recipe


RECIPES: dict[str, tuple[Recipe, list[str]]] = {
    "file": (lambda r: fstree.build(r, {"f": "x", "k": ""}), ["f"]),
    "missing": (lambda r: fstree.build(r, {"k": ""}), ["nope"]),
    "dir-no-r": (lambda r: fstree.build(r, {"d": {"x": ""}}), ["d"]),
    "empty-dir-no-r": (lambda r: fstree.build(r, {"d": {}}), ["d"]),
    "tree": (
        lambda r: fstree.build(
            r,
            {
                "k": "keep",
                "d": {"a": {"b": {"c": ""}}, "p": Special.FIFO, "l": Symlink(".."), "h": Hardlink("../k")},
            },
        ),
        ["-r", "d"],
    ),
    "symlink-to-dir": (
        lambda r: fstree.build(r, {"real": {"x": ""}, "link": Symlink("real")}),
        ["-r", "link"],
    ),
    "dangling-symlink": (lambda r: fstree.build(r, {"l": Symlink("gone")}), ["l"]),
    "file-trailing-slash": (lambda r: fstree.build(r, {"f": ""}), ["f/"]),
    "mixed-operands": (
        lambda r: fstree.build(r, {"a": "", "d": {"x": ""}, "b": {"y": {}}}),
        ["-r", "a", "missing", "d", "b"],
    ),
    "unreadable-subdir": (
        _locked("d/locked", 0o000, {"d": {"locked": {"s": ""}, "free": {"x": ""}, "f": ""}}),
        ["-r", "d"],
    ),
    "unwritable-subdir": (
        _locked("d/frozen", 0o555, {"d": {"frozen": {"a": "", "sub": {"b": ""}}, "free": ""}}),
        ["-r", "d"],
    ),
    "unwritable-parent": (
        _locked("p", 0o555, {"p": {"t": {"a": ""}}}),
        ["-r", "p/t"],
    ),
}


@pytest.fixture(autouse=True)
def _need_rm() -> None:
    if not os.access(RM, os.X_OK):
        pytest.skip(f"{RM} not available")


@pytest.mark.parametrize("name", list(RECIPES))
def test_matches_rm(remmy_bin: Path, workdir: Path, name: str, is_root: bool) -> None:
    if is_root and name.startswith("un"):
        pytest.skip("root bypasses permissions")
    recipe, args = RECIPES[name]
    compare_with_rm(remmy_bin, workdir, recipe, args)
