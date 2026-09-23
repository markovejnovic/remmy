"""Syscall failures that permissions cannot produce.

The contract under a failing syscall: remmy removes everything the failure
does not block, names the failing path on stderr, exits 1, and terminates.
Transient descriptor exhaustion is the exception: it must be retried.
"""

from __future__ import annotations

import errno
import os
from pathlib import Path

import pytest
from hypothesis import assume, event, given, target
from hypothesis import strategies as st
from pytest_check import check

import fstree
import strategies
from faults import Fault, Injection, run_with_faults
from fstree import Special, Symlink
from known_bugs import SWALLOWED_UNLINK_ERROR, UNCLASSIFIED_ENTRY_TREATED_AS_FILE


def _tree(root: Path) -> Path:
    return fstree.build(
        root / "t",
        {
            "a": {"victim": "v", "b": {"g": ""}, "sib": ""},
            "c": {"h": "", "l": Symlink(".."), "p": Special.FIFO},
            "x": "",
        },
    )


def _ancestors(rel: str) -> set[str]:
    parts = rel.split("/")
    return {"/".join(parts[:i]) for i in range(1, len(parts))}


def _rel(p: Path, root: Path) -> str:
    return os.path.relpath(p, os.path.realpath(root))


# --- Hard failures are reported, contained, and final ------------------------


@SWALLOWED_UNLINK_ERROR
def test_failed_unlink_is_reported_by_name(
    remmy_bin: Path, faultlib: Path, workdir: Path, threads: int
) -> None:
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir, threads=threads,
        faults=[Fault("unlinkat", errno.EIO, "name=victim")],
    )

    assert [i.fn for i in hit] == ["unlinkat"]
    with check:
        assert res.returncode == 1, res
    with check:
        assert "t/a/victim" in res.stderr, res
    with check:
        assert os.strerror(errno.EIO) in res.stderr, res
    with check:
        assert fstree.listing(workdir) == {"t", "t/a", "t/a/victim"}


def test_failed_rmdir_is_reported_by_name(
    remmy_bin: Path, faultlib: Path, workdir: Path, threads: int
) -> None:
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir, threads=threads,
        faults=[Fault("rmdir", errno.EBUSY, "name=b")],
    )

    assert hit, "rmdir of t/a/b was never attempted"
    with check:
        assert res.returncode == 1, res
    with check:
        assert "t/a/b" in res.stderr and os.strerror(errno.EBUSY) in res.stderr, res
    with check:
        assert fstree.listing(workdir) == {"t", "t/a", "t/a/b"}


@pytest.mark.parametrize("err", [errno.EIO, errno.EACCES, errno.ENOENT, errno.ELOOP])
def test_failed_subdirectory_open_skips_only_that_subtree(
    remmy_bin: Path, faultlib: Path, workdir: Path, err: int
) -> None:
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir,
        faults=[Fault("openat", err, "name=a")],
    )

    assert hit
    with check:
        assert res.returncode == 1, res
    with check:
        assert "t/a" in res.stderr and os.strerror(err) in res.stderr, res
    with check:
        assert fstree.listing(workdir) == {"t", "t/a", "t/a/victim", "t/a/b", "t/a/b/g", "t/a/sib"}


def test_failed_operand_open(remmy_bin: Path, faultlib: Path, workdir: Path) -> None:
    _tree(workdir)
    fstree.build(workdir, {"other": {"f": ""}})

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t", "other"], cwd=workdir,
        faults=[Fault("open", errno.EIO, "name=t")],
    )

    assert hit
    with check:
        assert res.returncode == 1, res
    with check:
        assert "'t'" in res.stderr and os.strerror(errno.EIO) in res.stderr, res
    with check:
        assert not fstree.exists(workdir / "other")
    with check:
        assert fstree.exists(workdir / "t/a/victim")


def test_failed_operand_lstat(remmy_bin: Path, faultlib: Path, workdir: Path) -> None:
    fstree.build(workdir, {"f": "", "g": ""})

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["f", "g"], cwd=workdir,
        faults=[Fault("lstat", errno.EIO, "name=f")],
    )

    assert hit
    with check:
        assert res.returncode == 1, res
    with check:
        assert "'f'" in res.stderr, res
    with check:
        assert fstree.listing(workdir) == {"f"}


def test_failed_top_level_unlink(remmy_bin: Path, faultlib: Path, workdir: Path) -> None:
    fstree.build(workdir, {"f": "", "g": ""})

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["f", "g"], cwd=workdir,
        faults=[Fault("unlink", errno.EPERM, "name=f")],
    )

    assert hit
    with check:
        assert res.returncode == 1, res
    with check:
        assert "'f'" in res.stderr and os.strerror(errno.EPERM) in res.stderr, res
    with check:
        assert fstree.listing(workdir) == {"f"}


@pytest.mark.parametrize("selector", ["nth=1", "nth=2", "all"])
def test_failed_directory_read_is_reported(
    remmy_bin: Path, faultlib: Path, workdir: Path, selector: str
) -> None:
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir,
        faults=[Fault("getdirentries", errno.EIO, selector)],
    )

    assert hit
    with check:
        assert res.returncode == 1, res
    for i in hit:
        with check:
            assert _rel(i.path, workdir) in res.stderr, res
    with check:
        assert os.strerror(errno.EIO) in res.stderr, res


# --- Transient descriptor exhaustion must be retried -------------------------


@pytest.mark.stress
@pytest.mark.parametrize("err", [errno.EMFILE, errno.ENFILE])
@pytest.mark.parametrize("selector", ["nth=1", "nth=3", "name=b"])
def test_transient_fd_exhaustion_is_retried(
    remmy_bin: Path, faultlib: Path, workdir: Path, threads: int, err: int, selector: str
) -> None:
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir, threads=threads,
        faults=[Fault("openat", err, selector)],
    )

    assert hit
    # Even an openat that always fails this way is recoverable: remmy parks the
    # directory and later opens it by path instead.
    with check:
        assert (res.returncode, res.stderr) == (0, ""), res
    with check:
        assert fstree.listing(workdir) == set()


@pytest.mark.stress
def test_permanent_openat_exhaustion_falls_back_to_paths(
    remmy_bin: Path, faultlib: Path, workdir: Path, threads: int
) -> None:
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir, threads=threads,
        faults=[Fault("openat", errno.EMFILE, "all")],
    )

    assert hit
    with check:
        assert (res.returncode, res.stderr) == (0, ""), res
    with check:
        assert fstree.listing(workdir) == set()


@pytest.mark.stress
@pytest.mark.parametrize("fns", [("open",), ("open", "openat")], ids="+".join)
def test_permanent_exhaustion_terminates_with_an_error(
    remmy_bin: Path, faultlib: Path, workdir: Path, threads: int, fns: tuple[str, ...]
) -> None:
    """With no way to get a descriptor, remmy must give up, not spin."""
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir, threads=threads,
        faults=[Fault(fn, errno.EMFILE, "all") for fn in fns],
    )

    assert hit
    with check:
        assert res.returncode == 1, res
    with check:
        assert os.strerror(errno.EMFILE) in res.stderr, res


# --- Filesystems that do not report entry types ------------------------------


def test_unknown_entry_types_fall_back_to_stat(
    remmy_bin: Path, faultlib: Path, workdir: Path, threads: int
) -> None:
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir, threads=threads, dt_unknown=True,
    )

    assert hit == []
    with check:
        assert (res.returncode, res.stderr) == (0, ""), res
    with check:
        assert fstree.listing(workdir) == set()


@UNCLASSIFIED_ENTRY_TREATED_AS_FILE
def test_failed_fallback_stat_is_reported(
    remmy_bin: Path, faultlib: Path, workdir: Path
) -> None:
    _tree(workdir)

    res, hit = run_with_faults(
        remmy_bin, faultlib, ["-r", "t"], cwd=workdir, dt_unknown=True,
        faults=[Fault("fstatat", errno.EIO, "name=a")],
    )

    assert hit
    with check:
        assert res.returncode == 1, res
    with check:
        assert "t/a" in res.stderr and os.strerror(errno.EIO) in res.stderr, res
    # Whatever remmy does with an entry it cannot classify, the rest goes.
    with check:
        assert not fstree.exists(workdir / "t/c")
    with check:
        assert not fstree.exists(workdir / "t/x")


# --- Property: any single hard failure anywhere ------------------------------

HARD_ERRNOS = st.sampled_from([errno.EIO, errno.EACCES, errno.EPERM, errno.EBUSY, errno.EROFS])
FAULT_FNS = st.sampled_from(["openat", "unlinkat", "rmdir", "getdirentries"])


def _allowed_survivors(inj: Injection, root: Path) -> set[str]:
    """What a single failure at ``inj`` may legitimately leave behind."""
    rel = _rel(inj.path, root)
    blocked = {rel} | _ancestors(rel)
    if inj.fn in ("openat", "getdirentries"):
        # The directory could not be (fully) read: its contents may remain.
        subtree = {p for p in fstree.listing(root) if p.startswith(rel + "/")}
        return blocked | subtree
    return blocked


@SWALLOWED_UNLINK_ERROR
@given(
    tree=strategies.trees.filter(bool),
    fn=FAULT_FNS,
    err=HARD_ERRNOS,
    nth=st.integers(1, 40),
    threads=st.integers(1, 8),
    dt_unknown=st.booleans(),
)
def test_any_single_failure_is_contained_and_reported(
    remmy_bin: Path,
    faultlib: Path,
    base: Path,
    tree: fstree.Spec,
    fn: str,
    err: int,
    nth: int,
    threads: int,
    dt_unknown: bool,
) -> None:
    strategies.describe_tree(tree)
    event(f"dt_unknown: {dt_unknown}")
    with fstree.scratch(base) as d:
        fstree.build(d / "t", tree)

        res, hit = run_with_faults(
            remmy_bin, faultlib, ["-r", "t"], cwd=d, threads=threads, dt_unknown=dt_unknown,
            faults=[Fault(fn, err, f"nth={nth}")],
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
        assert rel in res.stderr, f"failing path {rel!r} not named\n{res}"
        assert survivors <= _allowed_survivors(inj, d), (
            f"collateral survivors: {sorted(survivors - _allowed_survivors(inj, d))}\n{res}"
        )


@pytest.fixture(scope="module")
def base(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return tmp_path_factory.mktemp("faults")
