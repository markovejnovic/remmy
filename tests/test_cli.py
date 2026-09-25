"""Argument parsing the way BSD rm's getopt(3) does it: usage, unknown options, option placement, ``--``."""

from __future__ import annotations

from pathlib import Path

import fstree
import pytest
from harness import Runner
from pytest_check import check

USAGE = "usage: rm [-f | -i] [-dIPRrvWx] file ...\n       unlink [--] file\n"


@pytest.mark.parametrize("args", [(), ("-r",), ("-rv",), ("--",), ("-fi",)])
def test_no_operands_prints_usage_and_exits_64(run: Runner, workdir: Path, args: tuple[str, ...]) -> None:
    fstree.build(workdir, {"keep": {"x": "x"}})
    before = fstree.snapshot_dir(workdir)

    res = run(*args)

    with check:
        assert (res.returncode, res.stdout, res.stderr) == (64, "", USAGE)
    with check:
        assert fstree.snapshot_dir(workdir) == before


# -f and -i override each other, so only a -f that comes last counts.
@pytest.mark.parametrize("args", [("-f",), ("-rf",), ("-f", "--"), ("-if",), ("-f", "-v")])
def test_no_operands_under_force_is_a_silent_success(run: Runner, workdir: Path, args: tuple[str, ...]) -> None:
    fstree.build(workdir, {"keep": "x"})
    before = fstree.snapshot_dir(workdir)

    res = run(*args)

    with check:
        assert (res.returncode, res.stdout, res.stderr) == (0, "", "")
    with check:
        assert fstree.snapshot_dir(workdir) == before


# Every --long option is an illegal '-' to getopt(3).
@pytest.mark.parametrize(
    ("bogus", "letter"),
    [("-z", "z"), ("-rz", "z"), ("-h", "h"), ("--help", "-"), ("--bogus", "-"), ("--recursive=yes", "-"), ("-r-", "-")],
)
def test_unknown_option_fails_before_touching_anything(run: Runner, workdir: Path, bogus: str, letter: str) -> None:
    fstree.build(workdir, {"victim": "x", "tree": {"f": "y"}})
    before = fstree.snapshot_dir(workdir)

    res = run("-r", bogus, "victim", "tree")

    with check:
        assert res.returncode == 64, res
    with check:
        assert res.stderr.endswith(f": illegal option -- {letter}\n{USAGE}"), res.stderr
    with check:
        assert res.stdout == ""
    with check:
        assert fstree.snapshot_dir(workdir) == before


@pytest.mark.parametrize("spelling", [("-r",), ("-R",), ("-rR",), ("-dr",), ("-r", "-v", "-f")])
def test_recursive_flag_spellings(run: Runner, workdir: Path, spelling: tuple[str, ...]) -> None:
    fstree.build(workdir, {"a": {"b": {"c": "x"}}, "f": "y"})

    res = run(*spelling, "a", "f")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == set()


def test_options_after_an_operand_are_operands(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"f": "y", "-r": "file named -r", "d": {"x": "x"}})

    # Nothing is permuted: "-r" is a file to remove, so "d" is a directory without -r.
    res = run("f", "-r", "d")

    with check:
        assert res.returncode == 1, res
    with check:
        assert fstree.listing(workdir) == {"d", "d/x"}


def test_lone_dash_is_an_operand(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"-": "file named -", "-f": "file named -f", "keep": "k"})

    res = run("-", "-f")

    with check:
        assert res.returncode == 0, res
    with check:
        assert fstree.listing(workdir) == {"keep"}


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
