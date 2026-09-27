"""Hand-picked scenarios where remmy must leave exactly what ``/bin/rm`` leaves (test_properties generates more)."""

import os
from pathlib import Path

import fstree
import pytest
from fstree import Hardlink, Special, Symlink
from oracle import RM, compare_with_rm

pytestmark = [pytest.mark.differential, pytest.mark.skipif(not os.access(RM, os.X_OK), reason=f"{RM} not available")]

TREE = {"k": "keep", "d": {"a": {"b": {"c": ""}}, "p": Special.FIFO, "l": Symlink(".."), "h": Hardlink("../k")}}
UNREADABLE = {"d": {"locked": {"s": ""}, "free": {"x": ""}, "f": ""}}
UNWRITABLE = {"d": {"frozen": {"a": "", "sub": {"b": ""}}, "free": ""}}

# name: (spec, args, (path to chmod, mode) or None)
CASES = {
    "file": ({"f": "x", "k": ""}, ["f"], None),
    "missing": ({"k": ""}, ["nope"], None),
    "dir-no-r": ({"d": {"x": ""}}, ["d"], None),
    "empty-dir-no-r": ({"d": {}}, ["d"], None),
    "tree": (TREE, ["-r", "d"], None),
    "symlink-to-dir": ({"real": {"x": ""}, "link": Symlink("real")}, ["-r", "link"], None),
    "dangling-symlink": ({"l": Symlink("gone")}, ["l"], None),
    "file-trailing-slash": ({"f": ""}, ["f/"], None),
    "mixed-operands": ({"a": "", "d": {"x": ""}, "b": {"y": {}}}, ["-r", "a", "missing", "d", "b"], None),
    "unreadable-subdir": (UNREADABLE, ["-r", "d"], ("d/locked", 0o000)),
    "unwritable-subdir": (UNWRITABLE, ["-r", "d"], ("d/frozen", 0o555)),
    "unwritable-parent": ({"p": {"t": {"a": ""}}}, ["-r", "p/t"], ("p", 0o555)),
}


@pytest.mark.parametrize("name", list(CASES))
def test_matches_rm(remmy_bin: Path, workdir: Path, name: str, is_root: bool) -> None:
    spec, args, lock = CASES[name]
    if is_root and lock:
        pytest.skip("root bypasses permissions")

    def recipe(root: Path) -> None:
        fstree.build(root, spec)
        if lock:
            os.chmod(root / lock[0], lock[1])

    compare_with_rm(remmy_bin, workdir, recipe, args)
