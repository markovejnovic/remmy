"""Argument parsing the way BSD rm's getopt(3) does it: usage, unknown options, option placement, ``--``."""

from __future__ import annotations

import os
import subprocess
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


# Until remmy implements them, the options that would make rm ask or keep something
# refuse the whole command line instead of silently removing it all.
@pytest.mark.parametrize(
    ("args", "letter"),
    [
        (("-i", "f"), "i"),
        (("-r", "-i", "d"), "i"),
        (("-fi", "f"), "i"),
        (("-f", "-i", "f"), "i"),
        (("-I", "-r", "d"), "I"),
        (("-I", "f", "g", "h", "d"), "I"),
        (("-If", "f", "g", "h", "d"), "I"),
        (("-W", "f"), "W"),
        (("-fW", "f"), "W"),
        (("-rx", "d"), "x"),
    ],
)
def test_unimplemented_safety_options_refuse_everything(
    run: Runner, workdir: Path, args: tuple[str, ...], letter: str
) -> None:
    fstree.build(workdir, {"f": "x", "g": "x", "h": "x", "d": {"x": "x", "sub": {"y": "y"}}})
    before = fstree.snapshot_dir(workdir)

    res = run(*args)

    with check:
        assert res.returncode == 1, res
    with check:
        assert res.stderr.endswith(f": -{letter}: not supported yet; nothing was removed\n"), res.stderr
    with check:
        assert fstree.snapshot_dir(workdir) == before


# Where rm would neither ask nor keep anything, those options change nothing.
@pytest.mark.parametrize(
    ("args", "left"),
    [
        (("-if", "f"), {"g", "d", "d/x", "d/sub", "d/sub/y"}),
        (("-I", "f", "g", "missing", "d"), {"d", "d/x", "d/sub", "d/sub/y"}),
        (("-I", "-r", "f"), {"g", "d", "d/x", "d/sub", "d/sub/y"}),
        (("-rW", "f"), {"g", "d", "d/x", "d/sub", "d/sub/y"}),
        (("-x", "f"), {"g", "d", "d/x", "d/sub", "d/sub/y"}),
    ],
)
def test_options_rm_would_not_act_on_are_ignored(
    run: Runner, workdir: Path, args: tuple[str, ...], left: set[str]
) -> None:
    fstree.build(workdir, {"f": "x", "g": "x", "d": {"x": "x", "sub": {"y": "y"}}})

    res = run(*args)

    with check:
        assert res.stderr == "" or "is a directory" in res.stderr or "Is a directory" in res.stderr, res
    with check:
        assert fstree.listing(workdir) == left


# BSD rm still exits 64 when it cannot even print its usage.
@pytest.mark.parametrize("args", [(), ("-z", "f")])
def test_usage_with_stderr_closed_still_exits_64(remmy_bin: Path, workdir: Path, args: tuple[str, ...]) -> None:
    fstree.build(workdir, {"f": "x"})
    before = fstree.snapshot_dir(workdir)

    proc = subprocess.run(
        [str(remmy_bin), *args],
        cwd=workdir,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        pass_fds=(),
        preexec_fn=lambda: os.close(2),
        check=False,
    )

    with check:
        assert proc.returncode == 64
    with check:
        assert fstree.snapshot_dir(workdir) == before
