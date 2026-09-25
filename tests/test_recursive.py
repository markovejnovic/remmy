"""Recursive removal: tree shapes, thread counts, symlink containment, operands."""

from __future__ import annotations

import errno
import os
from pathlib import Path

import fstree
import pytest
from fstree import Hardlink, Special, Symlink
from harness import Runner
from pytest_check import check


def test_directory_without_recursive_is_refused_and_untouched(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"d": {"f": "x", "e": {}}, "empty": {}})
    before = fstree.snapshot_dir(workdir)

    res = run("d", "empty")

    with check:
        assert res.returncode == 1, res
    with check:
        assert len(res.errors) == 2, res
    with check:
        assert os.strerror(errno.EISDIR) in res.stderr
    with check:
        assert fstree.snapshot_dir(workdir) == before


def test_empty_directory(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"d": {}, "keep": {}})

    res = run("-r", "d", threads=threads)

    with check:
        assert (res.returncode, res.stdout, res.stderr) == (0, "", ""), res
    with check:
        assert fstree.listing(workdir) == {"keep"}


def test_mixed_inode_types_throughout_tree(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(
        workdir,
        {
            "t": {
                "f": "file",
                "empty": b"",
                "fifo": Special.FIFO,
                "sock": Special.SOCKET,
                "dangling": Symlink("nowhere"),
                "up": Symlink(".."),
                "self": Symlink("self"),
                "hl": Hardlink("f"),
                ".hidden": {".also": "x", "..weird": {}},
                "nest": {"a": {"b": {"c": {"d": {"leaf": "x", "fifo": Special.FIFO}}}}},
                "emptydirs": {"e1": {}, "e2": {"e3": {}}},
            },
            "keep": "k",
        },
    )

    res = run("-r", "t", threads=threads)

    with check:
        assert (res.returncode, res.stdout, res.stderr) == (0, "", ""), res
    with check:
        assert fstree.listing(workdir) == {"keep"}


def test_wide_directory(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"w": {f"f{i}": "" for i in range(20_000)}})

    res = run("-r", "w", threads=threads)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


def test_many_sibling_directories(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"w": {f"d{i}": {"f": "", "g": {"h": ""}} for i in range(3_000)}})

    res = run("-r", "w", threads=threads)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


def test_bushy_tree(run: Runner, workdir: Path, threads: int) -> None:
    def level(depth: int) -> fstree.Spec:
        if depth == 0:
            return {f"f{i}": "x" for i in range(4)}
        return {f"d{i}": level(depth - 1) for i in range(5)} | {"f": "x"}

    fstree.build(workdir, {"t": level(5)})  # 3906 dirs

    res = run("-r", "t", threads=threads)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


def test_deep_chain_within_path_max(run: Runner, workdir: Path, threads: int) -> None:
    length = fstree.deep_chain(workdir / "deep", depth=400, name="d")
    assert length < 1024

    res = run("-r", "deep", threads=threads)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


# --- Containment: never escape the operand -----------------------------------


def test_symlinks_inside_tree_are_never_followed(run: Runner, workdir: Path, tmp_path: Path, threads: int) -> None:
    outside = fstree.build(tmp_path / "outside", {"file": "keep", "dir": {"nested": {"deep": "keep"}}})
    fstree.build(workdir, {"sibling": {"s": "keep"}})
    fstree.build(
        workdir,
        {
            "t": {
                "abs_dir": Symlink(str(outside)),
                "abs_file": Symlink(str(outside / "file")),
                "rel_sibling": Symlink("../sibling"),
                "parent": Symlink(".."),
                "root": Symlink("/"),
                "sub": {"deeper_escape": Symlink("../../sibling")},
            }
        },
    )
    outside_before = fstree.snapshot_dir(outside)
    sibling_before = fstree.snapshot_dir(workdir / "sibling")

    res = run("-r", "t", threads=threads)

    with check:
        assert res.returncode == 0, res
    with check:
        assert not fstree.exists(workdir / "t")
    with check:
        assert fstree.snapshot_dir(outside) == outside_before
    with check:
        assert fstree.snapshot_dir(workdir / "sibling") == sibling_before


def test_hard_links_leaving_the_tree_survive(run: Runner, workdir: Path) -> None:
    fstree.build(
        workdir, {"outside": "shared", "t": {"a": Hardlink("../outside"), "d": {"b": Hardlink("../../outside")}}}
    )

    res = run("-r", "t")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"outside"}
    with check:
        assert (workdir / "outside").read_text() == "shared"
    with check:
        assert os.lstat(workdir / "outside").st_nlink == 1


def test_neighbours_with_shared_prefix_are_untouched(run: Runner, workdir: Path) -> None:
    spec = {"d": {"x": ""}, "d2": {"x": ""}, "d.bak": {"x": ""}, "d ": {"x": ""}, "dd": ""}
    fstree.build(workdir, spec)
    survivors = {k for k in spec if k != "d"}
    before = {k: v for k, v in fstree.snapshot_dir(workdir).items() if k.split("/")[0] in survivors}

    res = run("-r", "d")

    with check:
        assert res.returncode == 0, res
    after = fstree.snapshot_dir(workdir)
    del after[""]  # workdir's own link count drops with d gone
    with check:
        assert after == before


# --- Operand spellings -------------------------------------------------------


@pytest.mark.parametrize(
    "spelling",
    ["d", "d/", "d//", "./d", "./d/", "sub/../d", "ABS", "ABS/"],
)
def test_operand_spellings(run: Runner, workdir: Path, spelling: str) -> None:
    fstree.build(workdir, {"d": {"x": {"y": ""}, "z": ""}, "sub": {}})
    operand = spelling.replace("ABS", str(workdir / "d"))

    res = run("-r", operand)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"sub"}


def test_operand_in_deep_relative_cwd(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"a": {"b": {"c": {"t": {"x": {"y": ""}}}}}})

    res = run("-r", "t", cwd=workdir / "a/b/c", threads=threads)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"a", "a/b", "a/b/c"}


@pytest.mark.parametrize("recursive", [True, False], ids=["r", "no-r"])
@pytest.mark.parametrize("operand", [".", "..", "sub/.", "sub/..", "./", "sub/..//"])
def test_dot_and_dotdot_operands_are_refused(run: Runner, workdir: Path, operand: str, recursive: bool) -> None:
    """POSIX: an operand whose last component is `.` or `..` gets a diagnostic and nothing else."""
    fstree.build(workdir, {"keep": "x", "a": {"f": "x", "b": {"g": "x", "sub": {"h": "x"}}}})
    before = fstree.snapshot_dir(workdir)

    res = run(*(["-r"] if recursive else []), operand, cwd=workdir / "a" / "b")

    with check:
        assert res.returncode == 1, res
    with check:
        assert res.stderr, res
    with check:
        assert fstree.snapshot_dir(workdir) == before


def test_other_operands_are_removed_after_a_refused_dot(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"keep": "x", "gone": {"f": "x"}, "also": "x"})

    res = run("-r", ".", "gone", "also", cwd=workdir)

    with check:
        assert res.returncode == 1, res
    with check:
        assert fstree.listing(workdir) == {"keep"}


def test_parent_relative_operand(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"here": {}, "there": {"x": {"y": ""}}})

    res = run("-r", "../there", cwd=workdir / "here")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"here"}


def test_multiple_directory_operands(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(
        workdir,
        {f"t{i}": {"a": {"b": ""}, "c": ""} for i in range(20)} | {"f": "", "keep": ""},
    )

    res = run("-r", *(f"t{i}" for i in range(20)), "f", threads=threads)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"keep"}


@pytest.mark.stress
def test_nested_operands(run: Runner, workdir: Path, threads: int) -> None:
    """Parent and child both named: tree must go, and nothing may crash."""
    fstree.build(workdir, {"a": {"b": {"c": {f"f{i}": "" for i in range(100)}}, "x": ""}})

    res = run("-r", "a", "a/b", "a/b/c", "a/x", threads=threads)

    with check:
        assert res.returncode in (0, 1), res
    with check:
        assert fstree.listing(workdir) == set()
    for line in res.errors:
        with check:
            assert os.strerror(errno.ENOENT) in line, res


@pytest.mark.stress
def test_child_operand_before_parent(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"a": {"b": {"c": ""}, "d": ""}})

    res = run("-r", "a/b", "a", threads=threads)

    with check:
        assert res.returncode in (0, 1), res
    with check:
        assert fstree.listing(workdir) == set()


@pytest.mark.stress
def test_same_directory_twice(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"d": {f"s{i}": {"f": ""} for i in range(200)}})

    res = run("-r", "d", "d", threads=threads)

    with check:
        assert res.returncode in (0, 1), res
    with check:
        assert fstree.listing(workdir) == set()


# --- Thread configuration ----------------------------------------------------


@pytest.mark.parametrize("value", ["0", "", "abc", "-1", "1.5", "99999999", "65536", "256"])
def test_bad_or_extreme_thread_env_still_removes(run: Runner, workdir: Path, value: str) -> None:
    fstree.build(workdir, {"t": {f"d{i}": {"f": ""} for i in range(50)}})

    res = run("-r", "t", threads=value)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()
