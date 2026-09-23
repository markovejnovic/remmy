"""Argument parsing: help, unknown options, option placement, ``--``."""

from __future__ import annotations

from pathlib import Path

import pytest
from pytest_check import check

import fstree
from harness import Runner


@pytest.mark.parametrize("flag", ["-h", "--help"])
def test_help_goes_to_stdout_and_exits_zero(run: Runner, flag: str) -> None:
    res = run(flag)
    with check:
        assert res.returncode == 0, res
    with check:
        assert res.stderr == ""
    for needle in ("-r", "--recursive", "-h", "--help"):
        with check:
            assert needle in res.stdout


def test_help_wins_over_paths_and_touches_nothing(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"victim": "x", "tree": {"f": "y"}})
    before = fstree.snapshot_dir(workdir)

    res = run("-r", "victim", "tree", "--help")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.snapshot_dir(workdir) == before


def test_no_arguments_is_a_successful_noop(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"keep": "x"})
    before = fstree.snapshot_dir(workdir)

    res = run()

    with check:
        assert (res.returncode, res.stdout, res.stderr) == (0, "", "")
    with check:
        assert fstree.snapshot_dir(workdir) == before


def test_recursive_flag_alone_is_a_successful_noop(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"keep": {"x": "x"}})
    before = fstree.snapshot_dir(workdir)

    res = run("-r")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.snapshot_dir(workdir) == before


@pytest.mark.parametrize("bogus", ["--bogus", "-z", "--recursive=yes", "-rz"])
def test_unknown_option_fails_before_touching_anything(
    run: Runner, workdir: Path, bogus: str
) -> None:
    fstree.build(workdir, {"victim": "x", "tree": {"f": "y"}})
    before = fstree.snapshot_dir(workdir)

    res = run("-r", "victim", bogus, "tree")

    with check:
        assert res.returncode != 0, res
    with check:
        assert bogus in res.stderr
    with check:
        assert res.stdout == ""
    with check:
        assert fstree.snapshot_dir(workdir) == before


@pytest.mark.parametrize("spelling", ["-r", "--recursive"])
@pytest.mark.parametrize("position", ["first", "last"])
def test_recursive_flag_spelling_and_position(
    run: Runner, workdir: Path, spelling: str, position: str
) -> None:
    fstree.build(workdir, {"a": {"b": {"c": "x"}}, "f": "y"})
    args = ["a", "f"]
    args = [spelling, *args] if position == "first" else [*args, spelling]

    res = run(*args)

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


def test_double_dash_allows_removing_dash_prefixed_names(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"-r": "file named -r", "--help": "x", "-d": {"f": "x"}, "keep": "k"})

    res = run("-r", "--", "-r", "--help", "-d")

    with check:
        assert res.returncode == 0, res
    with check:
        assert res.stdout == ""
    with check:
        assert fstree.listing(workdir) == {"keep"}


def test_dot_slash_prefix_removes_dash_prefixed_names(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"-x": "x", "keep": "k"})

    res = run("./-x")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"keep"}
