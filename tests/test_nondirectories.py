"""Removing non-directory operands: every inode type, names, and links."""

from __future__ import annotations

import errno
import os
from pathlib import Path

import fstree
import pytest
from fstree import Hardlink, Special, Symlink
from harness import Runner
from pytest_check import check


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

    with check:
        assert (res.returncode, res.stdout, res.stderr) == (0, "", ""), res
    with check:
        assert fstree.listing(workdir) == {"keep"}


def test_fifo_removal_does_not_block_on_open(run: Runner, workdir: Path) -> None:
    # Opening a reader-less FIFO would hang; the run timeout turns that into a failure.
    fstree.build(workdir, {"p": Special.FIFO, "d": {"p": Special.FIFO, "q": Special.FIFO}})

    res = run("-r", "p", "d")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


def test_read_only_file_is_removed_without_prompting(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"ro": "x"})
    os.chmod(workdir / "ro", 0o000)

    res = run("ro")

    with check:
        assert res.returncode == 0, res
    with check:
        assert not fstree.exists(workdir / "ro")


def test_symlink_to_file_removes_link_not_target(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"target": "precious", "link": Symlink("target")})

    res = run("link")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"target"}
    with check:
        assert (workdir / "target").read_text() == "precious"


@pytest.mark.parametrize("recursive", [False, True], ids=["plain", "-r"])
def test_symlink_to_directory_removes_only_the_link(run: Runner, workdir: Path, recursive: bool) -> None:
    fstree.build(workdir, {"real": {"a": "1", "sub": {"b": "2"}}, "link": Symlink("real")})
    before = fstree.snapshot_dir(workdir / "real")

    res = run(*(["-r"] if recursive else []), "link")

    with check:
        assert res.returncode == 0, res
    with check:
        assert not fstree.exists(workdir / "link")
    with check:
        assert fstree.snapshot_dir(workdir / "real") == before


def test_symlink_with_absolute_target_outside_workdir(run: Runner, workdir: Path, tmp_path: Path) -> None:
    outside = fstree.build(tmp_path / "outside", {"x": "keep me", "d": {"y": "and me"}})
    before = fstree.snapshot_dir(outside)
    os.symlink(outside, workdir / "abs")

    res = run("-r", "abs")

    with check:
        assert res.returncode == 0, res
    with check:
        assert not fstree.exists(workdir / "abs")
    with check:
        assert fstree.snapshot_dir(outside) == before


def test_hard_link_removal_leaves_other_names_intact(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"a": "shared", "b": Hardlink("a"), "d": {"c": Hardlink("../a")}})

    res = run("a", "d/c")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"b", "d"}
    with check:
        assert (workdir / "b").read_text() == "shared"
    with check:
        assert os.lstat(workdir / "b").st_nlink == 1


def test_many_operands_in_one_invocation(run: Runner, workdir: Path) -> None:
    names = [f"f{i:05}" for i in range(5000)]
    fstree.build(workdir, {n: "" for n in names} | {"keep": "k"})

    res = run(*names)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"keep"}


def test_absolute_and_relative_operands(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"a": "", "sub": {"b": "", "c": ""}})

    res = run(workdir / "a", "sub/b", "./sub/../sub/c")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"sub"}


def test_cwd_is_respected(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"inner": {"same": "inner"}, "same": "outer"})

    res = run("same", cwd=workdir / "inner")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"inner", "same"}
    with check:
        assert (workdir / "same").read_text() == "outer"


# --- Failures ----------------------------------------------------------------


def test_missing_operand_reports_enoent(run: Runner, workdir: Path) -> None:
    res = run("ghost")

    with check:
        assert res.returncode == 1, res
    with check:
        assert res.stdout == ""
    with check:
        assert len(res.errors) == 1
    with check:
        assert ": ghost: " in res.stderr
    with check:
        assert os.strerror(errno.ENOENT) in res.stderr


def test_force_silences_only_missing_operands(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"a": "", "f": "", "keep": ""})

    res = run("-f", "ghost", "", "nodir/x", "a", "f/")

    with check:
        assert res.returncode == 1, res
    with check:
        assert len(res.errors) == 1, res
    with check:
        assert res.stderr.endswith(f": f/: {os.strerror(errno.ENOTDIR)}\n"), res
    with check:
        assert fstree.listing(workdir) == {"f", "keep"}


def test_failures_do_not_stop_later_operands(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"a": "", "b": "", "dir": {"x": ""}, "keep": ""})

    res = run("a", "ghost1", "dir", "b", "ghost2")

    with check:
        assert res.returncode == 1, res
    with check:
        assert fstree.listing(workdir) == {"dir", "dir/x", "keep"}
    msgs = "\n".join(res.errors)
    with check:
        assert len(res.errors) == 3, res
    for n in (": ghost1: ", ": ghost2: ", ": dir: "):
        with check:
            assert n in msgs
    # Errors are reported in operand order.
    with check:
        assert msgs.index(": ghost1: ") < msgs.index(": dir: ") < msgs.index(": ghost2: ")


def test_same_file_twice_removes_once_and_reports_once(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"f": ""})

    res = run("f", "f")

    with check:
        assert res.returncode == 1, res
    with check:
        assert not fstree.exists(workdir / "f")
    with check:
        assert len(res.errors) == 1, res
    with check:
        assert os.strerror(errno.ENOENT) in res.stderr


def test_trailing_slash_on_file_is_not_a_directory(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"f": "keep"})

    res = run("f/")

    with check:
        assert res.returncode == 1, res
    with check:
        assert os.strerror(errno.ENOTDIR) in res.stderr
    with check:
        assert (workdir / "f").read_text() == "keep"


def test_file_in_unwritable_directory_is_reported(run: Runner, workdir: Path, is_root: bool) -> None:
    if is_root:
        pytest.skip("root bypasses directory write permission")
    fstree.build(workdir, {"locked": {"f": "x"}})

    with fstree.chmod(workdir / "locked", 0o555):
        res = run("locked/f")

    with check:
        assert res.returncode == 1, res
    with check:
        assert ": locked/f: " in res.stderr
    with check:
        assert os.strerror(errno.EACCES) in res.stderr
    with check:
        assert fstree.exists(workdir / "locked/f")


def test_path_through_unsearchable_directory_is_reported(run: Runner, workdir: Path, is_root: bool) -> None:
    if is_root:
        pytest.skip("root bypasses directory search permission")
    fstree.build(workdir, {"locked": {"f": "x"}})

    with fstree.chmod(workdir / "locked", 0o000):
        res = run("locked/f")

    with check:
        assert res.returncode == 1, res
    with check:
        assert os.strerror(errno.EACCES) in res.stderr
    with check:
        assert fstree.exists(workdir / "locked/f")


def test_empty_string_operand_is_an_error(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"keep": ""})

    res = run("", "keep")

    with check:
        assert res.returncode == 1, res
    with check:
        assert fstree.listing(workdir) == set()
