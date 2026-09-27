"""BSD /bin/rm parity, part 3: edge cases the first two parts miss."""

from parity import *

T = (File("a", "A"), File("b", "B"), Dir("d"), File("d/x", "X"), Dir("d/sub"), File("d/sub/y", "Y"))
F4 = tuple(File(n, n.upper()) for n in "abcd")
LINKED = (Dir("t"), File("t/x", "x"), Symlink("l", "t"))
LEGACY = {"COMMAND_MODE": "legacy"}
PATH_PREFIX = "/".join(["abcdefghij"] * 92) + "/"  # 1012 bytes
PATH_1023 = PATH_PREFIX + "x" * 11
PATH_1024 = PATH_PREFIX + "x" * 12  # PATH_MAX counts the NUL, so this one is too long
DX = (Dir("d"), File("d/x"))


test_cwd_and_dot_shapes = parity_test(
    # The cwd is removed by absolute path; later relative operands then fail with ENOENT.
    Case("rm_cwd_abs_then_dot", "-rv {ROOT}/d x .", (Dir("d"), File("d/x", "X")), cwd="d"),
    Case("rm_cwd_ancestor_abs", "-rv {ROOT}/d", T, cwd="d/sub"),
    Case("rm_cwd_then_relative", "-rv {ROOT}/d sub/y ../a", T, cwd="d"),
    # The guard looks only at the last component.
    Case("dotdot_last_component_forms", "-r ./.. d/../.. ../. ... .../", (Dir("d"),)),
    Case("dotdot_after_file", "-rf a/..", (File("a", "A"),)),
    Case("dotdot_after_missing", "-rf nope/.."),
    Case("dot_after_missing", "-r nope/."),
    Case("dot_after_symlink_to_dir", "-rv l/.", LINKED),
    Case("slashes_only_nor", "///"),
    Case("slashes_only_d", "-d ///"),
    Case("slashes_only_f", "-f ///"),
)


test_symlinks_to_directories = parity_test(
    # -d on the bare link removes the link; with a slash it rmdirs the target.
    Case("d_symlink_to_dir", "-dv l", LINKED),
    Case("d_symlink_to_nonempty_dir_slash", "-dv l/", LINKED),
    # Every trailing slash is kept in -v output, joined with one more.
    Case("rd_symlink_to_dir_two_slashes", "-rdv l//", LINKED),
    Case("r_symlink_to_file_slash", "-rv l/", (File("t", "x"), Symlink("l", "t"))),
    Case("r_dangling_symlink_slash", "-rv l/", (Symlink("l", "nowhere"),)),
    Case("rf_dangling_symlink_slash", "-rfv l/", (Symlink("l", "nowhere"),)),
    # A link to '.' or '..' walks back into the tree that holds it.
    Case("r_symlink_to_dot_slash", "-rv l/", (Symlink("l", "."), File("a", "x"))),
    Case("r_symlink_to_dotdot_slash", "-rv d/l/", (Dir("d"), Symlink("d/l", ".."), File("a", "x"))),
    Case("i_symlink_to_dir_nor", "-i l", LINKED, stdin=Y),
    Case("ir_symlink_to_dir_slash", "-irv l/", LINKED, stdin=pipe("y\n" * 6)),
)


test_length_boundaries = parity_test(
    Case("path_1023_missing", [PATH_1023]),
    Case("path_1023_missing_f", ["-f", PATH_1023]),
    Case("path_1023_missing_r", ["-r", PATH_1023]),
    Case("path_1024_missing", [PATH_1024]),
    # ENAMETOOLONG is not ENOENT, so -f still reports it and exits 1.
    Case("path_1024_missing_f", ["-f", PATH_1024]),
    Case("path_1024_missing_r", ["-r", PATH_1024]),
    Case("name_255_missing", ["b" * 255]),
    Case("name_256_in_dir_f", ["-f", "d/" + "b" * 256], (Dir("d"),)),
)


test_prompt_once_I = parity_test(
    Case("I_4files_pty_eof", "-I a b c d", F4, stdin=pty("\x04")),
    Case("I_r_pty_eof", "-rI d", T, stdin=pty("\x04")),
    Case("I_4files_closed", "-I a b c d", F4, stdin=CLOSED),
    Case("I_r_closed", "-rI d", DX, stdin=CLOSED),
    Case("Iv_4files_y", "-Iv a b c d", F4, stdin=Y),
    # Duplicates count as separate operands; declining the prompt exits 1.
    Case("I_same_file_4_times", "-I a a a a", F4, stdin=N),
    Case("I_12files", ["-I", *(f"f{i}" for i in range(12))], tuple(File(f"f{i}") for i in range(12)), stdin=N),
    Case("I_r_dir_and_file_y", "-rI d a", T, stdin=Y),
    Case(
        "I_r_wide_dir",
        "-rI w",
        (Dir("w"), *(File(f"w/f{i}") for i in range(30)), Dir("w/s"), File("w/s/q")),
        stdin=N,
    ),
    # A yes to -I does not stop the per-file override prompt on a tty.
    Case("I_r_then_override_pty", "-rIv d", (Dir("d"), File("d/x", mode=0o444), File("d/y")), stdin=pty("y\nn\n")),
    Case(
        "I_4files_then_override_pty",
        "-Iv a b c d",
        (File("a", mode=0o444), File("b"), File("c"), File("d")),
        stdin=pty("y\ny\n"),
    ),
    Case("I_4files_ro_pipe", "-I a b c d", (File("a", mode=0o444), File("b"), File("c"), File("d")), stdin=Y),
    Case("I_r_pty_n", "-rI d", DX, stdin=TN),
    Case("I_r_legacy", "-rI d", DX, stdin=N, env=LEGACY),
)


test_interactive_i = parity_test(
    Case("i_pty_three_answers", "-iv a b c", F4, stdin=pty("y\nn\nyes\n")),
    Case("i_pty_leading_spaces", "-i a", F4, stdin=pty("  y\n")),
    Case("i_pipe_leading_tab", "-i a", F4, stdin=pipe("\ty\n")),
    Case("i_pipe_yes_sentence", "-i a b", F4, stdin=pipe("yes please\nnope\n")),
    Case("i_pipe_nul_in_answer", "-i a b", F4, stdin=pipe(b"y\x00\nn\n")),
    Case("i_pipe_eof_mid_line", "-i a b", F4, stdin=pipe("n\ny")),
    Case("i_raw_names_in_prompt", ["-i", "e\x1bx", "n\nl"], (File("e\x1bx"), File("n\nl")), stdin=pipe("n\nn\n")),
    Case("ir_closed_stdin", "-ir d", DX, stdin=CLOSED),
    Case("id_nonempty_dir", "-id d", T, stdin=Y),
    Case("idv_empty_dir", "-idv e", (Dir("e"),), stdin=Y),
    Case("id_empty_dir_n", "-id e", (Dir("e"),), stdin=N),
    Case("ir_unreadable_subdir", "-ir d", (Dir("d"), Dir("d/s", mode=0o000), File("d/s/x")), stdin=pipe("y\n" * 6)),
    Case(
        "irv_special_files_in_tree", "-irv d", (Dir("d"), Symlink("d/l", "nowhere"), Fifo("d/p")), stdin=pipe("y\n" * 6)
    ),
    # Legacy mode asks "remove d?" rather than "examine files in directory d?".
    Case("ir_legacy", "-ir d", DX, stdin=pipe("y\ny\ny\n"), env=LEGACY),
    Case("ro_file_pty_legacy", "a", (File("a", "A", mode=0o444),), stdin=TN, env=LEGACY),
    Case("ro_file_in_tree_closed", "-rv d", (Dir("d"), File("d/x", mode=0o444)), stdin=CLOSED),
)


test_P_and_W = parity_test(
    Case("P_hardlink", "-Pv a", (File("a", "hello"), Hardlink("b", "a"))),
    Case("P_fifo", "-Pv p", (Fifo("p"),)),
    Case("P_uchg", "-P a", (File("a", "A", flags=("uchg",)),)),
    Case("P_mode_000", "-P a", (File("a", "A", mode=0o000),)),
    Case("P_large_file", "-Pv a", (File("a", "A" * 100000),)),
    Case("P_empty_file", "-Pv a", (File("a", ""),)),
    Case("P_i_n", "-Pi a", (File("a", "A"),), stdin=N),
    Case("iW_missing", "-iW nope", stdin=Y),
    Case("iW_existing", "-iW a", (File("a", "A"),), stdin=Y),
)


test_unlink_mode = parity_test(
    Case("unlink_dotdot", "..", prog="unlink"),
    Case("unlink_symlink_to_dir", "l", LINKED, prog="unlink"),
    Case("unlink_symlink_to_dir_slash", "l/", LINKED, prog="unlink"),
    Case("unlink_fifo", "p", (Fifo("p"),), prog="unlink"),
    Case("unlink_dangling_symlink", "l", (Symlink("l", "nowhere"),), prog="unlink"),
    # Unlink mode never prompts, not even for a read-only file on a tty.
    Case("unlink_ro_devnull", "a", (File("a", "A", mode=0o444),), prog="unlink"),
    Case("unlink_ro_pty_y", "a", (File("a", "A", mode=0o444),), prog="unlink", stdin=TY),
    Case("unlink_name_too_long", ["a" * 256], prog="unlink"),
    Case("unlink_dashdash_dash_z", "-- -z", prog="unlink"),
    Case("unlink_dashdash_prefixed_name", "--x", prog="unlink"),
    Case("unlink_dash_then_second", "- a", (File("a"), File("-")), prog="unlink"),
    Case("unlink_escaped_name", ["e\x1bx"], prog="unlink"),
    Case("unlink_legacy_dir", "d", (Dir("d"),), prog="unlink", env=LEGACY),
    Case("unlink_legacy_missing", "nope", prog="unlink", env=LEGACY),
    # Only `unlink file` and `unlink -- file` skip the "." / ".." / "/" guards;
    # rm rejects every other shape with its usage. An option must never make
    # remmy walk one of these operands.
    Case("unlink_dashdash_dot", "-- .", T, prog="unlink"),
    Case("unlink_rf_dot", "-rf .", T, prog="unlink"),
    Case("unlink_r_dotdot_from_subdir", "-r ..", T, cwd="d", prog="unlink"),
    Case("unlink_r_nested_dotdot", "-r d/sub/..", T, prog="unlink"),
    Case("unlink_rf_slash_sandboxed", "-rf /", prog="unlink"),
    Case("unlink_argv0_rf_dot", "-rf .", T, argv0="unlink"),
)


test_misc = parity_test(
    Case("rv_raw_names_in_tree", "-rv d", (Dir("d"), File("d/a\nb"), Dir("d/c\x1bd"), File("d/c\x1bd/x"))),
    Case("r_error_newline_name_in_tree", "-r d", (Dir("d"), Dir("d/a\nb", mode=0o555), File("d/a\nb/x"))),
    Case("legacy_dotdot", "-r ..", env=LEGACY),
    Case("legacy_slash", "-rf /", env=LEGACY),
    Case("legacy_badopt", "-z", env=LEGACY),
)


test_mount_operands = parity_test(
    # rmdir of a mount point is EBUSY; -r empties the volume first.
    Case("d_mountpoint", "-dv m", (Mount("m"),)),
    Case("r_mountpoint_operand", "-rv m", (Mount("m"), File("m/inner", "x"))),
    Case("rf_mountpoint_operand", "-rfv m", (Mount("m"), Dir("m/s"), File("m/s/inner", "x"))),
    Case("rx_empty_nested_mount", "-rxv d", (Dir("d"), Mount("d/mnt"))),
    slow=True,
)
