"""BSD /bin/rm parity, part 2: filesystem semantics and error text.

Ids the CLI module also uses carry an fs_ prefix. -v output and errors inside a directory follow raw readdir order
(fts has no comparator): not alphabetical, but deterministic on the same filesystem, so everything compares exactly.
"""

import os
import unicodedata

from parity import *

UCHG = ("uchg",)
TREE = (Dir("d"), File("d/a", "x"), File("d/b", "x"), Dir("d/sub"), File("d/sub/c", "x"), Dir("d/sub/e"))
LONG = "a" * 256
TOO_LONG_PATH = "/".join(["abcdefghij"] * 100)  # 1099 bytes, over PATH_MAX (1024)
NFC = unicodedata.normalize("NFC", "café")
NFD = unicodedata.normalize("NFD", "café")
NONUTF8 = os.fsdecode(b"bad\xff\xfename")
DEEP_NAME = "n" * 50
DEEP_OPERAND = "d/" + "/".join([DEEP_NAME] * 25) + "/leaf"  # ~1300 bytes, over PATH_MAX
D = (Dir("d"),)
FF = (File("f", "x"),)
DX = (Dir("d"), File("d/x", "x"))
FA = (File("a", "x"),)
FG = (File("f", "x"), File("g", "x"))
LOCKED_F = (Dir("d"), Dir("d/s", mode=0o000), File("d/s/f", "x"))
RO_DIR = (Dir("d", mode=0o555), File("d/f", "x"))
LINK_F = (File("t", "x"), Symlink("l", "t"))
LOOP = (Symlink("a", "b"), Symlink("b", "a"))


test_missing_and_directories = parity_test(
    Case("fs_missing_file", "nope"),
    Case("fs_missing_file_f", "-f nope"),
    Case("missing_file_r", "-r nope"),
    Case("missing_file_rf", "-rf nope"),
    Case("missing_file_v", "-v nope"),
    Case("fs_missing_and_present", "a nope c", (File("a", "x"), File("c", "x"))),
    Case("fs_missing_and_present_f", "-f a nope c", (File("a", "x"), File("c", "x"))),
    Case("missing_and_present_v", "-v a nope c", (File("a", "x"), File("c", "x"))),
    Case("missing_in_missing_dir", "nodir/x"),
    Case("missing_in_missing_dir_f", "-f nodir/x"),
    Case("empty_string_operand", [""]),
    Case("empty_string_operand_f", ["-f", ""]),
    # -f hides ENOENT only; every other error still prints and still sets exit 1.
    Case("only_errors_f_exit", "-f nope a/", FA),
    Case("fs_dir_no_r", "d", D),
    Case("fs_dir_no_r_f", "-f d", D),
    Case("fs_dir_no_r_v", "-v d", D),
    Case("nonempty_dir_no_r", "d", DX),
    Case("empty_dir_d", "-d d", D),
    Case("empty_dir_dv", "-dv d", D),
    Case("nonempty_dir_d", "-d d", DX),
    Case("nonempty_dir_df", "-df d", DX),
    Case("file_with_d", "-d f", FF),
    Case("rd_tree", "-rd d", TREE),
    Case("rd_nested_empty_dirs", "-dv a", (Dir("a/b/c"), Dir("a/e"))),
    Case("rdv_nested", "-rdv a", (Dir("a/b/c"), Dir("a/e"))),
    Case("mixed_no_r_dir_and_files", "-v a d b", (File("a", "x"), Dir("d"), File("b", "x"))),
)


test_path_shapes = parity_test(
    Case("dir_trailing_slash_d", "-d d/", D),
    Case("dir_trailing_slash_r", "-rv d/", DX),
    Case("dir_trailing_slashes_r", "-rv d//", DX),
    Case("dir_trailing_slash_no_r", "d/", D),
    Case("fs_file_trailing_slash", "f/", FF),
    Case("fs_file_trailing_slash_f", "-f f/", FF),
    Case("file_trailing_slash_r", "-r f/", FF),
    Case("fifo_trailing_slash", "p/", (Fifo("p"),)),
    Case("socket_trailing_slash", "s/", (Socket("s"),)),
    Case("path_through_file", "a/b", FA),
    Case("path_through_file_f", "-f a/b", FA),
    Case("path_through_file_r", "-r a/b", FA),
    # With -r and -f, rm's fts walk passes over an operand it cannot stat without a word, whatever the error.
    Case("file_trailing_slash_rf", "-rf f/", FF),
    Case("file_trailing_slash_Rfv", "-Rfv f/", FF),
    Case("file_trailing_slash_rif", "-rif f/", FF),
    Case("file_trailing_slash_rfi", "-rfi f/", FF),
    Case("path_through_file_rf", "-rf a/b", FA),
    Case("rf_unstatable_then_file", "-rf f/ a/b g", (File("f", "x"), File("a", "x"), File("g", "x"))),
    Case("relative_and_absolute", "-v f {ROOT}/f", FF),
    Case("absolute_path_v", "-rv {ROOT}/d", DX),
    Case("absolute_missing", "{ROOT}/nope"),
    Case("dot_slash_prefix", "-rv ./d", DX),
    Case("dotdot_path", "-rv ../e", (Dir("d"), Dir("e"), File("e/x", "x")), cwd="d"),
    Case("redundant_slashes_in_path", "-v d//x", DX),
    Case("dot_component_path", "-v d/./x", DX),
    Case("rm_dotfile_not_dot", "-v .x ..y ...", (File(".x", "x"), File("..y", "x"), File("...", "x"))),
)


test_dot_operands = parity_test(
    Case("rm_dot", "-r .", D, cwd="d"),
    Case("rm_dotdot", "-r ..", D, cwd="d"),
    Case("rm_dot_f", "-rf .", D, cwd="d"),
    Case("rm_dot_no_r", ".", D, cwd="d"),
    Case("rm_dot_slash", "-r ./", D, cwd="d"),
    Case("rm_dotdot_slash", "-r ../", D, cwd="d"),
    Case("rm_dot_and_file", "-rv . x", DX, cwd="d"),
    Case("rm_dot_mixed_with_missing", "-rf nope . .. nope2", D, cwd="d"),
    Case("rm_dir_slash_dot", "-rv d/.", DX),
    Case("rm_dir_slash_dotdot", "-rv d/e/..", (Dir("d"), Dir("d/e"))),
    Case("rm_dir_dot_slash", "-r d/./", DX),
)


test_length_limits = parity_test(
    Case("name_too_long", [LONG]),
    Case("name_too_long_f", ["-f", LONG]),
    Case("name_too_long_r", ["-r", LONG]),
    Case("name_too_long_rf", ["-rf", LONG]),
    Case("path_too_long_missing", [TOO_LONG_PATH]),
    Case("path_too_long_missing_f", ["-f", TOO_LONG_PATH]),
    Case("path_too_long_missing_rf", ["-rf", TOO_LONG_PATH]),
    Case("name_255_ok", ["-v", "b" * 255], (File("b" * 255, "x"),)),
    # The leaf is ~2040 bytes deep, past PATH_MAX; fts still removes it.
    Case("deep_tree_beyond_pathmax", "-r d", (DeepChain("d", DEEP_NAME, 40),)),
    Case("deep_tree_beyond_pathmax_v", "-rv d", (DeepChain("d", DEEP_NAME, 25),)),
    Case("deep_existing_operand_too_long", [DEEP_OPERAND], (DeepChain("d", DEEP_NAME, 25),)),
    Case("deep_existing_operand_too_long_f", ["-f", DEEP_OPERAND], (DeepChain("d", DEEP_NAME, 25),)),
)


TARGET = (Dir("t"), File("t/x", "x"), Symlink("l", "t"))

test_links_and_special_files = parity_test(
    Case("symlink_to_dir", "l", TARGET),
    Case("symlink_to_dir_slash", "l/", TARGET),
    Case("symlink_to_dir_r", "-rv l", TARGET),
    # A trailing slash follows the link: the target is emptied and rmdir'd, the link stays.
    Case("symlink_to_dir_slash_r", "-rv l/", TARGET),
    Case("symlink_to_dir_slash_d", "-dv l/", (Dir("t"), Symlink("l", "t"))),
    Case(
        "symlink_to_dir_slash_rf_leaves_symlink",
        "-rfv l/",
        (Dir("t"), Dir("t/s"), File("t/s/y", "x"), Symlink("l", "t")),
    ),
    Case("symlink_to_file", "-v l", LINK_F),
    Case("symlink_to_file_slash", "l/", LINK_F),
    Case("dangling_symlink", "-v l", (Symlink("l", "nowhere"),)),
    Case("dangling_symlink_slash", "l/", (Symlink("l", "nowhere"),)),
    Case("symlink_loop", "-v a", LOOP),
    Case("symlink_loop_slash", "a/", LOOP),
    Case("symlink_loop_slash_rf", "-rf a/", LOOP),
    Case("symlink_to_file_slash_rf", "-rf l/", LINK_F),
    Case("symlink_self_loop_r", "-rv s", (Symlink("s", "s"),)),
    Case("symlink_inside_tree_r", "-rv d", (Dir("t"), File("t/x", "x"), Dir("d"), Symlink("d/l", "../t"))),
    Case("symlink_through_operand", "-v l/x", TARGET),
    Case("fifo", "-v p", (Fifo("p"),)),
    Case("socket", "-v s", (Socket("s"),)),
    Case("fifo_socket_in_tree", "-r d", (Dir("d"), Fifo("d/p"), Socket("d/s"))),
    Case("hardlinks", "-v a", (File("a", "hello"), Hardlink("b", "a"))),
    Case("hardlinks_both", "-v a b", (File("a", "hello"), Hardlink("b", "a"))),
    Case("hardlink_in_tree", "-rv d", (File("a", "hello"), Dir("d"), Hardlink("d/b", "a"))),
)


LOCKED_SUB = (Dir("d"), File("d/a", "x"), Dir("d/s", mode=0o000), File("d/s/x", "x"), File("d/z", "x"))
RO_F = (File("f", "x", mode=0o444),)

test_permissions = parity_test(
    Case("file_in_0555_dir", "d/f", RO_DIR),
    Case("file_in_0555_dir_f", "-f d/f", RO_DIR),
    Case("file_in_0555_dir_v", "-v d/f", RO_DIR),
    Case("rf_0555_dir", "-rf d", RO_DIR),
    Case("r_0555_dir", "-rv d", (Dir("d", mode=0o555), File("d/f", "x"), File("d/g", "x"))),
    Case("r_subdir_0000", "-r d", LOCKED_SUB),
    Case("rv_subdir_0000", "-rv d", LOCKED_SUB),
    Case("rf_subdir_0000", "-rf d", LOCKED_SUB),
    Case("r_empty_subdir_0000", "-rv d", (Dir("d"), Dir("d/s", mode=0o000))),
    Case("r_operand_0000", "-r d", (Dir("d", mode=0o000), File("d/x", "x"))),
    Case("r_empty_operand_0000", "-rv d", (Dir("d", mode=0o000),)),
    # -f still rmdirs a directory it cannot read, silently when that works.
    Case("rf_empty_subdir_0000", "-rf d", (Dir("d"), Dir("d/s", mode=0o000))),
    Case("rfv_empty_subdir_0000", "-rfv d", (Dir("d"), Dir("d/s", mode=0o000))),
    Case("rf_empty_nested_0000", "-rf d", (Dir("d"), Dir("d/s"), Dir("d/s/t", mode=0o000))),
    Case("rf_empty_operand_0000", "-rf d", (Dir("d", mode=0o000),)),
    Case("rf_operand_0000", "-rf d", (Dir("d", mode=0o000), File("d/x", "x"))),
    Case("rf_empty_subdir_0300", "-rf d", (Dir("d"), Dir("d/s", mode=0o300))),
    Case("r_subdir_0555", "-r d", (Dir("d"), Dir("d/s", mode=0o555), File("d/s/x", "x"), File("d/s/y", "x"))),
    Case("rf_subdir_0555", "-rf d", (Dir("d"), Dir("d/s", mode=0o555), File("d/s/x", "x"), File("d/s/y", "x"))),
    Case("r_subdir_0333", "-rv d", (Dir("d"), Dir("d/s", mode=0o333), File("d/s/x", "x"))),
    Case("r_subdir_0444", "-rv d", (Dir("d"), Dir("d/s", mode=0o444), File("d/s/x", "x"))),
    Case(
        "r_nested_0000_two",
        "-r d",
        (
            Dir("d"),
            Dir("d/a", mode=0o000),
            File("d/a/x", "x"),
            Dir("d/b", mode=0o000),
            File("d/b/y", "x"),
            File("d/c", "x"),
        ),
    ),
    Case("r_subdir_of_0555_parent", "-rv d/s", (Dir("d", mode=0o555), Dir("d/s"), File("d/s/x", "x"))),
    Case(
        "r_tree_with_0444_files_devnull",
        "-rv d",
        (Dir("d"), File("d/a", "x", mode=0o444), File("d/b", "x", mode=0o000)),
    ),
    Case("lookup_through_0000_dir", "d/s/f", LOCKED_F),
    Case("lookup_through_0000_dir_f", "-f d/s/f", LOCKED_F),
    Case("lookup_missing_through_0000_dir_f", "-f d/s/nope", (Dir("d"), Dir("d/s", mode=0o000))),
    Case("lookup_through_0000_dir_rf", "-rf d/s/f", LOCKED_F),
    Case("lookup_through_0000_dir_r", "-r d/s/f", LOCKED_F),
    # Read-only files: only a tty on stdin gets the override prompt.
    Case("file_0444_devnull", "-v f", RO_F),
    Case("file_0444_pipe", "f", RO_F, stdin=N),
    Case("file_0444_closed_stdin", "f", RO_F, stdin=CLOSED),
    Case("file_0444_pty_no", "f", RO_F, stdin=TN),
    Case("file_0444_pty_yes", "f", RO_F, stdin=TY),
    Case("file_0444_pty_f", "-f f", RO_F, stdin=TN),
    Case("tree_0444_files_r_pty", "-r d", (Dir("d"), File("d/f", "x", mode=0o444)), stdin=TN),
    Case("file_0000_devnull", "-v f", (File("f", "x", mode=0o000),)),
    Case("symlink_to_0444_pty", "l", (File("t", "x", mode=0o444), Symlink("l", "t")), stdin=TN),
)


UCHG_F = (File("f", "x", flags=UCHG),)
UAPPND_F = (File("f", "x", flags=("uappnd",)),)
UCHG_LINK = (File("t", "x"), Symlink("l", "t", flags=UCHG))

test_file_flags = parity_test(
    Case("uchg_file", "f", UCHG_F),
    Case("uchg_file_v", "-v f", UCHG_F),
    Case("fs_uchg_file_f", "-f f", UCHG_F),
    Case("uchg_file_fv", "-fv f", UCHG_F),
    Case("uappnd_file", "f", UAPPND_F),
    Case("uappnd_file_f", "-f f", UAPPND_F),
    Case("uchg_file_pty", "f", UCHG_F, stdin=TY),
    Case("uchg_file_pty_no", "f", UCHG_F, stdin=TN),
    Case("uchg_file_0444_pty", "f", (File("f", "x", mode=0o444, flags=UCHG),), stdin=TY),
    Case("uchg_in_tree_r", "-r d", (Dir("d"), File("d/a", "x"), File("d/f", "x", flags=UCHG))),
    Case("uchg_in_tree_rf", "-rfv d", (Dir("d"), File("d/a", "x"), File("d/f", "x", flags=UCHG))),
    Case("uchg_dir_r", "-r d", (Dir("d", flags=UCHG), File("d/a", "x"))),
    Case("uchg_dir_rf", "-rfv d", (Dir("d", flags=UCHG), File("d/a", "x"))),
    Case("uchg_empty_dir_d", "-d d", (Dir("d", flags=UCHG),)),
    Case("uchg_empty_dir_df", "-df d", (Dir("d", flags=UCHG),)),
    Case("uchg_subdir_rf", "-rfv d", (Dir("d"), Dir("d/s", flags=UCHG), File("d/s/x", "x"))),
    Case("uappnd_dir_rf", "-rfv d", (Dir("d", flags=("uappnd",)), File("d/a", "x"))),
    Case("uchg_symlink_f", "-fv l", UCHG_LINK),
    Case("uchg_symlink", "-v l", UCHG_LINK),
    Case("symlink_to_uchg_file", "-v l", (File("t", "x", flags=UCHG), Symlink("l", "t"))),
    Case("uchg_file_in_0555_dir_f", "-f d/f", (Dir("d", mode=0o555), File("d/f", "x", flags=UCHG))),
)


test_recursion_and_order = parity_test(
    Case("fs_rv_tree", "-rv d", TREE),
    Case("r_tree_quiet", "-r d", TREE),
    # 200 siblings: -v order is readdir order (f034, f033, f005, ...), not sorted.
    Case("rv_wide", "-rv w", (Dir("w"), *(File(f"w/f{i:03d}", "x") for i in range(200)))),
    Case("rv_deep", "-rv d", (Dir("/".join(["d"] * 29)), File("/".join(["d"] * 29) + "/leaf", "x"))),
    Case("rv_mixed_operands_fail", "-rv a nope d f/ e", (File("a", "x"), Dir("d"), File("d/x", "x"), Dir("e"))),
    Case("duplicate_operands", "-v f f", FF),
    Case("duplicate_operands_f", "-fv f f", FF),
    Case("duplicate_dir_operands_r", "-rv d d", DX),
    Case("overlap_parent_then_child", "-rv d d/x", (Dir("d"), File("d/x", "x"), File("d/y", "x"))),
    Case("overlap_child_then_parent", "-rv d/x d", (Dir("d"), File("d/x", "x"), File("d/y", "x"))),
)


CONTROL_SWEEP = [os.fsdecode(b"m" + bytes([b]) + b"z") for b in [*range(1, 32), 0x7F, 0x80, 0xFF]]

# Errors on stderr escape C0 controls vis-style, except TAB and LF. -v output
# and prompts print names raw.
test_names = parity_test(
    Case("name_with_space", ["-v", "a b", "no such"], (File("a b", "x"),)),
    Case("name_with_newline", ["-v", "a\nb", "no\nsuch"], (File("a\nb", "x"),)),
    Case("name_with_tab_esc", ["-v", "a\tb", "e\x1bx", "m\x1bissing"], (File("a\tb", "x"), File("e\x1bx", "x"))),
    # APFS refuses names that are not UTF-8, so only missing operands can be tested.
    Case("name_non_utf8_missing", [NONUTF8, os.fsdecode(b"miss\xff"), os.fsdecode(b"x\xc3")]),
    Case("name_non_utf8_missing_f", ["-f", NONUTF8]),
    Case("control_bytes_missing_sweep", CONTROL_SWEEP),
    Case("control_bytes_missing_sweep_C_locale", ["a\x1bb", "café", os.fsdecode(b"x\xffy")], env={"LC_ALL": "C"}),
    Case("esc_name_dir_no_r", ["e\x1bd"], (Dir("e\x1bd"),)),
    Case(
        "esc_name_v_stdout",
        ["-v", "e\x1bf", "c\rr", "b\x07l"],
        (File("e\x1bf", "x"), File("c\rr", "x"), File("b\x07l", "x")),
    ),
    Case("esc_name_in_tree_perm", "-r d", (Dir("d"), Dir("d/s\x1bt", mode=0o000), File("d/s\x1bt/x", "x"))),
    Case("esc_name_in_0555_dir", ["d\x1b/f\x01"], (Dir("d\x1b", mode=0o555), File("d\x1b/f\x01", "x"))),
    Case("esc_name_prompt_pty", ["e\x1bf"], (File("e\x1bf", "x", mode=0o444),), stdin=TN),
    Case("esc_name_trailing_slash", ["e\x1bf/"], (File("e\x1bf", "x"),)),
    Case("esc_name_dot_dir", ["-r", "e\x1bd/."], (Dir("e\x1bd"),)),
    Case("name_nfc_created_nfd_operand", ["-v", NFD], (File(NFC, "x"),)),
    Case("name_nfd_created_nfc_operand", ["-v", NFC], (File(NFD, "x"),)),
    Case("name_nfc_in_tree_v", "-rv d", (Dir("d"), File("d/" + NFC, "x"))),
    Case("name_unicode_missing", "日本"),
    Case("case_insensitive_operand", "-v foo", (File("Foo", "x"),)),
    Case("dash_name_double_dash", "-v -- -x", (File("-x", "x"),)),
    Case("dash_name_dot_slash", "-v ./-x", (File("-x", "x"),)),
    Case("dash_name_without_dd", "-x", (File("-x", "x"),)),
    Case("single_dash_operand", "-v -", (File("-", "x"),)),
)


MOUNTED = (Dir("d"), File("d/a", "x"), Mount("d/mnt"), File("d/mnt/inner", "x"))

test_mount_points = parity_test(
    Case("x_mountpoint_r", "-rxv d", MOUNTED),
    Case("mountpoint_r_no_x", "-rv d", MOUNTED),
    Case("x_operand_is_mount", "-rxv m", (Mount("m"), File("m/inner", "x"))),
    slow=True,
)


PLAIN = (File("f", "x"), Dir("d"))

test_stdin_kinds = parity_test(
    Case("stdin_closed_plain", "-v f d", PLAIN, stdin=CLOSED),
    Case("stdin_pipe_plain", "-v f d", PLAIN, stdin=Y),
    Case("stdin_pty_plain", "-v f d", PLAIN, stdin=pty()),
)


# The prefix comes from the basename of the file executed; unlink mode comes
# from the basename of argv[0]. The two are tested apart here.
test_invocation_names = parity_test(
    Case("argv0_symlink_rm", "d nope", D, argv0=FULL_PATH),
    Case("argv0_symlink_remmy", "d nope", D, prog="remmy", argv0=FULL_PATH),
    Case("argv0_symlink_remmy_usage", [], prog="remmy", argv0=FULL_PATH),
    Case("argv0_symlink_unlink", "f", FG, **UNLINK),
    Case("argv0_symlink_unlink_dir", "d", D, **UNLINK),
    Case("argv0_symlink_unlink_two", "f g", FG, **UNLINK),
    Case("argv0_symlink_unlink_missing", "nope", **UNLINK),
    Case("argv0_symlink_unlink_flag", "-f f", FF, **UNLINK),
    Case("argv0_symlink_unlink_dd", "-- -f", (File("-f", "x"),), **UNLINK),
    Case("argv0_symlink_unlink_noargs", [], **UNLINK),
    Case("argv0_arbitrary_name", "d nope", D, argv0="frobnicate"),
    Case("argv0_path_to_unlink_name", "f", FF, argv0="/some/where/unlink"),
    Case("exe_remmy_argv0_rm", "d nope", D, prog="remmy", argv0="rm"),
    Case("exe_unlink_argv0_frob_two_operands", "f g nope", FG, prog="unlink", argv0="frob"),
    Case("exe_rm_argv0_unlink_two_operands", "f g", FG, argv0="unlink"),
    Case("exe_rm_argv0_unlink_one_missing", "nope", argv0="/x/y/unlink"),
    Case("exe_rm_argv0_unlink_dir", "d", D, argv0="unlink"),
)
