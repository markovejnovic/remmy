"""Removing non-directory operands: every inode type, names, and links."""

import os
from errno import EACCES, ENOENT, ENOTDIR
from os import strerror
from pathlib import Path

import fstree
import pytest
from fstree import Hardlink, Special, Symlink
from harness import Runner


@pytest.mark.parametrize("recursive", [False, True], ids=["plain", "-r"])
@pytest.mark.parametrize(
    "node",
    [
        pytest.param(b"", id="empty-file"),
        pytest.param(b"x" * 3_000_000, id="large-file"),
        pytest.param(Special.FIFO, id="fifo"),
        pytest.param(Special.SOCKET, id="socket"),
        pytest.param(Symlink("elsewhere"), id="dangling-symlink"),
        pytest.param(Symlink("victim"), id="self-symlink"),
    ],
)
def test_removes_each_inode_type(run: Runner, workdir: Path, node: object, recursive: bool) -> None:
    fstree.build(workdir, {"victim": node, "keep": "k"})
    res = run(*(["-r"] if recursive else []), "victim")
    assert (res.returncode, res.stdout, res.stderr, fstree.listing(workdir)) == (0, "", "", {"keep"}), res


def test_fifo_removal_does_not_block_on_open(run: Runner, workdir: Path) -> None:
    # Opening a reader-less FIFO would hang; the run timeout turns that into a failure.
    fstree.build(workdir, {"p": Special.FIFO, "d": {"p": Special.FIFO, "q": Special.FIFO}})
    res = run("-r", "p", "d")
    assert (res.returncode, fstree.listing(workdir)) == (0, set()), res


def test_read_only_file_is_removed_without_prompting(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"ro": "x"})
    os.chmod(workdir / "ro", 0o000)
    res = run("ro")
    assert (res.returncode, fstree.exists(workdir / "ro")) == (0, False), res


def test_symlink_to_file_removes_link_not_target(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"target": "precious", "link": Symlink("target")})
    res = run("link")
    assert (res.returncode, fstree.listing(workdir), (workdir / "target").read_text()) == (0, {"target"}, "precious")


@pytest.mark.parametrize("recursive", [False, True], ids=["plain", "-r"])
def test_symlink_to_directory_removes_only_the_link(run: Runner, workdir: Path, recursive: bool) -> None:
    fstree.build(workdir, {"real": {"a": "1", "sub": {"b": "2"}}, "link": Symlink("real")})
    before = fstree.snapshot_dir(workdir / "real")
    res = run(*(["-r"] if recursive else []), "link")
    assert (res.returncode, fstree.exists(workdir / "link")) == (0, False), res
    assert fstree.snapshot_dir(workdir / "real") == before


def test_symlink_with_absolute_target_outside_workdir(run: Runner, workdir: Path, tmp_path: Path) -> None:
    outside = fstree.build(tmp_path / "outside", {"x": "keep me", "d": {"y": "and me"}})
    before = fstree.snapshot_dir(outside)
    os.symlink(outside, workdir / "abs")
    res = run("-r", "abs")
    assert (res.returncode, fstree.exists(workdir / "abs")) == (0, False), res
    assert fstree.snapshot_dir(outside) == before


def test_hard_link_removal_leaves_other_names_intact(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"a": "shared", "b": Hardlink("a"), "d": {"c": Hardlink("../a")}})
    res = run("a", "d/c")
    b = workdir / "b"
    assert (res.returncode, fstree.listing(workdir), b.read_text(), b.lstat().st_nlink) == (0, {"b", "d"}, "shared", 1)


def test_many_operands_in_one_invocation(run: Runner, workdir: Path) -> None:
    names = [f"f{i:05}" for i in range(5000)]
    fstree.build(workdir, dict.fromkeys(names, "") | {"keep": "k"})
    res = run(*names)
    assert (res.returncode, fstree.listing(workdir)) == (0, {"keep"}), res


def test_absolute_and_relative_operands(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"a": "", "sub": {"b": "", "c": ""}})
    res = run(workdir / "a", "sub/b", "./sub/../sub/c")
    assert (res.returncode, fstree.listing(workdir)) == (0, {"sub"}), res


def test_cwd_is_respected(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"inner": {"same": "inner"}, "same": "outer"})
    res = run("same", cwd=workdir / "inner")
    listing, text = fstree.listing(workdir), (workdir / "same").read_text()
    assert (res.returncode, listing, text) == (0, {"inner", "same"}, "outer"), res


def test_missing_operand_reports_enoent(run: Runner, workdir: Path) -> None:
    res = run("ghost")
    assert (res.returncode, res.stdout, len(res.errors)) == (1, "", 1), res
    assert ": ghost: " in res.stderr and strerror(ENOENT) in res.stderr, res


def test_force_silences_only_missing_operands(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"a": "", "f": "", "keep": ""})
    res = run("-f", "ghost", "", "nodir/x", "a", "f/")
    assert (res.returncode, len(res.errors), fstree.listing(workdir)) == (1, 1, {"f", "keep"}), res
    assert res.stderr.endswith(f": f/: {strerror(ENOTDIR)}\n"), res


def test_failures_do_not_stop_later_operands(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"a": "", "b": "", "dir": {"x": ""}, "keep": ""})
    res = run("a", "ghost1", "dir", "b", "ghost2")
    assert (res.returncode, len(res.errors), fstree.listing(workdir)) == (1, 3, {"dir", "dir/x", "keep"}), res
    # Errors are reported in operand order.
    msgs = "\n".join(res.errors)
    assert -1 < msgs.find(": ghost1: ") < msgs.find(": dir: ") < msgs.find(": ghost2: "), res


def test_same_file_twice_removes_once_and_reports_once(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"f": ""})
    res = run("f", "f")
    assert (res.returncode, fstree.exists(workdir / "f"), len(res.errors)) == (1, False, 1), res
    assert strerror(ENOENT) in res.stderr, res


def test_trailing_slash_on_file_is_not_a_directory(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"f": "keep"})
    res = run("f/")
    assert (res.returncode, (workdir / "f").read_text()) == (1, "keep"), res
    assert strerror(ENOTDIR) in res.stderr, res


def test_file_in_unwritable_directory_is_reported(run: Runner, workdir: Path, is_root: bool) -> None:
    _check_locked_parent(run, workdir, is_root, 0o555)


def test_path_through_unsearchable_directory_is_reported(run: Runner, workdir: Path, is_root: bool) -> None:
    _check_locked_parent(run, workdir, is_root, 0o000)


def _check_locked_parent(run: Runner, workdir: Path, is_root: bool, mode: int) -> None:
    if is_root:
        pytest.skip("root bypasses directory permissions")
    fstree.build(workdir, {"locked": {"f": "x"}})
    with fstree.chmod(workdir / "locked", mode):
        res = run("locked/f")
    assert (res.returncode, fstree.exists(workdir / "locked/f")) == (1, True), res
    assert strerror(EACCES) in res.stderr, res
    if mode == 0o555:
        assert ": locked/f: " in res.stderr, res


def test_empty_string_operand_is_an_error(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"keep": ""})
    res = run("", "keep")
    assert (res.returncode, fstree.listing(workdir)) == (1, set()), res
