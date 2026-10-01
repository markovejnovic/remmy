"""Syscall failures that permissions cannot produce.

Under a failing syscall remmy removes everything the failure does not block, names the failing path, exits 1, and
terminates. Transient descriptor exhaustion is the exception: it must be retried.
"""

import errno
import os
from functools import partial
from pathlib import Path

import fstree
import pytest
import strategies
from faults import Injection, run_with_faults
from fstree import Special, Symlink
from hypothesis import assume, event, given, target
from hypothesis import strategies as st
from pytest_check import check

EIO, EMFILE = errno.EIO, errno.EMFILE
TREE = {"a": {"victim": "v", "b": {"g": ""}, "sib": ""}, "c": {"h": "", "l": Symlink(".."), "p": Special.FIFO}, "x": ""}
_C_ESCAPES = {"\a": "\\a", "\b": "\\b", "\v": "\\v", "\f": "\\f", "\r": "\\r"}


@pytest.fixture
def frun(remmy_bin: Path, faultlib: Path, workdir: Path):
    return partial(run_with_faults, remmy_bin, faultlib, cwd=workdir)


@pytest.fixture
def tree(workdir: Path) -> None:
    fstree.build(workdir / "t", TREE)


@pytest.fixture(scope="module")
def base(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return tmp_path_factory.mktemp("faults")


def _rel(p: Path, root: Path) -> str:
    return os.path.relpath(p, os.path.realpath(root))


def _shown(rel: str) -> str:
    """``rel`` as rm's diagnostics name it: C0 control bytes but TAB and LF escaped C style."""
    return "".join(c if c in "\t\n" or ord(c) >= 0x20 else _C_ESCAPES.get(c, f"\\{ord(c):03o}") for c in rel)


def _fails_with(res, err: int, *paths: str) -> None:
    check.equal(res.returncode, 1, res)
    check.is_true(all(p in res.stderr for p in paths) and os.strerror(err) in res.stderr, res)


# Hard failures are reported, contained, and final.


def test_failed_unlink_is_reported_by_name(frun, tree, workdir: Path, threads: int) -> None:
    res, hit = frun("-r", "t", threads=threads, faults=[("unlinkat", EIO, "name=victim")])
    assert [i.fn for i in hit] == ["unlinkat"]
    _fails_with(res, EIO, "t/a/victim")
    check.equal(fstree.listing(workdir), {"t", "t/a", "t/a/victim"})


def test_failed_rmdir_is_reported_by_name(frun, tree, workdir: Path, threads: int) -> None:
    res, hit = frun("-r", "t", threads=threads, faults=[("rmdir", errno.EBUSY, "name=b")])
    assert hit, "rmdir of t/a/b was never attempted"
    _fails_with(res, errno.EBUSY, "t/a/b")
    check.equal(fstree.listing(workdir), {"t", "t/a", "t/a/b"})


@pytest.mark.parametrize("err", [EIO, errno.EACCES, errno.ELOOP])
def test_failed_subdirectory_open_skips_only_that_subtree(frun, tree, workdir: Path, err: int) -> None:
    res, hit = frun("-r", "t", faults=[("openat", err, "name=a")])
    assert hit
    _fails_with(res, err, "t/a")
    check.equal(fstree.listing(workdir), {"t", "t/a", "t/a/victim", "t/a/b", "t/a/b/g", "t/a/sib"})


def test_vanished_subdirectory_is_passed_over(frun, tree, workdir: Path) -> None:
    """ENOENT on a listed name means it is gone, as fts takes it: no word about it, only its parent's rmdir fails."""
    res, hit = frun("-r", "t", faults=[("openat", errno.ENOENT, "name=a")])
    assert hit
    check.equal(res.returncode, 1, res)
    check.equal(res.errors, [f"remmy: t: {os.strerror(errno.ENOTEMPTY)}"], res)
    check.equal(fstree.listing(workdir), {"t", "t/a", "t/a/victim", "t/a/b", "t/a/b/g", "t/a/sib"})


def test_failed_operand_open(frun, tree, workdir: Path) -> None:
    fstree.build(workdir, {"other": {"f": ""}})
    res, hit = frun("-r", "t", "other", faults=[("open", EIO, "name=t")])
    assert hit
    _fails_with(res, EIO, f": t: {os.strerror(EIO)}\n")
    check.is_false(fstree.exists(workdir / "other"))
    check.is_true(fstree.exists(workdir / "t/a/victim"))


def test_failed_operand_lstat(frun, workdir: Path) -> None:
    fstree.build(workdir, {"f": "", "g": ""})
    res, hit = frun("f", "g", faults=[("lstat", EIO, "name=f")])
    assert hit
    _fails_with(res, EIO, ": f: ")
    check.equal(fstree.listing(workdir), {"f"})


def test_failed_top_level_unlink(frun, workdir: Path) -> None:
    fstree.build(workdir, {"f": "", "g": ""})
    res, hit = frun("f", "g", faults=[("unlink", errno.EPERM, "name=f")])
    assert hit
    _fails_with(res, errno.EPERM, f": f: {os.strerror(errno.EPERM)}\n")
    check.equal(fstree.listing(workdir), {"f"})


@pytest.mark.parametrize("selector", ["nth=1", "nth=2", "all"])
def test_failed_directory_read_is_reported(frun, tree, workdir: Path, selector: str) -> None:
    res, hit = frun("-r", "t", faults=[("getdirentries", EIO, selector)])
    assert hit
    _fails_with(res, EIO, *(_rel(i.path, workdir) for i in hit))


# Transient descriptor exhaustion must be retried.


@pytest.mark.stress
@pytest.mark.parametrize("err", [EMFILE, errno.ENFILE])
@pytest.mark.parametrize("selector", ["nth=1", "nth=3", "name=b"])
def test_transient_fd_exhaustion_is_retried(frun, tree, workdir: Path, threads: int, err: int, selector: str) -> None:
    res, hit = frun("-r", "t", threads=threads, faults=[("openat", err, selector)])
    assert hit
    # Even an openat that always fails this way is recoverable: remmy parks the directory and later opens it by path.
    check.equal((res.returncode, res.stderr), (0, ""), res)
    check.equal(fstree.listing(workdir), set())


@pytest.mark.stress
def test_permanent_openat_exhaustion_falls_back_to_paths(frun, tree, workdir: Path, threads: int) -> None:
    res, hit = frun("-r", "t", threads=threads, faults=[("openat", EMFILE, "all")])
    assert hit
    check.equal((res.returncode, res.stderr), (0, ""), res)
    check.equal(fstree.listing(workdir), set())


@pytest.mark.stress
@pytest.mark.parametrize("fns", [("open",), ("open", "openat")], ids="+".join)
def test_permanent_exhaustion_terminates_with_an_error(frun, tree, threads: int, fns: tuple[str, ...]) -> None:
    """With no way to get a descriptor, remmy must give up, not spin."""
    res, hit = frun("-r", "t", threads=threads, faults=[(fn, EMFILE, "all") for fn in fns])
    assert hit
    _fails_with(res, EMFILE)


# Filesystems that do not report entry types.


def test_unknown_entry_types_fall_back_to_stat(frun, tree, workdir: Path, threads: int) -> None:
    res, hit = frun("-r", "t", threads=threads, dt_unknown=True)
    assert hit == []
    check.equal((res.returncode, res.stderr), (0, ""), res)
    check.equal(fstree.listing(workdir), set())


def test_failed_fallback_stat_is_reported(frun, tree, workdir: Path) -> None:
    res, hit = frun("-r", "t", dt_unknown=True, faults=[("fstatat", EIO, "name=a")])
    assert hit
    _fails_with(res, EIO, "t/a")
    # Whatever remmy does with an entry it cannot classify, the rest goes.
    check.is_false(fstree.exists(workdir / "t/c"))
    check.is_false(fstree.exists(workdir / "t/x"))


def _allowed_survivors(inj: Injection, root: Path) -> set[str]:
    """What a single failure at ``inj`` may legitimately leave behind: it, its ancestors, and, if the directory could
    not be (fully) read, its contents."""
    rel = _rel(inj.path, root)
    parts = rel.split("/")
    blocked = {"/".join(parts[:i]) for i in range(1, len(parts) + 1)}
    if inj.fn in ("openat", "getdirentries"):
        blocked |= {p for p in fstree.listing(root) if p.startswith(rel + "/")}
    return blocked


@given(
    spec=strategies.trees.filter(bool),
    fn=st.sampled_from(["openat", "unlinkat", "rmdir", "getdirentries"]),
    err=st.sampled_from([EIO, errno.EACCES, errno.EPERM, errno.EBUSY, errno.EROFS]),
    nth=st.integers(1, 40),
    threads=st.integers(1, 8),
    dt_unknown=st.booleans(),
)
def test_any_single_failure_is_contained_and_reported(
    remmy_bin: Path, faultlib: Path, base: Path, spec, fn: str, err: int, nth: int, threads: int, dt_unknown: bool
) -> None:
    strategies.describe_tree(spec)
    event(f"dt_unknown: {dt_unknown}")
    with fstree.scratch(base) as d:
        fstree.build(d / "t", spec)
        res, hit = run_with_faults(
            remmy_bin,
            faultlib,
            "-r",
            "t",
            cwd=d,
            threads=threads,
            dt_unknown=dt_unknown,
            faults=[(fn, err, f"nth={nth}")],
        )
        event(f"fault fired: {bool(hit)}")
        if not hit:
            assert (res.returncode, res.stderr) == (0, ""), res
            assert not fstree.exists(d / "t")
            return

        (inj,) = hit
        rel = _rel(inj.path, d)
        assume(rel == "t" or rel.startswith("t/"))  # ignore libc-internal calls
        event(f"failed: {fn}")
        target(float(rel.count("/")), label="depth of injected failure")
        survivors = fstree.listing(d)
        assert res.returncode == 1, res
        assert _shown(rel) in res.stderr, f"failing path {rel!r} not named\n{res}"
        extra = survivors - _allowed_survivors(inj, d)
        assert not extra, f"collateral survivors: {sorted(extra)}\n{res}"
