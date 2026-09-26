"""Exact parity with BSD ``/bin/rm``, part 2: filesystem semantics and error text.

Covers missing operands, directories, trailing slashes, name and path length
limits, symlinks, special files, hard links, permissions, BSD file flags,
recursive traversal order, overlapping operands, odd names (control bytes, NFC
and NFD, invalid UTF-8), ``-x`` across mount points and invocation names. The
case ids are the ones the probes recorded in catalog-fs.json, except that ids
the CLI module also uses carry an ``fs_`` prefix so a ``-k`` filter picks one.

Order: ``-v`` output and errors inside a directory follow raw readdir order,
post-order, because fts is opened with no comparator. That order is not
alphabetical, but on the same filesystem it is deterministic: every probe
repeated byte for byte, and both runs here build the same names at the same
path. So everything is compared exactly. No case needs an order-insensitive
comparison.

The cases remmy does not pass yet are listed in ``parity_gaps`` and run as strict
xfails.
"""

from __future__ import annotations

import os
import sys
import unicodedata
from pathlib import Path

import pytest
from parity import (
    CLOSED,
    FULL_PATH,
    Case,
    DeepChain,
    Dir,
    Fifo,
    File,
    Hardlink,
    Mount,
    Socket,
    Symlink,
    assert_parity,
    case_id,
    pipe,
    pty,
)

pytestmark = [
    pytest.mark.parity,
    pytest.mark.skipif(sys.platform != "darwin", reason="BSD rm parity is macOS-only"),
]

UCHG = ("uchg",)
TREE = (Dir("d"), File("d/a", "x"), File("d/b", "x"), Dir("d/sub"), File("d/sub/c", "x"), Dir("d/sub/e"))
LONG = "a" * 256
TOO_LONG_PATH = "/".join(["abcdefghij"] * 100)  # 1099 bytes, over PATH_MAX (1024)
NFC = unicodedata.normalize("NFC", "café")
NFD = unicodedata.normalize("NFD", "café")
NONUTF8 = os.fsdecode(b"bad\xff\xfename")
DEEP_NAME = "n" * 50
DEEP_OPERAND = "d/" + "/".join([DEEP_NAME] * 25) + "/leaf"  # ~1300 bytes, over PATH_MAX


def check(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    assert_parity(case, remmy_bin, tmp_path, is_root=is_root)


# --------------------------------------------------------------------------- missing operands, directories

BASICS = [
    Case("fs_missing_file", ["nope"]),
    Case("fs_missing_file_f", ["-f", "nope"]),
    Case("missing_file_r", ["-r", "nope"]),
    Case("missing_file_rf", ["-rf", "nope"]),
    Case("missing_file_v", ["-v", "nope"]),
    Case("fs_missing_and_present", ["a", "nope", "c"], (File("a", "x"), File("c", "x"))),
    Case("fs_missing_and_present_f", ["-f", "a", "nope", "c"], (File("a", "x"), File("c", "x"))),
    Case("missing_and_present_v", ["-v", "a", "nope", "c"], (File("a", "x"), File("c", "x"))),
    Case("missing_in_missing_dir", ["nodir/x"]),
    Case("missing_in_missing_dir_f", ["-f", "nodir/x"]),
    Case("empty_string_operand", [""]),
    Case("empty_string_operand_f", ["-f", ""]),
    # -f hides ENOENT only; every other error still prints and still sets exit 1.
    Case("only_errors_f_exit", ["-f", "nope", "a/"], (File("a", "x"),)),
    Case("fs_dir_no_r", ["d"], (Dir("d"),)),
    Case("fs_dir_no_r_f", ["-f", "d"], (Dir("d"),)),
    Case("fs_dir_no_r_v", ["-v", "d"], (Dir("d"),)),
    Case("nonempty_dir_no_r", ["d"], (Dir("d"), File("d/x", "x"))),
    Case("empty_dir_d", ["-d", "d"], (Dir("d"),)),
    Case("empty_dir_dv", ["-dv", "d"], (Dir("d"),)),
    Case("nonempty_dir_d", ["-d", "d"], (Dir("d"), File("d/x", "x"))),
    Case("nonempty_dir_df", ["-df", "d"], (Dir("d"), File("d/x", "x"))),
    Case("file_with_d", ["-d", "f"], (File("f", "x"),)),
    Case("rd_tree", ["-rd", "d"], TREE),
    Case("rd_nested_empty_dirs", ["-dv", "a"], (Dir("a/b/c"), Dir("a/e"))),
    Case("rdv_nested", ["-rdv", "a"], (Dir("a/b/c"), Dir("a/e"))),
    Case("mixed_no_r_dir_and_files", ["-v", "a", "d", "b"], (File("a", "x"), Dir("d"), File("b", "x"))),
]


@pytest.mark.parametrize("case", BASICS, ids=case_id)
def test_missing_and_directories(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- trailing slashes and path shape

PATHS = [
    Case("dir_trailing_slash_d", ["-d", "d/"], (Dir("d"),)),
    Case("dir_trailing_slash_r", ["-rv", "d/"], (Dir("d"), File("d/x", "x"))),
    Case("dir_trailing_slashes_r", ["-rv", "d//"], (Dir("d"), File("d/x", "x"))),
    Case("dir_trailing_slash_no_r", ["d/"], (Dir("d"),)),
    Case("fs_file_trailing_slash", ["f/"], (File("f", "x"),)),
    Case("fs_file_trailing_slash_f", ["-f", "f/"], (File("f", "x"),)),
    Case("file_trailing_slash_r", ["-r", "f/"], (File("f", "x"),)),
    Case("fifo_trailing_slash", ["p/"], (Fifo("p"),)),
    Case("socket_trailing_slash", ["s/"], (Socket("s"),)),
    Case("path_through_file", ["a/b"], (File("a", "x"),)),
    Case("path_through_file_f", ["-f", "a/b"], (File("a", "x"),)),
    Case("path_through_file_r", ["-r", "a/b"], (File("a", "x"),)),
    # With -r and -f, rm's fts walk passes over an operand it cannot stat without a word, whatever the error.
    Case("file_trailing_slash_rf", ["-rf", "f/"], (File("f", "x"),)),
    Case("file_trailing_slash_Rfv", ["-Rfv", "f/"], (File("f", "x"),)),
    Case("file_trailing_slash_rif", ["-rif", "f/"], (File("f", "x"),)),
    Case("file_trailing_slash_rfi", ["-rfi", "f/"], (File("f", "x"),)),
    Case("path_through_file_rf", ["-rf", "a/b"], (File("a", "x"),)),
    Case("rf_unstatable_then_file", ["-rf", "f/", "a/b", "g"], (File("f", "x"), File("a", "x"), File("g", "x"))),
    Case("relative_and_absolute", ["-v", "f", "{ROOT}/f"], (File("f", "x"),)),
    Case("absolute_path_v", ["-rv", "{ROOT}/d"], (Dir("d"), File("d/x", "x"))),
    Case("absolute_missing", ["{ROOT}/nope"]),
    Case("dot_slash_prefix", ["-rv", "./d"], (Dir("d"), File("d/x", "x"))),
    Case("dotdot_path", ["-rv", "../e"], (Dir("d"), Dir("e"), File("e/x", "x")), cwd="d"),
    Case("redundant_slashes_in_path", ["-v", "d//x"], (Dir("d"), File("d/x", "x"))),
    Case("dot_component_path", ["-v", "d/./x"], (Dir("d"), File("d/x", "x"))),
    Case("rm_dotfile_not_dot", ["-v", ".x", "..y", "..."], (File(".x", "x"), File("..y", "x"), File("...", "x"))),
]


@pytest.mark.parametrize("case", PATHS, ids=case_id)
def test_path_shapes(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- '.' and '..'

DOTS = [
    Case("rm_dot", ["-r", "."], (Dir("d"),), cwd="d"),
    Case("rm_dotdot", ["-r", ".."], (Dir("d"),), cwd="d"),
    Case("rm_dot_f", ["-rf", "."], (Dir("d"),), cwd="d"),
    Case("rm_dot_no_r", ["."], (Dir("d"),), cwd="d"),
    Case("rm_dot_slash", ["-r", "./"], (Dir("d"),), cwd="d"),
    Case("rm_dotdot_slash", ["-r", "../"], (Dir("d"),), cwd="d"),
    Case("rm_dot_and_file", ["-rv", ".", "x"], (Dir("d"), File("d/x", "x")), cwd="d"),
    Case("rm_dot_mixed_with_missing", ["-rf", "nope", ".", "..", "nope2"], (Dir("d"),), cwd="d"),
    Case("rm_dir_slash_dot", ["-rv", "d/."], (Dir("d"), File("d/x", "x"))),
    Case("rm_dir_slash_dotdot", ["-rv", "d/e/.."], (Dir("d"), Dir("d/e"))),
    Case("rm_dir_dot_slash", ["-r", "d/./"], (Dir("d"), File("d/x", "x"))),
]


@pytest.mark.parametrize("case", DOTS, ids=case_id)
def test_dot_operands(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- length limits

LIMITS = [
    Case("name_too_long", [LONG]),
    Case("name_too_long_f", ["-f", LONG]),
    Case("name_too_long_r", ["-r", LONG]),
    Case("name_too_long_rf", ["-rf", LONG]),
    Case("path_too_long_missing", [TOO_LONG_PATH]),
    Case("path_too_long_missing_f", ["-f", TOO_LONG_PATH]),
    Case("path_too_long_missing_rf", ["-rf", TOO_LONG_PATH]),
    Case("name_255_ok", ["-v", "b" * 255], (File("b" * 255, "x"),)),
    # The leaf is ~2040 bytes deep, past PATH_MAX; fts still removes it.
    Case("deep_tree_beyond_pathmax", ["-r", "d"], (DeepChain("d", DEEP_NAME, 40),)),
    Case("deep_tree_beyond_pathmax_v", ["-rv", "d"], (DeepChain("d", DEEP_NAME, 25),)),
    Case("deep_existing_operand_too_long", [DEEP_OPERAND], (DeepChain("d", DEEP_NAME, 25),)),
    Case("deep_existing_operand_too_long_f", ["-f", DEEP_OPERAND], (DeepChain("d", DEEP_NAME, 25),)),
]


@pytest.mark.parametrize("case", LIMITS, ids=case_id)
def test_length_limits(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- symlinks, special files, hard links

TARGET = (Dir("t"), File("t/x", "x"), Symlink("l", "t"))

LINKS = [
    Case("symlink_to_dir", ["l"], TARGET),
    Case("symlink_to_dir_slash", ["l/"], TARGET),
    Case("symlink_to_dir_r", ["-rv", "l"], TARGET),
    # A trailing slash follows the link: the target is emptied and rmdir'd, the link stays.
    Case("symlink_to_dir_slash_r", ["-rv", "l/"], TARGET),
    Case("symlink_to_dir_slash_d", ["-dv", "l/"], (Dir("t"), Symlink("l", "t"))),
    Case(
        "symlink_to_dir_slash_rf_leaves_symlink",
        ["-rfv", "l/"],
        (Dir("t"), Dir("t/s"), File("t/s/y", "x"), Symlink("l", "t")),
    ),
    Case("symlink_to_file", ["-v", "l"], (File("t", "x"), Symlink("l", "t"))),
    Case("symlink_to_file_slash", ["l/"], (File("t", "x"), Symlink("l", "t"))),
    Case("dangling_symlink", ["-v", "l"], (Symlink("l", "nowhere"),)),
    Case("dangling_symlink_slash", ["l/"], (Symlink("l", "nowhere"),)),
    Case("symlink_loop", ["-v", "a"], (Symlink("a", "b"), Symlink("b", "a"))),
    Case("symlink_loop_slash", ["a/"], (Symlink("a", "b"), Symlink("b", "a"))),
    Case("symlink_loop_slash_rf", ["-rf", "a/"], (Symlink("a", "b"), Symlink("b", "a"))),
    Case("symlink_to_file_slash_rf", ["-rf", "l/"], (File("t", "x"), Symlink("l", "t"))),
    Case("symlink_self_loop_r", ["-rv", "s"], (Symlink("s", "s"),)),
    Case("symlink_inside_tree_r", ["-rv", "d"], (Dir("t"), File("t/x", "x"), Dir("d"), Symlink("d/l", "../t"))),
    Case("symlink_through_operand", ["-v", "l/x"], TARGET),
    Case("fifo", ["-v", "p"], (Fifo("p"),)),
    Case("socket", ["-v", "s"], (Socket("s"),)),
    Case("fifo_socket_in_tree", ["-r", "d"], (Dir("d"), Fifo("d/p"), Socket("d/s"))),
    Case("hardlinks", ["-v", "a"], (File("a", "hello"), Hardlink("b", "a"))),
    Case("hardlinks_both", ["-v", "a", "b"], (File("a", "hello"), Hardlink("b", "a"))),
    Case("hardlink_in_tree", ["-rv", "d"], (File("a", "hello"), Dir("d"), Hardlink("d/b", "a"))),
]


@pytest.mark.parametrize("case", LINKS, ids=case_id)
def test_links_and_special_files(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- permissions

LOCKED_SUB = (Dir("d"), File("d/a", "x"), Dir("d/s", mode=0o000), File("d/s/x", "x"), File("d/z", "x"))
RO_F = (File("f", "x", mode=0o444),)

PERMISSIONS = [
    Case("file_in_0555_dir", ["d/f"], (Dir("d", mode=0o555), File("d/f", "x"))),
    Case("file_in_0555_dir_f", ["-f", "d/f"], (Dir("d", mode=0o555), File("d/f", "x"))),
    Case("file_in_0555_dir_v", ["-v", "d/f"], (Dir("d", mode=0o555), File("d/f", "x"))),
    Case("rf_0555_dir", ["-rf", "d"], (Dir("d", mode=0o555), File("d/f", "x"))),
    Case("r_0555_dir", ["-rv", "d"], (Dir("d", mode=0o555), File("d/f", "x"), File("d/g", "x"))),
    Case("r_subdir_0000", ["-r", "d"], LOCKED_SUB),
    Case("rv_subdir_0000", ["-rv", "d"], LOCKED_SUB),
    Case("rf_subdir_0000", ["-rf", "d"], LOCKED_SUB),
    Case("r_empty_subdir_0000", ["-rv", "d"], (Dir("d"), Dir("d/s", mode=0o000))),
    Case("r_operand_0000", ["-r", "d"], (Dir("d", mode=0o000), File("d/x", "x"))),
    Case("r_empty_operand_0000", ["-rv", "d"], (Dir("d", mode=0o000),)),
    # -f still rmdirs a directory it cannot read, silently when that works.
    Case("rf_empty_subdir_0000", ["-rf", "d"], (Dir("d"), Dir("d/s", mode=0o000))),
    Case("rfv_empty_subdir_0000", ["-rfv", "d"], (Dir("d"), Dir("d/s", mode=0o000))),
    Case("rf_empty_nested_0000", ["-rf", "d"], (Dir("d"), Dir("d/s"), Dir("d/s/t", mode=0o000))),
    Case("rf_empty_operand_0000", ["-rf", "d"], (Dir("d", mode=0o000),)),
    Case("rf_operand_0000", ["-rf", "d"], (Dir("d", mode=0o000), File("d/x", "x"))),
    Case("rf_empty_subdir_0300", ["-rf", "d"], (Dir("d"), Dir("d/s", mode=0o300))),
    Case("r_subdir_0555", ["-r", "d"], (Dir("d"), Dir("d/s", mode=0o555), File("d/s/x", "x"), File("d/s/y", "x"))),
    Case("rf_subdir_0555", ["-rf", "d"], (Dir("d"), Dir("d/s", mode=0o555), File("d/s/x", "x"), File("d/s/y", "x"))),
    Case("r_subdir_0333", ["-rv", "d"], (Dir("d"), Dir("d/s", mode=0o333), File("d/s/x", "x"))),
    Case("r_subdir_0444", ["-rv", "d"], (Dir("d"), Dir("d/s", mode=0o444), File("d/s/x", "x"))),
    Case(
        "r_nested_0000_two",
        ["-r", "d"],
        (
            Dir("d"),
            Dir("d/a", mode=0o000),
            File("d/a/x", "x"),
            Dir("d/b", mode=0o000),
            File("d/b/y", "x"),
            File("d/c", "x"),
        ),
    ),
    Case("r_subdir_of_0555_parent", ["-rv", "d/s"], (Dir("d", mode=0o555), Dir("d/s"), File("d/s/x", "x"))),
    Case(
        "r_tree_with_0444_files_devnull",
        ["-rv", "d"],
        (Dir("d"), File("d/a", "x", mode=0o444), File("d/b", "x", mode=0o000)),
    ),
    Case("lookup_through_0000_dir", ["d/s/f"], (Dir("d"), Dir("d/s", mode=0o000), File("d/s/f", "x"))),
    Case("lookup_through_0000_dir_f", ["-f", "d/s/f"], (Dir("d"), Dir("d/s", mode=0o000), File("d/s/f", "x"))),
    Case("lookup_missing_through_0000_dir_f", ["-f", "d/s/nope"], (Dir("d"), Dir("d/s", mode=0o000))),
    Case("lookup_through_0000_dir_rf", ["-rf", "d/s/f"], (Dir("d"), Dir("d/s", mode=0o000), File("d/s/f", "x"))),
    Case("lookup_through_0000_dir_r", ["-r", "d/s/f"], (Dir("d"), Dir("d/s", mode=0o000), File("d/s/f", "x"))),
    # Read-only files: only a tty on stdin gets the override prompt.
    Case("file_0444_devnull", ["-v", "f"], RO_F),
    Case("file_0444_pipe", ["f"], RO_F, stdin=pipe("n\n")),
    Case("file_0444_closed_stdin", ["f"], RO_F, stdin=CLOSED),
    Case("file_0444_pty_no", ["f"], RO_F, stdin=pty("n\n")),
    Case("file_0444_pty_yes", ["f"], RO_F, stdin=pty("y\n")),
    Case("file_0444_pty_f", ["-f", "f"], RO_F, stdin=pty("n\n")),
    Case("tree_0444_files_r_pty", ["-r", "d"], (Dir("d"), File("d/f", "x", mode=0o444)), stdin=pty("n\n")),
    Case("file_0000_devnull", ["-v", "f"], (File("f", "x", mode=0o000),)),
    Case("symlink_to_0444_pty", ["l"], (File("t", "x", mode=0o444), Symlink("l", "t")), stdin=pty("n\n")),
]


@pytest.mark.parametrize("case", PERMISSIONS, ids=case_id)
def test_permissions(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- BSD file flags

UCHG_F = (File("f", "x", flags=UCHG),)
UAPPND_F = (File("f", "x", flags=("uappnd",)),)
UCHG_LINK = (File("t", "x"), Symlink("l", "t", flags=UCHG))

FLAGS = [
    Case("uchg_file", ["f"], UCHG_F),
    Case("uchg_file_v", ["-v", "f"], UCHG_F),
    Case("fs_uchg_file_f", ["-f", "f"], UCHG_F),
    Case("uchg_file_fv", ["-fv", "f"], UCHG_F),
    Case("uappnd_file", ["f"], UAPPND_F),
    Case("uappnd_file_f", ["-f", "f"], UAPPND_F),
    Case("uchg_file_pty", ["f"], UCHG_F, stdin=pty("y\n")),
    Case("uchg_file_pty_no", ["f"], UCHG_F, stdin=pty("n\n")),
    Case("uchg_file_0444_pty", ["f"], (File("f", "x", mode=0o444, flags=UCHG),), stdin=pty("y\n")),
    Case("uchg_in_tree_r", ["-r", "d"], (Dir("d"), File("d/a", "x"), File("d/f", "x", flags=UCHG))),
    Case("uchg_in_tree_rf", ["-rfv", "d"], (Dir("d"), File("d/a", "x"), File("d/f", "x", flags=UCHG))),
    Case("uchg_dir_r", ["-r", "d"], (Dir("d", flags=UCHG), File("d/a", "x"))),
    Case("uchg_dir_rf", ["-rfv", "d"], (Dir("d", flags=UCHG), File("d/a", "x"))),
    Case("uchg_empty_dir_d", ["-d", "d"], (Dir("d", flags=UCHG),)),
    Case("uchg_empty_dir_df", ["-df", "d"], (Dir("d", flags=UCHG),)),
    Case("uchg_subdir_rf", ["-rfv", "d"], (Dir("d"), Dir("d/s", flags=UCHG), File("d/s/x", "x"))),
    Case("uappnd_dir_rf", ["-rfv", "d"], (Dir("d", flags=("uappnd",)), File("d/a", "x"))),
    Case("uchg_symlink_f", ["-fv", "l"], UCHG_LINK),
    Case("uchg_symlink", ["-v", "l"], UCHG_LINK),
    Case("symlink_to_uchg_file", ["-v", "l"], (File("t", "x", flags=UCHG), Symlink("l", "t"))),
    Case("uchg_file_in_0555_dir_f", ["-f", "d/f"], (Dir("d", mode=0o555), File("d/f", "x", flags=UCHG))),
]


@pytest.mark.parametrize("case", FLAGS, ids=case_id)
def test_file_flags(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- recursion, order, overlap

RECURSION = [
    Case("fs_rv_tree", ["-rv", "d"], TREE),
    Case("r_tree_quiet", ["-r", "d"], TREE),
    # 200 siblings: -v order is readdir order (f034, f033, f005, ...), not sorted.
    Case("rv_wide", ["-rv", "w"], (Dir("w"), *(File(f"w/f{i:03d}", "x") for i in range(200)))),
    Case("rv_deep", ["-rv", "d"], (Dir("/".join(["d"] * 29)), File("/".join(["d"] * 29) + "/leaf", "x"))),
    Case(
        "rv_mixed_operands_fail",
        ["-rv", "a", "nope", "d", "f/", "e"],
        (File("a", "x"), Dir("d"), File("d/x", "x"), Dir("e")),
    ),
    Case("duplicate_operands", ["-v", "f", "f"], (File("f", "x"),)),
    Case("duplicate_operands_f", ["-fv", "f", "f"], (File("f", "x"),)),
    Case("duplicate_dir_operands_r", ["-rv", "d", "d"], (Dir("d"), File("d/x", "x"))),
    Case("overlap_parent_then_child", ["-rv", "d", "d/x"], (Dir("d"), File("d/x", "x"), File("d/y", "x"))),
    Case("overlap_child_then_parent", ["-rv", "d/x", "d"], (Dir("d"), File("d/x", "x"), File("d/y", "x"))),
    # Without -v too, a later operand sees what an earlier walk removed, and
    # errors of walked operands come out in operand order.
    Case("duplicate_dir_operands_quiet", ["-r", "d", "d"], (Dir("d"), File("d/x", "x"))),
    Case("duplicate_dir_operands_other_case", ["-r", "d", "D"], (Dir("d"), File("d/x", "x"))),
    Case("overlap_parent_then_child_quiet", ["-r", "d", "d/x"], (Dir("d"), File("d/x", "x"), File("d/y", "x"))),
    Case("overlap_parent_then_subdir_quiet", ["-r", "d", "d/s"], (Dir("d/s"), File("d/s/x", "x"))),
    Case("overlap_subdir_then_parent_quiet", ["-r", "d/s", "d/t", "d"], (Dir("d/s"), Dir("d/t"), File("d/s/x", "x"))),
    Case("overlap_via_symlink_quiet", ["-r", "d", "l/s"], (Dir("d/s"), File("d/s/x", "x"), Symlink("l", "d"))),
    Case("overlap_symlink_slash_then_link", ["-r", "l/", "l"], (Dir("t/s"), File("t/s/x", "x"), Symlink("l", "t"))),
    Case("overlap_dir_then_symlink_slash", ["-r", "t", "l/"], (Dir("t/s"), File("t/s/x", "x"), Symlink("l", "t"))),
    Case("siblings_rf", ["-rf", "p/a", "p/b", "p/f", "p/c"], (Dir("p/a/s"), Dir("p/b"), Dir("p/c"), File("p/f", "x"))),
    Case(
        "sibling_walks_errors_in_order",
        ["-r", "a", "b", "nope", "c", "f"],
        (
            *(e for n in "abc" for e in (Dir(f"{n}/ro", mode=0o555), File(f"{n}/ro/x", "x"), File(f"{n}/y", "x"))),
            File("f", "x"),
        ),
    ),
    Case(
        "sibling_walks_errors_in_order_rf",
        ["-rf", "p/a", "p/b", "p/nope", "p/c"],
        tuple(e for n in "abc" for e in (Dir(f"p/{n}/ro", mode=0o555), File(f"p/{n}/ro/x", "x"))),
    ),
]


@pytest.mark.parametrize("case", RECURSION, ids=case_id)
def test_recursion_and_order(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- names

CONTROL_SWEEP = [os.fsdecode(b"m" + bytes([b]) + b"z") for b in [*range(1, 32), 0x7F, 0x80, 0xFF]]

# Errors on stderr escape C0 controls vis-style, except TAB and LF. -v output
# and prompts print names raw.
NAMES = [
    Case("name_with_space", ["-v", "a b", "no such"], (File("a b", "x"),)),
    Case("name_with_newline", ["-v", "a\nb", "no\nsuch"], (File("a\nb", "x"),)),
    Case("name_with_tab_esc", ["-v", "a\tb", "e\x1bx", "m\x1bissing"], (File("a\tb", "x"), File("e\x1bx", "x"))),
    # APFS refuses names that are not UTF-8, so only missing operands can be tested.
    Case("name_non_utf8_missing", [NONUTF8, os.fsdecode(b"miss\xff"), os.fsdecode(b"x\xc3")]),
    Case("name_non_utf8_missing_f", ["-f", NONUTF8]),
    Case("control_bytes_missing_sweep", CONTROL_SWEEP),
    Case(
        "control_bytes_missing_sweep_C_locale",
        ["a\x1bb", "café", os.fsdecode(b"x\xffy")],
        env={"LC_ALL": "C"},
    ),
    Case("esc_name_dir_no_r", ["e\x1bd"], (Dir("e\x1bd"),)),
    Case(
        "esc_name_v_stdout",
        ["-v", "e\x1bf", "c\rr", "b\x07l"],
        (File("e\x1bf", "x"), File("c\rr", "x"), File("b\x07l", "x")),
    ),
    Case("esc_name_in_tree_perm", ["-r", "d"], (Dir("d"), Dir("d/s\x1bt", mode=0o000), File("d/s\x1bt/x", "x"))),
    Case("esc_name_in_0555_dir", ["d\x1b/f\x01"], (Dir("d\x1b", mode=0o555), File("d\x1b/f\x01", "x"))),
    Case("esc_name_prompt_pty", ["e\x1bf"], (File("e\x1bf", "x", mode=0o444),), stdin=pty("n\n")),
    Case("esc_name_trailing_slash", ["e\x1bf/"], (File("e\x1bf", "x"),)),
    Case("esc_name_dot_dir", ["-r", "e\x1bd/."], (Dir("e\x1bd"),)),
    Case("name_nfc_created_nfd_operand", ["-v", NFD], (File(NFC, "x"),)),
    Case("name_nfd_created_nfc_operand", ["-v", NFC], (File(NFD, "x"),)),
    Case("name_nfc_in_tree_v", ["-rv", "d"], (Dir("d"), File("d/" + NFC, "x"))),
    Case("name_unicode_missing", ["日本"]),
    Case("case_insensitive_operand", ["-v", "foo"], (File("Foo", "x"),)),
    Case("dash_name_double_dash", ["-v", "--", "-x"], (File("-x", "x"),)),
    Case("dash_name_dot_slash", ["-v", "./-x"], (File("-x", "x"),)),
    Case("dash_name_without_dd", ["-x"], (File("-x", "x"),)),
    Case("single_dash_operand", ["-v", "-"], (File("-", "x"),)),
]


@pytest.mark.parametrize("case", NAMES, ids=case_id)
def test_names(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- mount points (-x)

MOUNTED = (Dir("d"), File("d/a", "x"), Mount("d/mnt"), File("d/mnt/inner", "x"))

MOUNTS = [
    Case("x_mountpoint_r", ["-rxv", "d"], MOUNTED),
    Case("mountpoint_r_no_x", ["-rv", "d"], MOUNTED),
    Case("x_operand_is_mount", ["-rxv", "m"], (Mount("m"), File("m/inner", "x"))),
]


@pytest.mark.slow
@pytest.mark.parametrize("case", MOUNTS, ids=case_id)
def test_mount_points(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- stdin kinds

PLAIN = (File("f", "x"), Dir("d"))

STDIN = [
    Case("stdin_closed_plain", ["-v", "f", "d"], PLAIN, stdin=CLOSED),
    Case("stdin_pipe_plain", ["-v", "f", "d"], PLAIN, stdin=pipe("y\n")),
    Case("stdin_pty_plain", ["-v", "f", "d"], PLAIN, stdin=pty()),
]


@pytest.mark.parametrize("case", STDIN, ids=case_id)
def test_stdin_kinds(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- invocation names

# The prefix comes from the basename of the file executed; unlink mode comes
# from the basename of argv[0]. The two are tested apart here.
INVOCATION = [
    Case("argv0_symlink_rm", ["d", "nope"], (Dir("d"),), argv0=FULL_PATH),
    Case("argv0_symlink_remmy", ["d", "nope"], (Dir("d"),), prog="remmy", argv0=FULL_PATH),
    Case("argv0_symlink_remmy_usage", [], prog="remmy", argv0=FULL_PATH),
    Case("argv0_symlink_unlink", ["f"], (File("f", "x"), File("g", "x")), prog="unlink", argv0=FULL_PATH),
    Case("argv0_symlink_unlink_dir", ["d"], (Dir("d"),), prog="unlink", argv0=FULL_PATH),
    Case("argv0_symlink_unlink_two", ["f", "g"], (File("f", "x"), File("g", "x")), prog="unlink", argv0=FULL_PATH),
    Case("argv0_symlink_unlink_missing", ["nope"], prog="unlink", argv0=FULL_PATH),
    Case("argv0_symlink_unlink_flag", ["-f", "f"], (File("f", "x"),), prog="unlink", argv0=FULL_PATH),
    Case("argv0_symlink_unlink_dd", ["--", "-f"], (File("-f", "x"),), prog="unlink", argv0=FULL_PATH),
    Case("argv0_symlink_unlink_noargs", [], prog="unlink", argv0=FULL_PATH),
    Case("argv0_arbitrary_name", ["d", "nope"], (Dir("d"),), argv0="frobnicate"),
    Case("argv0_path_to_unlink_name", ["f"], (File("f", "x"),), argv0="/some/where/unlink"),
    Case("exe_remmy_argv0_rm", ["d", "nope"], (Dir("d"),), prog="remmy", argv0="rm"),
    Case(
        "exe_unlink_argv0_frob_two_operands",
        ["f", "g", "nope"],
        (File("f", "x"), File("g", "x")),
        prog="unlink",
        argv0="frob",
    ),
    Case("exe_rm_argv0_unlink_two_operands", ["f", "g"], (File("f", "x"), File("g", "x")), argv0="unlink"),
    Case("exe_rm_argv0_unlink_one_missing", ["nope"], argv0="/x/y/unlink"),
    Case("exe_rm_argv0_unlink_dir", ["d"], (Dir("d"),), argv0="unlink"),
]


@pytest.mark.parametrize("case", INVOCATION, ids=case_id)
def test_invocation_names(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)
