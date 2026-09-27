"""Recursive removal that cannot fully succeed.

remmy removes everything it is allowed to, keeps every ancestor of anything it could not remove, names the offending
path on stderr, exits 1, and still terminates.
"""

import errno
import os
from pathlib import Path

import fstree
import pytest
from harness import Runner
from pytest_check import check

DENIED, NOTEMPTY = os.strerror(errno.EACCES), os.strerror(errno.ENOTEMPTY)


@pytest.fixture(autouse=True)
def _not_root(is_root: bool) -> None:
    if is_root:
        pytest.skip("root bypasses the permission checks these tests rely on")


def _lock(workdir: Path, *paths: str) -> None:
    for p in paths:
        os.chmod(workdir / p, 0o000)


def test_unreadable_subdirectory(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"t": {"ok1": {"x": ""}, "locked": {"secret": "s", "inner": {}}, "ok2": "", "f": ""}})
    with fstree.chmod(workdir / "t/locked", 0o000):
        res = run("-r", "t", threads=threads)
    check.equal(res.returncode, 1, res)
    check.equal(fstree.listing(workdir), {"t", "t/locked", "t/locked/secret", "t/locked/inner"})
    check.is_in("t/locked", res.stderr)
    check.is_in(DENIED, res.stderr)
    # One complaint for the locked dir, one for its parent that could not be emptied.
    check.is_true(any(e.endswith(f": t: {NOTEMPTY}") for e in res.errors), res)


def test_unwritable_subdirectory_keeps_its_files(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"t": {"frozen": {"a": "", "b": "", "sub": {"c": ""}}, "gone": {"x": ""}}})
    with fstree.chmod(workdir / "t/frozen", 0o555):
        res = run("-r", "t", threads=threads)
    check.equal(res.returncode, 1, res)
    left = fstree.listing(workdir)
    check.is_true({"t", "t/frozen", "t/frozen/a", "t/frozen/b"} <= left, left)
    check.is_false(any(p.startswith("t/gone") for p in left), left)
    check.is_in("t/frozen", res.stderr)


def test_failure_deep_in_tree_preserves_exact_ancestor_chain(run: Runner, workdir: Path, threads: int) -> None:
    a = {"b": {"c": {"locked": {"k": ""}, "junk": ""}, "junk": {"j": ""}}, "junk": ""}
    fstree.build(workdir, {"t": {"a": a, "z": {f"n{i}": {"f": ""} for i in range(200)}}})
    with fstree.chmod(workdir / "t/a/b/c/locked", 0o000):
        res = run("-r", "t", threads=threads)
    check.equal(res.returncode, 1, res)
    check.equal(fstree.listing(workdir), {"t", "t/a", "t/a/b", "t/a/b/c", "t/a/b/c/locked", "t/a/b/c/locked/k"})


def test_many_scattered_failures_all_reported(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"t": {f"d{i}": {"locked": {"x": ""}, "free": ""} for i in range(40)}})
    _lock(workdir, *(f"t/d{i}/locked" for i in range(40)))
    res = run("-r", "t", threads=threads)
    check.equal(res.returncode, 1, res)
    for i in range(40):
        check.is_in(f"t/d{i}/locked", res.stderr, res)
        check.is_false(fstree.exists(workdir / f"t/d{i}/free"))


def test_unremovable_operand_does_not_affect_other_operands(run: Runner, workdir: Path, threads: int) -> None:
    fstree.build(workdir, {"bad": {"locked": {"x": ""}}, "good1": {"a": {"b": ""}}, "good2": ""})
    with fstree.chmod(workdir / "bad/locked", 0o000):
        res = run("-r", "good1", "bad", "good2", threads=threads)
    check.equal(res.returncode, 1, res)
    check.equal(fstree.listing(workdir), {"bad", "bad/locked", "bad/locked/x"})


def test_unreadable_operand_itself(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"locked": {"x": ""}, "keep": ""})
    with fstree.chmod(workdir / "locked", 0o000):
        res = run("-r", "locked")
    check.equal(res.returncode, 1, res)
    check.is_in(": locked: ", res.stderr)
    check.equal(fstree.listing(workdir), {"locked", "locked/x", "keep"})


def test_empty_unreadable_directory(run: Runner, workdir: Path) -> None:
    """Nothing to read inside, but rmdir only needs the parent writable."""
    fstree.build(workdir, {"t": {"sealed": {}}})
    _lock(workdir, "t/sealed")
    res = run("-r", "t")
    # rm(1) removes such a directory; remmy must not leave it behind silently.
    if res.returncode == 0:
        check.equal(fstree.listing(workdir), set())
    else:
        check.is_in("t/sealed", res.stderr)
        check.is_true(fstree.exists(workdir / "t/sealed"))


def test_operand_under_unwritable_parent(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"p": {"t": {"a": "", "b": {"c": ""}}}})
    with fstree.chmod(workdir / "p", 0o555):
        res = run("-r", "p/t")
    check.equal(res.returncode, 1, res)
    check.is_in("p/t", res.stderr)
    check.equal(fstree.listing(workdir), {"p", "p/t"})


@pytest.mark.parametrize("contents", ["empty", "full"])
def test_unreadable_subdirectory_opened_late_under_fd_pressure(
    run: Runner, workdir: Path, threads: int, contents: str
) -> None:
    """A subdirectory parked for want of a descriptor and then refused is only reported.

    Like rm, remmy names it once and leaves it (and so its parent) in place: it must not rmdir it, which adds a
    'Directory not empty' for it or, when it is empty, removes it and its parent under plain -r.
    """
    full = contents == "full"
    locked = [f"t/L{k}" for k in range(12)]
    fstree.build(workdir, {"t": {f"d{i}": {"e": {"f": ""}} for i in range(300)}})
    fstree.build(workdir, {p: {"x": ""} if full else {} for p in locked})
    _lock(workdir, *locked)
    res = run("-r", "t", threads=threads, fd_limit=16)
    check.equal(res.returncode, 1, res)
    check.equal(sorted(res.errors), sorted([f"remmy: {p}: {DENIED}" for p in locked] + [f"remmy: t: {NOTEMPTY}"]), res)
    for p in locked:
        if fstree.exists(workdir / p):
            os.chmod(workdir / p, 0o700)
    check.equal(fstree.listing(workdir), {"t", *locked, *(f"{p}/x" for p in locked if full)})


def test_force_removes_unreadable_empty_directories(run: Runner, workdir: Path, threads: int) -> None:
    """Like rm -rf, which still rmdirs a directory it cannot read and says nothing when that works."""
    fstree.build(workdir, {"t": {"sealed": {}, "deep": {"sealed": {}}, "x": ""}, "op": {}})
    _lock(workdir, "t/sealed", "t/deep/sealed", "op")
    res = run("-rf", "t", "op", threads=threads)
    check.equal((res.returncode, res.stderr), (0, ""), res)
    check.equal(fstree.listing(workdir), set())


@pytest.mark.parametrize("fd_limit", [None, 16])
def test_force_keeps_unreadable_full_directories(
    run: Runner, workdir: Path, threads: int, fd_limit: int | None
) -> None:
    """-f removes the unreadable empty ones, also opened late under fd pressure; full ones stay, reported once."""
    empty, full = [f"t/E{k}" for k in range(12)], [f"t/F{k}" for k in range(12)]
    fstree.build(workdir, {"t": {f"d{i}": {"e": {"f": ""}} for i in range(300)}})
    fstree.build(workdir, {**{p: {} for p in empty}, **{p: {"x": ""} for p in full}})
    _lock(workdir, *empty, *full)
    res = run("-rf", "t", threads=threads, fd_limit=fd_limit)
    check.equal(res.returncode, 1, res)
    check.equal(sorted(res.errors), sorted([f"remmy: {p}: {DENIED}" for p in full] + [f"remmy: t: {NOTEMPTY}"]), res)
    for p in full:
        os.chmod(workdir / p, 0o700)
    check.equal(fstree.listing(workdir), {"t", *full, *(f"{p}/x" for p in full)})
