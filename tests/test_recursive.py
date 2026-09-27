"""Recursive removal: tree shapes, thread counts, symlink containment, operands."""

import os
import subprocess
from collections.abc import Collection
from errno import ENOENT
from pathlib import Path

import fstree
import pytest
from fstree import Hardlink, Special, Symlink
from harness import Runner


def _gone(run: Runner, workdir: Path, spec: fstree.Spec, *args: str, left: Collection[str] = frozenset(), **kw) -> None:
    """Build ``spec``, run, and expect success with only ``left`` remaining."""
    fstree.build(workdir, spec)
    res = run(*args, **kw)
    assert (res.returncode, fstree.listing(workdir)) == (0, set(left)), res


def test_directory_without_recursive_is_refused_and_untouched(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"d": {"f": "x", "e": {}}, "empty": {}})
    before = fstree.snapshot_dir(workdir)
    res = run("d", "empty")
    assert (res.returncode, len(res.errors), fstree.snapshot_dir(workdir) == before) == (1, 2, True), res
    # BSD rm's own text, not strerror(EISDIR)'s "Is a directory".
    assert ": d: is a directory\n" in res.stderr and ": empty: is a directory\n" in res.stderr, res


def test_empty_directory(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"d": {}, "keep": {}})
    res = run("-r", "d", threads=threads)
    assert (res.returncode, res.stdout, res.stderr, fstree.listing(workdir)) == (0, "", "", {"keep"}), res


def test_mixed_inode_types_throughout_tree(run: Runner, workdir: Path, threads: int) -> None:
    t = {
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
    }
    fstree.build(workdir, {"t": t, "keep": "k"})
    res = run("-r", "t", threads=threads)
    assert (res.returncode, res.stdout, res.stderr, fstree.listing(workdir)) == (0, "", "", {"keep"}), res


def test_wide_directory(run: Runner, workdir: Path, threads: int) -> None:
    _gone(run, workdir, {"w": {f"f{i}": "" for i in range(20_000)}}, "-r", "w", threads=threads)


def test_many_sibling_directories(run: Runner, workdir: Path, threads: int) -> None:
    _gone(run, workdir, {"w": {f"d{i}": {"f": "", "g": {"h": ""}} for i in range(3_000)}}, "-r", "w", threads=threads)


def test_bushy_tree(run: Runner, workdir: Path, threads: int) -> None:
    def level(depth: int) -> fstree.Spec:
        if depth == 0:
            return {f"f{i}": "x" for i in range(4)}
        return {f"d{i}": level(depth - 1) for i in range(5)} | {"f": "x"}

    _gone(run, workdir, {"t": level(5)}, "-r", "t", threads=threads)  # 3906 dirs


def test_deep_chain_within_path_max(run: Runner, workdir: Path, threads: int) -> None:
    assert fstree.deep_chain(workdir / "deep", depth=400, name="d") < 1024
    _gone(run, workdir, {}, "-r", "deep", threads=threads)


def test_symlinks_inside_tree_are_never_followed(run: Runner, workdir: Path, tmp_path: Path, threads: int) -> None:
    outside = fstree.build(tmp_path / "outside", {"file": "keep", "dir": {"nested": {"deep": "keep"}}})
    t = {
        "abs_dir": Symlink(str(outside)),
        "abs_file": Symlink(str(outside / "file")),
        "rel_sibling": Symlink("../sibling"),
        "parent": Symlink(".."),
        "root": Symlink("/"),
        "sub": {"deeper_escape": Symlink("../../sibling")},
    }
    fstree.build(workdir, {"sibling": {"s": "keep"}, "t": t})
    before = fstree.snapshot_dir(outside), fstree.snapshot_dir(workdir / "sibling")
    res = run("-r", "t", threads=threads)
    assert (res.returncode, fstree.exists(workdir / "t")) == (0, False), res
    assert (fstree.snapshot_dir(outside), fstree.snapshot_dir(workdir / "sibling")) == before


def test_hard_links_leaving_the_tree_survive(run: Runner, workdir: Path) -> None:
    spec = {"outside": "shared", "t": {"a": Hardlink("../outside"), "d": {"b": Hardlink("../../outside")}}}
    _gone(run, workdir, spec, "-r", "t", left={"outside"})
    out = workdir / "outside"
    assert (out.read_text(), out.lstat().st_nlink) == ("shared", 1)


def test_neighbours_with_shared_prefix_are_untouched(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"d": {"x": ""}, "d2": {"x": ""}, "d.bak": {"x": ""}, "d ": {"x": ""}, "dd": ""})
    before = {k: v for k, v in fstree.snapshot_dir(workdir).items() if k.split("/")[0] not in ("", "d")}
    res = run("-r", "d")
    after = fstree.snapshot_dir(workdir)
    del after[""]  # workdir's own link count drops with d gone
    assert (res.returncode, after) == (0, before), res


@pytest.mark.parametrize("spelling", ["d", "d/", "d//", "./d", "./d/", "sub/../d", "ABS", "ABS/"])
def test_operand_spellings(run: Runner, workdir: Path, spelling: str) -> None:
    operand = spelling.replace("ABS", str(workdir / "d"))
    _gone(run, workdir, {"d": {"x": {"y": ""}, "z": ""}, "sub": {}}, "-r", operand, left={"sub"})


def test_operand_in_deep_relative_cwd(run: Runner, workdir: Path, threads: int) -> None:
    spec = {"a": {"b": {"c": {"t": {"x": {"y": ""}}}}}}
    _gone(run, workdir, spec, "-r", "t", cwd=workdir / "a/b/c", threads=threads, left={"a", "a/b", "a/b/c"})


@pytest.mark.parametrize("recursive", [True, False], ids=["r", "no-r"])
@pytest.mark.parametrize("operand", [".", "..", "sub/.", "sub/..", "./", "sub/..//"])
def test_dot_and_dotdot_operands_are_refused(run: Runner, workdir: Path, operand: str, recursive: bool) -> None:
    """POSIX: an operand whose last component is `.` or `..` gets a diagnostic and nothing else."""
    fstree.build(workdir, {"keep": "x", "a": {"f": "x", "b": {"g": "x", "sub": {"h": "x"}}}})
    before = fstree.snapshot_dir(workdir)
    res = run(*(["-r"] if recursive else []), operand, cwd=workdir / "a" / "b")
    assert (res.returncode, bool(res.stderr), fstree.snapshot_dir(workdir) == before) == (1, True, True), res


@pytest.mark.parametrize("via", ["symlink", "argv0"])
@pytest.mark.parametrize("args", [["-r", "."], ["-rf", "."], ["-r", ".."], ["-rf", "sub/.."], ["-R", "./"]])
def test_unlink_mode_options_never_walk_dot_operands(
    remmy_bin: Path, tmp_path: Path, workdir: Path, args: list[str], via: str
) -> None:
    """Invoked as unlink(1), rm parses no options: any of these is two operands, so a usage error that removes nothing."""
    fstree.build(workdir, {"keep": "x", "a": {"f": "x", "b": {"g": "x", "sub": {"h": "x"}}}})
    before = fstree.snapshot_dir(workdir)
    link = tmp_path / "bin" / "unlink"
    if via == "symlink":
        link.parent.mkdir()
        link.symlink_to(remmy_bin)
    argv0, exe = (link, None) if via == "symlink" else ("unlink", remmy_bin)
    res = subprocess.run([argv0, *args], executable=exe, cwd=workdir / "a/b", capture_output=True, check=False)
    assert (res.returncode, res.stderr[:7], fstree.snapshot_dir(workdir) == before) == (64, b"usage: ", True), res


def test_other_operands_are_removed_after_a_refused_dot(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"keep": "x", "gone": {"f": "x"}, "also": "x"})
    res = run("-r", ".", "gone", "also")
    assert (res.returncode, fstree.listing(workdir)) == (1, {"keep"}), res


def test_parent_relative_operand(run: Runner, workdir: Path) -> None:
    _gone(run, workdir, {"here": {}, "there": {"x": {"y": ""}}}, "-r", "../there", cwd=workdir / "here", left={"here"})


def test_multiple_directory_operands(run: Runner, workdir: Path, threads: int) -> None:
    spec = {f"t{i}": {"a": {"b": ""}, "c": ""} for i in range(20)} | {"f": "", "keep": ""}
    _gone(run, workdir, spec, "-r", *(f"t{i}" for i in range(20)), "f", threads=threads, left={"keep"})


def _racy(run: Runner, workdir: Path, spec: fstree.Spec, *args: str, threads: int) -> list[str]:
    """Overlapping operands: the tree must go, and a failure may only be a lost race."""
    fstree.build(workdir, spec)
    res = run("-r", *args, threads=threads)
    assert (res.returncode in (0, 1), fstree.listing(workdir)) == (True, set()), res
    return res.errors


@pytest.mark.stress
def test_nested_operands(run: Runner, workdir: Path, threads: int) -> None:
    spec = {"a": {"b": {"c": {f"f{i}": "" for i in range(100)}}, "x": ""}}
    errors = _racy(run, workdir, spec, "a", "a/b", "a/b/c", "a/x", threads=threads)
    assert all(os.strerror(ENOENT) in line for line in errors), errors


@pytest.mark.stress
def test_child_operand_before_parent(run: Runner, workdir: Path, threads: int) -> None:
    _racy(run, workdir, {"a": {"b": {"c": ""}, "d": ""}}, "a/b", "a", threads=threads)


@pytest.mark.stress
def test_same_directory_twice(run: Runner, workdir: Path, threads: int) -> None:
    _racy(run, workdir, {"d": {f"s{i}": {"f": ""} for i in range(200)}}, "d", "d", threads=threads)


@pytest.mark.parametrize("value", ["0", "", "abc", "-1", "1.5", "99999999", "65536", "256"])
def test_bad_or_extreme_thread_env_still_removes(run: Runner, workdir: Path, value: str) -> None:
    _gone(run, workdir, {"t": {f"d{i}": {"f": ""} for i in range(50)}}, "-r", "t", threads=value)
