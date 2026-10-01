"""Argument parsing the way BSD rm's getopt(3) does it: usage, unknown options, option placement, ``--``."""

import os
import subprocess
from pathlib import Path

import fstree
import pytest
from harness import Runner

USAGE = "usage: rm [-f | -i] [-dIPRrvWx] file ...\n       unlink [--] file\n"


def _unchanged(run: Runner, workdir: Path, spec: fstree.Spec, *args: str):
    """Run on ``spec``; return the result and whether the tree is untouched."""
    fstree.build(workdir, spec)
    before = fstree.snapshot_dir(workdir)
    res = run(*args)
    return res, fstree.snapshot_dir(workdir) == before


@pytest.mark.parametrize("args", [(), ("-r",), ("-rv",), ("--",), ("-fi",)])
def test_no_operands_prints_usage_and_exits_64(run: Runner, workdir: Path, args: tuple[str, ...]) -> None:
    res, same = _unchanged(run, workdir, {"keep": {"x": "x"}}, *args)
    assert (res.returncode, res.stdout, res.stderr, same) == (64, "", USAGE, True)


# -f and -i override each other, so only a -f that comes last counts.
@pytest.mark.parametrize("args", [("-f",), ("-rf",), ("-f", "--"), ("-if",), ("-f", "-v")])
def test_no_operands_under_force_is_a_silent_success(run: Runner, workdir: Path, args: tuple[str, ...]) -> None:
    res, same = _unchanged(run, workdir, {"keep": "x"}, *args)
    assert (res.returncode, res.stdout, res.stderr, same) == (0, "", "", True)


# Every --long option is an illegal '-' to getopt(3).
@pytest.mark.parametrize(
    ("bogus", "letter"),
    [("-z", "z"), ("-rz", "z"), ("-h", "h"), ("--help", "-"), ("--bogus", "-"), ("--recursive=yes", "-"), ("-r-", "-")],
)
def test_unknown_option_fails_before_touching_anything(run: Runner, workdir: Path, bogus: str, letter: str) -> None:
    res, same = _unchanged(run, workdir, {"victim": "x", "tree": {"f": "y"}}, "-r", bogus, "victim", "tree")
    assert (res.returncode, res.stdout, same) == (64, "", True), res
    assert res.stderr.endswith(f": illegal option -- {letter}\n{USAGE}"), res.stderr


@pytest.mark.parametrize("spelling", [("-r",), ("-R",), ("-rR",), ("-dr",), ("-r", "-v", "-f")])
def test_recursive_flag_spellings(run: Runner, workdir: Path, spelling: tuple[str, ...]) -> None:
    fstree.build(workdir, {"a": {"b": {"c": "x"}}, "f": "y"})
    res = run(*spelling, "a", "f")
    assert (res.returncode, fstree.listing(workdir)) == (0, set()), res


def test_options_after_an_operand_are_operands(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"f": "y", "-r": "file named -r", "d": {"x": "x"}})
    # Nothing is permuted: "-r" is a file to remove, so "d" is a directory without -r.
    res = run("f", "-r", "d")
    assert (res.returncode, fstree.listing(workdir)) == (1, {"d", "d/x"}), res


def test_lone_dash_is_an_operand(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"-": "file named -", "-f": "file named -f", "keep": "k"})
    res = run("-", "-f")
    assert (res.returncode, fstree.listing(workdir)) == (0, {"keep"}), res


def test_double_dash_allows_removing_dash_prefixed_names(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"-r": "file named -r", "--help": "x", "-d": {"f": "x"}, "keep": "k"})
    res = run("-r", "--", "-r", "--help", "-d")
    assert (res.returncode, res.stdout, fstree.listing(workdir)) == (0, "", {"keep"}), res


def test_dot_slash_prefix_removes_dash_prefixed_names(run: Runner, workdir: Path) -> None:
    fstree.build(workdir, {"-x": "x", "keep": "k"})
    res = run("./-x")
    assert (res.returncode, fstree.listing(workdir)) == (0, {"keep"}), res


# Until remmy implements them, the options that would make rm ask or keep something
# refuse the whole command line instead of silently removing it all.
@pytest.mark.parametrize(
    ("args", "letter"),
    [
        (("-i", "f"), "i"),
        (("-r", "-i", "d"), "i"),
        (("-fi", "f"), "i"),
        (("-f", "-i", "f"), "i"),
        (("-W", "f"), "W"),
        (("-fW", "f"), "W"),
    ],
)
def test_unimplemented_safety_options_refuse_everything(run: Runner, workdir: Path, args, letter: str) -> None:
    res, same = _unchanged(run, workdir, {"f": "x", "g": "x", "h": "x", "d": {"x": "x", "sub": {"y": "y"}}}, *args)
    assert (res.returncode, same) == (1, True), res
    assert res.stderr.endswith(f": -{letter}: not supported yet; nothing was removed\n"), res.stderr


D = {"d", "d/x", "d/sub", "d/sub/y"}


# Where rm would neither ask nor keep anything, those options change nothing.
@pytest.mark.parametrize(
    ("args", "left"),
    [
        (("-if", "f"), {"g", *D}),
        (("-I", "f", "g", "missing", "d"), D),
        (("-I", "-r", "f"), {"g", *D}),
        (("-rW", "f"), {"g", *D}),
        (("-x", "f"), {"g", *D}),
    ],
)
def test_options_rm_would_not_act_on_are_ignored(run: Runner, workdir: Path, args, left: set[str]) -> None:
    fstree.build(workdir, {"f": "x", "g": "x", "d": {"x": "x", "sub": {"y": "y"}}})
    res = run(*args)
    assert all(e.endswith((": is a directory", ": No such file or directory")) for e in res.errors), res
    assert fstree.listing(workdir) == left


# BSD rm still exits 64 when it cannot even print its usage.
@pytest.mark.parametrize("args", [(), ("-z", "f")])
def test_usage_with_stderr_closed_still_exits_64(remmy_bin: Path, workdir: Path, args: tuple[str, ...]) -> None:
    fstree.build(workdir, {"f": "x"})
    before = fstree.snapshot_dir(workdir)
    null = subprocess.DEVNULL
    proc = subprocess.run(
        [remmy_bin, *args],
        cwd=workdir,
        stdin=null,
        stdout=null,
        stderr=null,
        preexec_fn=lambda: os.close(2),
        check=False,
    )
    assert (proc.returncode, fstree.snapshot_dir(workdir)) == (64, before)
