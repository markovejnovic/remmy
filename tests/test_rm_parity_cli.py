"""Exact parity with BSD ``/bin/rm``, part 1: the command line.

Covers usage text, getopt quirks, the ``-f``/``-i``/``-I`` prompts and how
answers are parsed, the ``.``/``..``/``/`` guards, invocation names and unlink
mode, and ``-P``/``-W``/``-x``. Every case must give the same stdout and stderr
bytes, the same exit code and the same leftover tree as ``/bin/rm`` from
file_cmds-487 (see ``parity``). The case ids are the ones the probes recorded in
catalog-cli.json.

remmy is not expected to pass these yet: they spell out what full parity means.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
from parity import (
    CLOSED,
    DEVNULL,
    FULL_PATH,
    Case,
    Dir,
    Fifo,
    File,
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

F = (File("a", "A"),)
F3 = tuple(File(n, n.upper()) for n in "abc")
F4 = tuple(File(n, n.upper()) for n in "abcd")
T = (File("a", "A"), File("b", "B"), Dir("d"), File("d/x", "X"), Dir("d/sub"), File("d/sub/y", "Y"))
RO = (File("a", "A", mode=0o444),)
UCHG = ("uchg",)


def check(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    assert_parity(case, remmy_bin, tmp_path, is_root=is_root)


# --------------------------------------------------------------------------- usage and getopt

USAGE = [
    Case("noargs", []),
    # With no operands, -f exits 0 silently and everything else prints usage (64).
    *(
        Case("only_opt" + "".join(opts).replace("-", "_"), opts)
        for opts in (["-f"], ["-r"], ["-rf"], ["-fr"], ["-i"], ["-I"], ["-v"], ["-d"], ["-P"], ["-W"], ["-x"], ["-R"])
    ),
    Case("only_opt_f_v", ["-f", "-v"]),
    Case("only_dashdash", ["--"]),
    Case("f_dashdash", ["-f", "--"]),
    # Unknown options: "<argv[0]>: illegal option -- c" then usage; long options fail on '-'.
    *(
        Case("unknown" + opt.replace("-", "_"), [opt, "a"], F)
        for opt in ("-z", "-h", "-rz", "-zr", "-fz", "--help", "--version", "--recursive", "--force", "---")
    ),
    Case("unknown_", ["-", "a"], F),  # a lone '-' is an operand, not an option
    Case("unknown_z_no_operand", ["-z"]),
    Case("unknown_after_operand", ["a", "-z"], F),
    Case("cluster_trailing_dash", ["-r-", "a"], F),
    # argv[0] is printed verbatim in the getopt message.
    Case("badopt_argv0_rm", ["-z"], argv0="rm"),
    Case("badopt_argv0_relpath", ["-z"], argv0="foo/bar"),
    Case("badopt_argv0_empty", ["-z"], argv0=""),
    Case("usage_argv0_foo", [], argv0="foo"),
    # getopt stops at the first operand and at a lone '-'; options are never permuted.
    Case("dash_then_opt", ["-", "-f", "a"], F),
    Case("operand_dashdash_after", ["a", "--", "b"], T),
    Case("double_dashdash", ["--", "--", "a"], F),
    Case("dashdash_twice", ["--", "--"], (File("--", "D"),)),
    Case("opt_after_operand_r_dir", ["d", "-r"], T),
    Case("opt_after_operand_f", ["nope", "-f"]),
    Case("opt_after_operand_v", ["a", "-v"], F),
    Case("opt_after_operand_v_posixly", ["a", "-v"], F, env={"POSIXLY_CORRECT": "1"}),
    Case("opt_after_operand_v_file_named_dashv", ["a", "-v"], (*F, File("-v", "V"))),
    Case("ff", ["-ff", "nope"]),
    Case("ii", ["-ii", "a"], F, stdin=pipe("y\n")),
    Case("rR", ["-rR", "d"], T),
    Case("all_flags", ["-dfIPRrvWx", "nope"]),
    Case("all_flags_valid_no_W", ["-dfIPRrvx", "d"], T),
]


@pytest.mark.parametrize("case", USAGE, ids=case_id)
def test_usage_and_getopt(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- operands

OPERANDS = [
    Case("file_plain", ["a"], F),
    Case("missing_file", ["nope"]),
    Case("missing_file_f", ["-f", "nope"]),
    Case("missing_and_present", ["nope", "a"], F),
    Case("missing_and_present_f", ["-f", "nope", "a"], F),
    Case("empty_string", [""]),
    Case("empty_string_f", ["-f", ""]),
    Case("empty_string_and_file", ["", "a"], F),
    Case("empty_string_rf", ["-rf", ""]),
    Case("dash_operand_missing", ["--", "-"]),
    Case("dash_operand_present", ["--", "-"], (File("-", "D"),)),
    Case("dash_operand_alone_present", ["-"], (File("-", "D"),)),
    Case("dash_operand_alone_missing", ["-"]),
    Case("dashfile_via_dashdash", ["--", "-f"], (File("-f", "D"),)),
    Case("dashfile_without_dashdash", ["-f"], (File("-f", "D"),)),
    Case("v_file", ["-v", "a"], F),
    Case("v_files", ["-v", "a", "b"], T),
    Case("v_repeated", ["-vv", "a"], F),
    Case("v_path_prefix", ["-v", "./a", "d/x", "d//sub/y"], T),
    Case("v_duplicate_operand", ["-v", "a", "a"], F),
    # Names are printed raw in -v output, errors and prompts.
    Case(
        "v_odd_names",
        ["-v", "sp ace", "new\nline", "pct%s", "é"],
        (File("sp ace"), File("new\nline"), File("pct%s"), File("é")),
    ),
    Case("err_odd_names", ["new\nline", "pct%s"]),
]


@pytest.mark.parametrize("case", OPERANDS, ids=case_id)
def test_operands(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- directories, -d, -r

DIRECTORIES = [
    Case("dir_no_r", ["d"], T),
    Case("dir_no_r_f", ["-f", "d"], T),
    Case("dir_no_r_v", ["-v", "d", "a"], T),
    Case("emptydir_no_r", ["e"], (Dir("e"),)),
    Case("emptydir_d", ["-d", "e"], (Dir("e"),)),
    Case("emptydir_dv", ["-dv", "e"], (Dir("e"),)),
    Case("nonemptydir_d", ["-d", "d"], T),
    Case("file_d", ["-d", "a"], F),
    Case("d_and_r", ["-dr", "d"], T),
    Case("d_and_r_v", ["-drv", "d"], T),
    Case("r_tree", ["-r", "d"], T),
    Case("R_tree", ["-R", "d"], T),
    Case("rv_tree", ["-rv", "d"], T),
    Case("Rv_tree", ["-Rv", "d"], T),
    Case("rv_all", ["-rv", "a", "b", "d"], T),
    Case("rfv_missing", ["-rfv", "nope", "a", "d"], T),
    Case("rv_missing", ["-rv", "nope", "a", "d"], T),
    Case("v_separate_flags", ["-r", "-v", "-f", "d"], T),
    # -v joins with '/', so a trailing slash on the operand doubles up.
    Case("rv_trailing_slash", ["-rv", "d/"], T),
    Case("rv_trailing_slashes", ["-rv", "d//"], T),
    Case("rv_dot_prefix", ["-rv", "./d"], T),
    Case("rv_abs", ["-rv", "{ROOT}/d"], T),
    Case("rv_two_trees", ["-rv", "d", "e"], (*T, Dir("e"), File("e/z", "Z"), Dir("e/s"), File("e/s/w", "W"))),
    Case("rv_symlink_to_dir", ["-rv", "l"], (*T, Symlink("l", "d"))),
    Case("rv_symlink_to_dir_slash", ["-rv", "l/"], (*T, Symlink("l", "d"))),
    Case("v_symlink_to_dir_nor_slash", ["-v", "l/"], (*T, Symlink("l", "d"))),
    Case("rv_nested_operands", ["-rv", "d", "d/x"], T),
    Case("rv_nested_operands_rev", ["-rv", "d/x", "d"], T),
]


@pytest.mark.parametrize("case", DIRECTORIES, ids=case_id)
def test_directories(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- -P, -W, -x

PWX = [
    Case("P_noop", ["-Pv", "a"], F),
    Case("P_r_tree", ["-rPv", "d"], T),
    Case("P_v_file", ["-Pv", "a", "b"], T),
    # -P opens the file for writing, so a read-only file fails even with -f.
    Case("P_f_ro", ["-Pf", "a"], RO),
    Case("P_ro_devnull", ["-P", "a"], RO),
    Case("P_ro_pty", ["-P", "a"], RO, stdin=pty("y\n")),
    # -W (undelete) on APFS: EEXIST for an existing file, EPERM for a missing one.
    Case("W_missing", ["-W", "a"], F),
    Case("W_nonexist", ["-W", "nope"]),
    Case("Wf_nonexist", ["-Wf", "nope"]),
    Case("Wv", ["-Wv", "a"], F),
    Case("W_dir", ["-W", "d"], T),
    Case("W_r_dir", ["-rW", "d"], T),
    Case("W_emptystring", ["-W", ""]),
    Case("fW_missing", ["-fW", "nope"]),
    Case("dW_missing", ["-dW", "nope"]),
    Case("rW_missing", ["-rW", "nope"]),
    Case("rWv_file", ["-rWv", "a"], F),
    Case("rWv_dir", ["-rWv", "d"], T),
    Case("rWf_missing", ["-rWf", "nope"]),
    Case("x_flag", ["-rxv", "d"], T),
    Case("x_file", ["-xv", "a"], F),
]


@pytest.mark.parametrize("case", PWX, ids=case_id)
def test_P_W_x(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- invocation names, unlink mode

# The message prefix is the basename of the file executed (getprogname), so the
# symlink's name matters. Unlink mode is picked from the basename of argv[0].
INVOCATION = [
    Case("argv0_symlink_rm_missing", ["nope"], argv0=FULL_PATH),
    Case("argv0_symlink_rm_dir", ["d"], T, argv0=FULL_PATH),
    Case("rm_symlink_named_rm_badopt_fullpath", ["-z"], argv0=FULL_PATH),
    Case("argv0_symlink_remmy_missing", ["nope"], prog="remmy", argv0=FULL_PATH),
    Case("argv0_symlink_remmy_noargs", [], prog="remmy", argv0=FULL_PATH),
    Case("argv0_symlink_remmy_badopt", ["-z"], prog="remmy", argv0=FULL_PATH),
    Case("argv0_custom_string", ["nope"], argv0="foo/bar"),
    Case("argv0_custom_unlink_string", ["nope"], argv0="unlink"),
    Case("argv0_custom_unlink_path", ["nope"], argv0="/x/y/unlink"),
    Case("argv0_empty", ["nope"], argv0=""),
    Case("unlink_noargs", [], prog="unlink", argv0=FULL_PATH),
    Case("unlink_one", ["a"], F, prog="unlink", argv0=FULL_PATH),
    Case("unlink_missing", ["nope"], prog="unlink", argv0=FULL_PATH),
    Case("unlink_two", ["a", "b"], T, prog="unlink", argv0=FULL_PATH),
    Case("unlink_dir", ["d"], T, prog="unlink", argv0=FULL_PATH),
    Case("unlink_emptydir", ["e"], (Dir("e"),), prog="unlink", argv0=FULL_PATH),
    Case("unlink_dashdash", ["--", "a"], F, prog="unlink", argv0=FULL_PATH),
    Case("unlink_dashdash_only", ["--"], prog="unlink", argv0=FULL_PATH),
    Case("unlink_dashdash_two", ["--", "a", "b"], T, prog="unlink", argv0=FULL_PATH),
    Case("unlink_opt_f", ["-f", "a"], F, prog="unlink", argv0=FULL_PATH),
    Case("unlink_opt_f_file", ["-f"], (File("-f", "D"),), prog="unlink", argv0=FULL_PATH),
    Case("unlink_symlink", ["l"], (File("a", "A"), Symlink("l", "a")), prog="unlink", argv0=FULL_PATH),
    Case("unlink_readonly_pty", ["a"], RO, prog="unlink", argv0=FULL_PATH, stdin=pty("n\n")),
    Case("unlink_empty_string", [""], prog="unlink", argv0=FULL_PATH),
    Case("unlink_dash", ["-"], (File("-", "D"),), prog="unlink", argv0=FULL_PATH),
    # /bin/unlink is a hard link to /bin/rm.
    Case("unlink_bin_direct", ["a"], F, prog="unlink", argv0=FULL_PATH),
    Case("unlink_bin_noargs", [], prog="unlink", argv0=FULL_PATH),
    Case("badopt_unlink_mode", ["-z"], prog="unlink"),
    Case("badopt_unlink_mode_two", ["-z", "a"], F, prog="unlink"),
    Case("unlink_mode_dash_v_file", ["-v"], prog="unlink"),
    Case("unlink_mode_dashdash_dashdash", ["--", "--"], (File("--", "D"),), prog="unlink"),
    Case("unlink_mode_trailing_slash", ["a/"], F, prog="unlink"),
    Case("unlink_mode_dot", ["."], prog="unlink"),
    Case("unlink_mode_slash", ["/"], prog="unlink"),
    Case("unlink_mode_uchg", ["a"], (File("a", "A", flags=UCHG),), prog="unlink"),
    Case("unlink_mode_ro_dir_parent", ["e/z"], (Dir("e", mode=0o555), File("e/z", "Z")), prog="unlink"),
]


@pytest.mark.parametrize("case", INVOCATION, ids=case_id)
def test_invocation_names(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- -i

ANSWERS = {
    "y": "y\n",
    "yes": "yes\n",
    "Y": "Y\n",
    "YES": "YES\n",
    "n": "n\n",
    "no": "no\n",
    "empty_line": "\n",
    "eof": "",
    "space_y": " y\n",
    "yx": "yx\n",
    "x": "x\n",
    "y_noeol": "y",
    "N": "N\n",
}
C_LOCALE = {"LANG": "C", "LC_ALL": "C"}

INTERACTIVE = [
    *(Case(f"i_file_pipe_{name}", ["-i", "a"], F, stdin=pipe(answer)) for name, answer in ANSWERS.items()),
    Case("i_file_devnull", ["-i", "a"], F, stdin=DEVNULL),
    Case("stdin_closed_i", ["-i", "a"], F, stdin=CLOSED),
    Case("i_file_pty_y", ["-i", "a"], F, stdin=pty("y\n")),
    Case("i_file_pty_n", ["-i", "a"], F, stdin=pty("n\n")),
    Case("i_file_pty_eof", ["-i", "a"], F, stdin=pty("\x04")),
    # Answers go through rpmatch(3), so the locale decides what counts as yes.
    Case("i_file_LANG_C_y", ["-i", "a"], F, stdin=pipe("y\n"), env=C_LOCALE),
    Case("i_file_LANG_C_yes", ["-i", "a"], F, stdin=pipe("yes\n"), env=C_LOCALE),
    Case("i_file_LANG_de_j", ["-i", "a"], F, stdin=pipe("j\n"), env={"LC_ALL": "de_DE.UTF-8"}),
    Case("i_file_LANG_fr_o", ["-i", "a"], F, stdin=pipe("o\n"), env={"LC_ALL": "fr_FR.UTF-8"}),
    Case("i_long_answer_line", ["-i", "a", "b"], F4, stdin=pipe("n" * 5000 + "\ny\n")),
    Case("i_crlf", ["-i", "a"], F, stdin=pipe("y\r\n")),
    Case("i_utf8_answer", ["-i", "a"], F, stdin=pipe("я\n")),
    Case("i_multi_mixed", ["-i", "a", "b", "c"], F3, stdin=pipe("y\nn\ny\n")),
    Case("i_multi_eof_early", ["-i", "a", "b", "c"], F3, stdin=pipe("y\n")),
    Case("iv_file_y", ["-iv", "a", "b"], F3, stdin=pipe("y\nn\n")),
    Case("i_missing", ["-i", "nope", "a"], F, stdin=pipe("y\n")),
    Case("i_symlink", ["-i", "l"], (File("a", "A"), Symlink("l", "a")), stdin=pipe("y\n")),
    Case("i_dangling_symlink", ["-i", "l"], (Symlink("l", "nowhere"),), stdin=pipe("y\n")),
    Case("i_fifo", ["-i", "p"], (Fifo("p"),), stdin=pipe("y\n")),
    Case("i_dir_no_r", ["-i", "d"], T, stdin=pipe("y\n")),
    Case(
        "i_prompt_odd_name",
        ["-i", "sp ace", "new\nline", "pct%s"],
        (File("sp ace"), File("new\nline"), File("pct%s")),
        stdin=pipe("n\nn\nn\n"),
    ),
    # Recursive: "examine files in directory X? " on the way down, "remove X? " on the way up.
    Case("id_emptydir_y", ["-id", "e"], (Dir("e"),), stdin=pipe("y\n")),
    Case("ir_tree_all_y", ["-ir", "d"], T, stdin=pipe("y\n" * 10)),
    Case("irv_tree_all_y", ["-irv", "d"], T, stdin=pipe("y\n" * 10)),
    Case("ir_tree_examine_n", ["-ir", "d"], T, stdin=pipe("n\n")),
    Case("ir_tree_examine_y_rest_n", ["-ir", "d"], T, stdin=pipe("y\n" + "n\n" * 10)),
    Case("ir_tree_eof", ["-ir", "d"], T, stdin=pipe("")),
    Case("ir_tree_y_then_eof", ["-ir", "d"], T, stdin=pipe("y\n")),
    Case("ir_tree_keep_one_file", ["-irv", "d"], T, stdin=pipe("y\ny\ny\nn\ny\ny\n")),
    Case("ir_emptydir", ["-ir", "e"], (Dir("e"),), stdin=pipe("y\ny\n")),
    Case("ir_emptydir_n_on_remove", ["-ir", "e"], (Dir("e"),), stdin=pipe("y\nn\n")),
    Case("ir_pty_all_y", ["-ir", "d"], T, stdin=pty("y\n" * 10)),
    Case("irv_examine_n_v", ["-irv", "d", "a"], T, stdin=pipe("n\ny\n")),
    Case("ird_tree", ["-ird", "d"], T, stdin=pipe("y\n" * 10)),
    Case("ir_trailing_slash", ["-ir", "d/"], T, stdin=pipe("y\n" * 10)),
    Case("ir_symlink_to_dir", ["-ir", "l"], (*T, Symlink("l", "d")), stdin=pipe("y\n")),
    Case("i_eof_examine_pty", ["-ir", "d"], T, stdin=pty("\x04")),
    # Between -f and -i the last one wins.
    Case("fi_file", ["-fi", "a"], F, stdin=pipe("n\n")),
    Case("if_file", ["-if", "a"], F, stdin=pipe("n\n")),
    Case("f_i_separate", ["-f", "-i", "a"], F, stdin=pipe("n\n")),
    Case("i_f_separate", ["-i", "-f", "a"], F, stdin=pipe("n\n")),
    Case("fi_missing", ["-fi", "nope"], stdin=pipe("n\n")),
    Case("if_missing", ["-if", "nope"], stdin=pipe("n\n")),
    Case("iI_file", ["-iI", "a"], F, stdin=pipe("n\n")),
    Case("Ii_file", ["-Ii", "a"], F, stdin=pipe("n\n")),
]


@pytest.mark.parametrize("case", INTERACTIVE, ids=case_id)
def test_interactive_i(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- -I

TWO_TREES = (*T, Dir("e"), File("e/z", "Z"))
FIVE_AND_DIR = (*T, File("c", "C"), File("e", "E"))

ONCE = [
    # Without -r the prompt comes only for more than three operands that exist.
    Case("I_3files", ["-I", "a", "b", "c"], F3, stdin=pipe("n\n")),
    Case("I_4files_n", ["-I", "a", "b", "c", "d"], F4, stdin=pipe("n\n")),
    Case("I_4files_y", ["-I", "a", "b", "c", "d"], F4, stdin=pipe("y\n")),
    Case("I_4files_eof", ["-I", "a", "b", "c", "d"], F4, stdin=pipe("")),
    Case("I_4files_pty_n", ["-I", "a", "b", "c", "d"], F4, stdin=pty("n\n")),
    Case("I_4files_devnull", ["-I", "a", "b", "c", "d"], F4, stdin=DEVNULL),
    Case("I_4operands_missing", ["-I", "a", "n1", "n2", "n3"], F, stdin=pipe("y\n")),
    Case("I_4_three_exist", ["-I", "a", "b", "c", "n"], F4, stdin=pipe("n\n")),
    Case("I_5_four_exist", ["-I", "a", "b", "c", "d", "n"], F4, stdin=pipe("n\n")),
    Case(
        "I_symlinks_4",
        ["-I", "l1", "l2", "l3", "l4"],
        tuple(Symlink(f"l{i}", "nowhere") for i in range(1, 5)),
        stdin=pipe("n\n"),
    ),
    Case("I_dir_nor", ["-I", "d"], T, stdin=pipe("n\n")),
    Case("I_dir_nor_plus_4", ["-I", "d", "a", "b", "c", "e"], FIVE_AND_DIR, stdin=pipe("n\n")),
    Case("I_4files_one_dot", ["-I", "a", "b", ".", "d"], tuple(File(n, n) for n in "abd"), stdin=pipe("n\n")),
    # With -r it prompts for any directory: "recursively remove d and 4 files? " etc.
    Case("I_r_dir_n", ["-rI", "d"], T, stdin=pipe("n\n")),
    Case("I_r_dir_y", ["-rIv", "d"], T, stdin=pipe("y\n")),
    Case("I_r_two_dirs_y", ["-rI", "d", "e"], TWO_TREES, stdin=pipe("y\ny\n")),
    Case("I_r_two_dirs_n_y", ["-rIv", "d", "e"], TWO_TREES, stdin=pipe("n\ny\n")),
    Case("I_r_file_only", ["-rI", "a"], F, stdin=pipe("n\n")),
    Case("I_r_emptydir", ["-rI", "e"], (Dir("e"),), stdin=pipe("n\n")),
    Case("I_r_dir_1file", ["-rI", "e"], (Dir("e"), File("e/z", "Z")), stdin=pipe("n\n")),
    Case("I_r_dir_2dirs", ["-rI", "e"], (Dir("e"), Dir("e/s"), Dir("e/t")), stdin=pipe("n\n")),
    Case("I_r_5files_and_dir", ["-rI", "a", "b", "c", "d", "e"], FIVE_AND_DIR, stdin=pipe("n\n")),
    Case("I_r_5files_and_dir_y", ["-rIv", "a", "b", "c", "d", "e"], FIVE_AND_DIR, stdin=pipe("y\ny\n")),
    Case("I_r_2dirs_1file", ["-rI", "d", "e", "a"], (*T, Dir("e")), stdin=pipe("n\n")),
    Case("I_r_1dir_1file", ["-rI", "d", "a"], T, stdin=pipe("n\n")),
    Case("I_r_3dirs_2files", ["-rI", "d", "e", "g", "a", "b"], (*T, Dir("e"), Dir("g")), stdin=pipe("n\n")),
    Case("I_r_trailing_slash_dir", ["-rI", "d/"], T, stdin=pipe("n\n")),
    Case("I_r_symlink_to_dir", ["-rI", "l"], (*T, Symlink("l", "d")), stdin=pipe("n\n")),
    Case("I_r_answer_yes_word", ["-rI", "d"], T, stdin=pipe("yes\n")),
    Case("I_r_pty_y", ["-rI", "d"], T, stdin=pty("y\n")),
    Case("I_r_missing_dir", ["-rI", "nope"], stdin=pipe("n\n")),
    Case("I_r_dot", ["-rI", ".", "d"], T, stdin=pipe("n\n")),
    Case("I_r_exit_code_yes_but_error", ["-rI", "d", "nope"], T, stdin=pipe("y\n")),
    # -I combined with -i and -f: -f does not silence the -I prompt.
    Case("I_and_i_r", ["-rIi", "d"], T, stdin=pipe("y\n" * 10)),
    Case("i_and_I_r_4files", ["-Ii", "a", "b", "c", "d"], F4, stdin=pipe("y\n" * 10)),
    Case("If_4files", ["-If", "a", "b", "c", "d"], F4, stdin=pipe("n\n")),
    Case("fI_4files", ["-fI", "a", "b", "c", "d"], F4, stdin=pipe("n\n")),
    Case("I_4files_devnull_f", ["-fI", "a", "b", "c", "d"], F4, stdin=DEVNULL),
]


@pytest.mark.parametrize("case", ONCE, ids=case_id)
def test_prompt_once_I(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- override prompts

# When stdin is a tty, a file rm cannot write (or one with user flags) gets
# "override <mode> <user>/<group> [flags ]for X? ". The owner names come from
# this machine; /bin/rm prints them live, so nothing here hardcodes them.
OVERRIDE = [
    Case("ro_file_devnull", ["a"], RO),
    Case("ro_file_devnull_v", ["-v", "a"], RO),
    Case("ro_file_pipe", ["a"], RO, stdin=pipe("n\n")),
    Case("ro_file_pty_n", ["a"], RO, stdin=pty("n\n")),
    Case("ro_file_pty_y", ["-v", "a"], RO, stdin=pty("y\n")),
    Case("ro_file_pty_eof", ["a"], RO, stdin=pty("\x04")),
    Case("ro_file_pty_yes_word", ["a"], RO, stdin=pty("yes\n")),
    Case("ro_file_pty_f", ["-f", "a"], RO, stdin=pty("n\n")),
    Case("ro_file_pty_i_n", ["-i", "a"], RO, stdin=pty("n\n")),
    Case("ro_file_pipe_i_n", ["-i", "a"], RO, stdin=pipe("n\n")),
    Case("i_ro_file_pty_y", ["-iv", "a"], RO, stdin=pty("y\n")),
    Case("ro_file_LANG_C_pty", ["a"], RO, stdin=pty("n\n"), env={"LC_ALL": "C"}),
    # The mode string is strmode() without the type: setuid/sticky show as S/T.
    *(
        Case(f"ro_file_{name}_pty", ["a"], (File("a", "A", mode=mode),), stdin=pty("n\n"))
        for name, mode in (("000", 0o000), ("555", 0o555), ("4444", 0o4444), ("1444", 0o1444))
    ),
    Case("ro_file_setgid_pty", ["a"], (File("a", "A", mode=0o2444),), stdin=pty("n\n")),
    Case("ro_file_sticky_x_pty", ["a"], (File("a", "A", mode=0o1555),), stdin=pty("n\n")),
    Case("wo_file_pty", ["a"], (File("a", "A", mode=0o200),), stdin=pty("n\n")),
    Case(
        "ro_files_pty_mixed",
        ["-v", "a", "b"],
        (File("a", "A", mode=0o444), File("b", "B", mode=0o400)),
        stdin=pty("n\ny\n"),
    ),
    Case("ro_symlink_pty", ["l"], (*RO, Symlink("l", "a")), stdin=pty("n\n")),
    Case("ro_dir_r_pty_n", ["-r", "e"], (Dir("e", mode=0o555), File("e/z", "Z")), stdin=pty("n\n")),
    Case("ro_dir_r_pty_y", ["-rv", "e"], (Dir("e", mode=0o555), File("e/z", "Z")), stdin=pty("y\ny\n")),
    Case("ro_emptydir_d_pty", ["-d", "e"], (Dir("e", mode=0o555),), stdin=pty("n\n")),
    Case("ro_emptydir_r_pty_y", ["-rv", "e"], (Dir("e", mode=0o555),), stdin=pty("y\n")),
    Case("ro_emptydir_r_pty_n", ["-rv", "e"], (Dir("e", mode=0o555),), stdin=pty("n\n")),
    Case("ro_emptydir_nor_pty", ["e"], (Dir("e", mode=0o555),), stdin=pty("y\n")),
    Case(
        "ro_file_in_tree_pty_n",
        ["-rv", "d"],
        (Dir("d"), File("d/x", "X", mode=0o444), File("d/w", "W")),
        stdin=pty("n\n"),
    ),
    Case("ro_file_r_pty", ["-rv", "d"], (Dir("d"), File("d/x", "X", mode=0o444)), stdin=pty("y\n")),
    Case("ro_file_r_pipe", ["-rv", "d"], (Dir("d"), File("d/x", "X", mode=0o444)), stdin=pipe("n\n")),
    Case("ro_file_rf_pty", ["-rfv", "d"], (Dir("d"), File("d/x", "X", mode=0o444)), stdin=pty("n\n")),
    # User flags: rm -f does not clear uchg/uappnd on this macOS.
    Case("uchg_file_pty_n", ["a"], (File("a", "A", flags=UCHG),), stdin=pty("n\n")),
    Case("uchg_file_pty_y", ["a"], (File("a", "A", flags=UCHG),), stdin=pty("y\n")),
    Case("uchg_ro_file_pty_n", ["a"], (File("a", "A", mode=0o444, flags=UCHG),), stdin=pty("n\n")),
    Case("uchg_file_f", ["-f", "a"], (File("a", "A", flags=UCHG),)),
    Case("uchg_file_devnull", ["a"], (File("a", "A", flags=UCHG),)),
    Case("i_uchg_file_pty_y", ["-i", "a"], (File("a", "A", flags=UCHG),), stdin=pty("y\n")),
    Case("uchg_uappnd_pty", ["a"], (File("a", "A", flags=("uchg", "uappnd")),), stdin=pty("n\n")),
    Case("uappnd_pty", ["a"], (File("a", "A", flags=("uappnd",)),), stdin=pty("n\n")),
    Case("hidden_flag_pty", ["a"], (File("a", "A", mode=0o444, flags=("hidden",)),), stdin=pty("n\n")),
    Case("nodump_flag_ro_pty", ["a"], (File("a", "A", mode=0o444, flags=("nodump",)),), stdin=pty("n\n")),
    Case("uchg_dir_r_pty", ["-rv", "e"], (Dir("e", flags=UCHG), File("e/z", "Z")), stdin=pty("y\n")),
    Case("uchg_file_rf", ["-rfv", "e"], (Dir("e"), File("e/z", "Z", flags=UCHG))),
]


@pytest.mark.parametrize("case", OVERRIDE, ids=case_id)
def test_override_prompts(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- '.', '..' and '/'

# All run with stdin a pipe holding "y\n", as the probes did, so a missing guard
# under -i shows up as well.
DOTS_TREE = (Dir("d"), File("d/x", "X"), File("a", "A"), Dir("e"), Dir("..x"), File("..x/q", "Q"))
DOT_ARGS = {
    "dot": ["."],
    "dotdot": [".."],
    "r_dot": ["-r", "."],
    "rf_dot": ["-rf", "."],
    "r_dotdot": ["-r", ".."],
    "rf_dot_and_file": ["-rf", ".", "a"],
    "r_file_and_dot": ["-r", "a", "."],
    "r_dir_dot": ["-r", "d/."],
    "r_dir_dotdot": ["-r", "d/.."],
    "r_dot_slash": ["-r", "./"],
    "r_a_dot_dot": ["-r", "a/./."],
    "r_d_dot_dot": ["-r", "d/./."],
    "r_dot_trailing_slash": ["-r", "./."],
    "r_dotslashslash": ["-r", ".//"],
    "r_dot_dot_slash": ["-r", "../"],
    "r_dir_dot_slash": ["-r", "d/./"],
    "d_dot": ["-d", "."],
    "f_dot": ["-f", "."],
    "f_dotdot": ["-f", ".."],
    "v_dot_file": ["-rv", ".", "a", ".."],
    "r_dotdir_name": ["-rv", "..x"],
    "r_dir_trailing": ["-rv", "d/"],
    "file_trailing_slash": ["a/"],
    "file_trailing_slash_f": ["-f", "a/"],
    "missing_trailing_slash": ["nope/"],
    "dir_trailing_slash_nor": ["d/"],
    "d_emptydir_trailing": ["-dv", "e/"],
    "r_dotdot_in_middle": ["-rv", "d/../d"],
    "i_dot": ["-i", "."],
    "ri_dot": ["-ri", "."],
}
DT = (Dir("d"), File("d/x", "X"), File("a", "A"))

# The "/" guard only matches the literal "/". Each run is sandboxed to its own
# box (see parity), so a program without the guard cannot do damage. The case
# `rm -rf //` is left out: /bin/rm walks the whole disk there, and its output is
# neither bounded nor deterministic.
GUARDS = [
    *(Case(name, args, DOTS_TREE, stdin=pipe("y\n")) for name, args in DOT_ARGS.items()),
    Case("r_dot_from_subdir", ["-r", "."], DT, cwd="d"),
    Case("r_dotdot_from_subdir", ["-rf", ".."], DT, cwd="d"),
    Case("r_dir_dot_from_parent_sb", ["-rv", "d/."], DT),
    Case("dotdot_nor", ["..", "a"], F),
    Case("dot_err_then_missing", ["-r", "nope", ".", "a"], F),
    Case("dotfile_named_dotdot_prefix", ["-v", "..a", ".a", "a."], (File("..a"), File(".a"), File("a."))),
    Case("dir_slash_dotdot_slash", ["-r", "d/../"], T),
    Case("slash_no_r", ["/"]),
    Case("slash_d", ["-d", "/"]),
    Case("slash_f", ["-f", "/"]),
    Case("slash_v", ["-v", "/", "a"], F),
    Case("slash_i", ["-i", "/", "a"], F, stdin=pipe("n\n")),
    Case("slash_rf_sandboxed", ["-rf", "/"]),
    Case("slash_r_sandboxed", ["-r", "/"]),
    Case("slash_rf_and_file_sandboxed", ["-rf", "a", "/"], F),
    Case("slash_rf_notsandbox_check", ["-rf", "a", "/"], F),
    Case("slash_dot_rf_sandboxed", ["-rf", "/."]),
    Case("slash_dotdot_rf_sandboxed", ["-rf", "/.."]),
    Case("slashslash_nor", ["//"]),
    Case("slashslash_d", ["-d", "//"]),
    Case("slash_dot_slash_nor", ["/./"]),
    Case("slash_and_dot", ["-rf", ".", "/"]),
    Case("dot_and_slash_order", ["-rf", "/", "."]),
]


@pytest.mark.parametrize("case", GUARDS, ids=case_id)
def test_dot_and_slash_guards(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)


# --------------------------------------------------------------------------- COMMAND_MODE=legacy

LEGACY = {"COMMAND_MODE": "legacy"}

LEGACY_CASES = [
    Case("legacy_missing_f", ["-f", "nope"], env=LEGACY),
    Case("legacy_noargs_f", ["-f"], env=LEGACY),
    Case("legacy_dot", ["-rf", "."], DT, env=LEGACY),
    Case("legacy_ro_devnull", ["a"], RO, env=LEGACY),
    Case("legacy_dir_no_r", ["d"], DT, env=LEGACY),
    Case("legacy_fi", ["-fi", "a"], F, stdin=pipe("n\n"), env=LEGACY),
    Case("legacy_i_missing", ["-i", "nope"], stdin=pipe("n\n"), env=LEGACY),
    Case("legacy_empty_string_f", ["-f", ""], env=LEGACY),
    Case("legacy_I_4files", ["-I", "a", "b", "c", "d"], F4, stdin=pipe("n\n"), env=LEGACY),
]


@pytest.mark.parametrize("case", LEGACY_CASES, ids=case_id)
def test_legacy_command_mode(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
    check(case, remmy_bin, tmp_path, is_root)
