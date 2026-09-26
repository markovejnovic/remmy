"""Ledger of the parity cases remmy fails today, one entry per underlying gap.

Every case in ``test_rm_parity_*.py`` that does not yet match ``/bin/rm`` is listed
under exactly one gap here, and ``conftest.py`` marks it as a *strict* xfail. Cases
that already match carry no mark. So against remmy the parity suite is all green
(passed or xfailed), and:

* a fix that makes a listed case pass turns it into XPASS, which strict makes a
  failure: the fix PR must delete those ids from the ledger, and the ledger can
  never claim a gap that is gone;
* a regression in a case that passes today fails outright.

``GAPS`` is ordered the way the gaps should be fixed, foundations first. A case
blocked by several gaps is listed under the last one in that order that it needs,
so it starts passing with that gap's fix and not before. ``DEFERRED`` holds cases
that are out of scope by decision; they stay xfailed.

A case that remmy passes or fails depending on thread timing is also listed in its
gap's ``timing_dependent``: its xfail is not strict, so it may XPASS by chance
without failing the suite. The ledger cannot catch a stale entry there, so a fix
must remove such a case by hand. Every other case stays strict.

Keys are ``<module file>::<test name>[<case id>]``, the node id without the
directory, so they do not depend on where pytest is started from.

Self-check of the harness: every case is its own oracle, so running the suite
with ``/bin/rm`` as the program under test and xfail marks ignored must pass all
of it::

    uv run pytest tests/test_rm_parity_*.py -n auto --runxfail --remmy /bin/rm

Without ``--runxfail`` that same run fails exactly the ledger's cases (XPASS,
strict), which is expected. ``test_parity_ledger.py`` checks that every key names
a real case and that no case is listed twice.
"""

from __future__ import annotations

from dataclasses import dataclass

import pytest


@dataclass(frozen=True)
class Gap:
    reason: str
    """What BSD rm does that remmy does not, in one paragraph."""
    cases: frozenset[str]
    """Keys (``module.py::test[case]``) of the cases this gap is the last blocker of."""
    timing_dependent: frozenset[str] = frozenset()
    """The keys among ``cases`` whose outcome depends on thread timing, so that remmy can
    match ``/bin/rm`` by chance on one run and not the next. Their xfail is not strict: a
    chance pass is reported as XPASS instead of failing the suite."""


def item_key(item: pytest.Item) -> str:
    """The ledger key for a collected test."""
    return f"{item.path.name}::{item.name}"


def ledger() -> dict[str, tuple[str, Gap]]:
    """Map every listed key to its gap slug and gap."""
    out: dict[str, tuple[str, Gap]] = {}
    for slug, gap in (*GAPS.items(), *DEFERRED.items()):
        for key in gap.cases:
            assert key not in out, f"{key} is listed under both {out[key][0]!r} and {slug!r}"
            out[key] = (slug, gap)
    return out


def mark_known_gaps(items: list[pytest.Item]) -> None:
    """Add an xfail to every parity item the ledger lists, strict unless its gap lists it as timing dependent."""
    known = ledger()
    for item in items:
        if item.get_closest_marker("parity") is None:
            continue
        key = item_key(item)
        entry = known.get(key)
        if entry is None:
            continue
        slug, gap = entry
        kind = "deferred" if slug in DEFERRED else "gap"
        strict = key not in gap.timing_dependent
        item.add_marker(pytest.mark.xfail(strict=strict, reason=f"parity {kind} {slug!r} (see parity_gaps.py)"))


_OPERAND_ORDER = frozenset(
    {
        "test_rm_parity_edges.py::test_cwd_and_dot_shapes[rm_cwd_abs_then_dot]",
        "test_rm_parity_fs.py::test_recursion_and_order[duplicate_dir_operands_r]",
        "test_rm_parity_fs.py::test_recursion_and_order[overlap_parent_then_child]",
        "test_rm_parity_fs.py::test_recursion_and_order[rv_mixed_operands_fail]",
    }
)

GAPS: dict[str, Gap] = {
    "operand-order": Gap(
        reason=(
            "Operands are handled strictly in order: an operand, including a directory's whole walk, is finished "
            "before the next one is looked at, so later operands see earlier removals ('rm -rv d d' reports 'd: "
            "No such file or directory') and output of different operands never interleaves. remmy walks a "
            "directory operand while it goes on with the next operands and walks all directories together. "
            "Parallelism inside one walk is unaffected. Until then the outcome of these cases depends on thread "
            "timing (a walk runs while later operands are looked at, and may finish first), so they are all "
            "timing dependent; the fix must delete them all, and with them this gap."
        ),
        cases=_OPERAND_ORDER,
        timing_dependent=_OPERAND_ORDER,
    ),
    "walk-semantics": Gap(
        reason=(
            "fts(3) behaviour inside a walk: a directory that can be listed but not searched (0444) is reported "
            "once as '<dir>: Permission denied' and neither emptied nor rmdir'd; names a listing returns that "
            "then vanish on open (the HFS+ private metadata directories at a volume root) are skipped silently, "
            "so a mount point ends with only 'Resource busy'; an operand 'l/' with l pointing at '.' or '..' "
            "walks through the link and reports the final rmdir's ENOENT."
        ),
        cases=frozenset(
            {
                "test_rm_parity_edges.py::test_mount_operands[r_mountpoint_operand]",
                "test_rm_parity_edges.py::test_mount_operands[rx_empty_nested_mount]",
                "test_rm_parity_edges.py::test_symlinks_to_directories[r_symlink_to_dot_slash]",
                "test_rm_parity_edges.py::test_symlinks_to_directories[r_symlink_to_dotdot_slash]",
                "test_rm_parity_fs.py::test_mount_points[mountpoint_r_no_x]",
                "test_rm_parity_fs.py::test_mount_points[x_operand_is_mount]",
                "test_rm_parity_fs.py::test_permissions[r_subdir_0444]",
            }
        ),
    ),
    "deep-paths": Gap(
        reason=(
            "Trees deeper than PATH_MAX: remmy rmdirs directories by full path and gets ENAMETOOLONG, fts removes "
            "them (and -v prints the long paths). Same bug as known_bugs.PATH_MAX_EXCEEDED."
        ),
        cases=frozenset(
            {
                "test_rm_parity_fs.py::test_length_limits[deep_tree_beyond_pathmax]",
                "test_rm_parity_fs.py::test_length_limits[deep_tree_beyond_pathmax_v]",
            }
        ),
    ),
    "prompt-interactive": Gap(
        reason=(
            "-i asks 'remove <path>? ' on stderr for each operand, reads one line from stdin (a pipe, /dev/null "
            "or a tty alike) and takes it through rpmatch(3) in the current locale ('j' under de_DE; EOF and "
            "anything unrecognised mean no; a no is not an error); -id asks 'remove <dir>? ' for directories; -ir "
            "asks 'examine files in directory <dir>? ' before entering and 'remove <dir>? ' after it "
            "(COMMAND_MODE=legacy: 'remove <dir>? ' first). Names are printed raw. Only cases whose prompts form "
            "one ancestor chain are listed here."
        ),
        cases=frozenset(
            {
                "test_rm_parity_cli.py::test_dot_and_slash_guards[slash_i]",
                "test_rm_parity_cli.py::test_interactive_i[Ii_file]",
                "test_rm_parity_cli.py::test_interactive_i[f_i_separate]",
                "test_rm_parity_cli.py::test_interactive_i[fi_file]",
                "test_rm_parity_cli.py::test_interactive_i[iI_file]",
                "test_rm_parity_cli.py::test_interactive_i[i_crlf]",
                "test_rm_parity_cli.py::test_interactive_i[i_dangling_symlink]",
                "test_rm_parity_cli.py::test_interactive_i[i_eof_examine_pty]",
                "test_rm_parity_cli.py::test_interactive_i[i_fifo]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_LANG_C_y]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_LANG_C_yes]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_LANG_de_j]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_LANG_fr_o]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_devnull]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_N]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_YES]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_Y]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_empty_line]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_eof]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_n]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_no]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_space_y]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_x]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_y]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_y_noeol]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_yes]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pipe_yx]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pty_eof]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pty_n]",
                "test_rm_parity_cli.py::test_interactive_i[i_file_pty_y]",
                "test_rm_parity_cli.py::test_interactive_i[i_long_answer_line]",
                "test_rm_parity_cli.py::test_interactive_i[i_missing]",
                "test_rm_parity_cli.py::test_interactive_i[i_multi_eof_early]",
                "test_rm_parity_cli.py::test_interactive_i[i_multi_mixed]",
                "test_rm_parity_cli.py::test_interactive_i[i_prompt_odd_name]",
                "test_rm_parity_cli.py::test_interactive_i[i_symlink]",
                "test_rm_parity_cli.py::test_interactive_i[i_utf8_answer]",
                "test_rm_parity_cli.py::test_interactive_i[id_emptydir_y]",
                "test_rm_parity_cli.py::test_interactive_i[ir_emptydir]",
                "test_rm_parity_cli.py::test_interactive_i[ir_emptydir_n_on_remove]",
                "test_rm_parity_cli.py::test_interactive_i[ir_symlink_to_dir]",
                "test_rm_parity_cli.py::test_interactive_i[ir_tree_eof]",
                "test_rm_parity_cli.py::test_interactive_i[ir_tree_examine_n]",
                "test_rm_parity_cli.py::test_interactive_i[irv_examine_n_v]",
                "test_rm_parity_cli.py::test_interactive_i[iv_file_y]",
                "test_rm_parity_cli.py::test_interactive_i[stdin_closed_i]",
                "test_rm_parity_cli.py::test_legacy_command_mode[legacy_fi]",
                "test_rm_parity_cli.py::test_override_prompts[i_ro_file_pty_y]",
                "test_rm_parity_cli.py::test_override_prompts[i_uchg_file_pty_y]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_pipe_i_n]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_pty_i_n]",
                "test_rm_parity_cli.py::test_usage_and_getopt[ii]",
                "test_rm_parity_edges.py::test_P_and_W[P_i_n]",
                "test_rm_parity_edges.py::test_interactive_i[i_pipe_eof_mid_line]",
                "test_rm_parity_edges.py::test_interactive_i[i_pipe_leading_tab]",
                "test_rm_parity_edges.py::test_interactive_i[i_pipe_nul_in_answer]",
                "test_rm_parity_edges.py::test_interactive_i[i_pipe_yes_sentence]",
                "test_rm_parity_edges.py::test_interactive_i[i_pty_leading_spaces]",
                "test_rm_parity_edges.py::test_interactive_i[i_pty_three_answers]",
                "test_rm_parity_edges.py::test_interactive_i[i_raw_names_in_prompt]",
                "test_rm_parity_edges.py::test_interactive_i[id_empty_dir_n]",
                "test_rm_parity_edges.py::test_interactive_i[id_nonempty_dir]",
                "test_rm_parity_edges.py::test_interactive_i[idv_empty_dir]",
                "test_rm_parity_edges.py::test_interactive_i[ir_closed_stdin]",
                "test_rm_parity_edges.py::test_interactive_i[ir_legacy]",
                "test_rm_parity_edges.py::test_interactive_i[ir_unreadable_subdir]",
                "test_rm_parity_edges.py::test_symlinks_to_directories[i_symlink_to_dir_nor]",
                "test_rm_parity_edges.py::test_symlinks_to_directories[ir_symlink_to_dir_slash]",
            }
        ),
    ),
    "prompt-once": Gap(
        reason=(
            "-I asks once before removing anything: 'remove N files? ' when more than three operands exist (not "
            "counting missing ones), or with -r and any directory operand 'recursively remove <dir>? ', "
            "'recursively remove N dirs? ', '... <dir> and N file(s)? ' or '... N dirs and M file(s)? '; no, EOF "
            "or no stdin exits 1 with nothing removed. -f does not suppress it."
        ),
        cases=frozenset(
            {
                "test_rm_parity_cli.py::test_legacy_command_mode[legacy_I_4files]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_4files_devnull]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_4files_devnull_f]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_4files_eof]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_4files_n]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_4files_pty_n]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_4files_y]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_5_four_exist]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_dir_nor_plus_4]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_1dir_1file]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_2dirs_1file]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_3dirs_2files]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_5files_and_dir]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_answer_yes_word]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_dir_1file]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_dir_2dirs]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_dir_n]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_dot]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_emptydir]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_exit_code_yes_but_error]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_pty_y]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_trailing_slash_dir]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_two_dirs_n_y]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_r_two_dirs_y]",
                "test_rm_parity_cli.py::test_prompt_once_I[I_symlinks_4]",
                "test_rm_parity_cli.py::test_prompt_once_I[If_4files]",
                "test_rm_parity_cli.py::test_prompt_once_I[fI_4files]",
                "test_rm_parity_cli.py::test_prompt_once_I[i_and_I_r_4files]",
                "test_rm_parity_cli.py::test_usage_and_getopt[all_flags_valid_no_W]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_12files]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_4files_closed]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_4files_pty_eof]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_4files_ro_pipe]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_r_closed]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_r_dir_and_file_y]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_r_legacy]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_r_pty_eof]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_r_pty_n]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_r_wide_dir]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_same_file_4_times]",
                "test_rm_parity_edges.py::test_prompt_once_I[Iv_4files_y]",
            }
        ),
    ),
    "override-prompt": Gap(
        reason=(
            "With a tty on stdin and neither -f nor -i in effect, an unwritable file, or one with user flags, "
            "gets 'override <strmode> <user>/<group> [<flags> ]for <path>? ' (strmode without the type, so setuid "
            "and sticky show as S and T; flags like 'uappnd,uchg') and is removed only on yes, directories at "
            "rmdir time; rm does not clear uchg/uappnd, so a yes still ends in 'Operation not permitted'. A pipe, "
            "/dev/null or closed stdin never prompts."
        ),
        cases=frozenset(
            {
                "test_rm_parity_cli.py::test_override_prompts[hidden_flag_pty]",
                "test_rm_parity_cli.py::test_override_prompts[nodump_flag_ro_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_dir_r_pty_n]",
                "test_rm_parity_cli.py::test_override_prompts[ro_dir_r_pty_y]",
                "test_rm_parity_cli.py::test_override_prompts[ro_emptydir_d_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_emptydir_r_pty_n]",
                "test_rm_parity_cli.py::test_override_prompts[ro_emptydir_r_pty_y]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_000_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_1444_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_4444_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_555_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_LANG_C_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_in_tree_pty_n]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_pty_eof]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_pty_n]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_pty_y]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_pty_yes_word]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_r_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_setgid_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_file_sticky_x_pty]",
                "test_rm_parity_cli.py::test_override_prompts[ro_files_pty_mixed]",
                "test_rm_parity_cli.py::test_override_prompts[uappnd_pty]",
                "test_rm_parity_cli.py::test_override_prompts[uchg_dir_r_pty]",
                "test_rm_parity_cli.py::test_override_prompts[uchg_file_pty_n]",
                "test_rm_parity_cli.py::test_override_prompts[uchg_file_pty_y]",
                "test_rm_parity_cli.py::test_override_prompts[uchg_ro_file_pty_n]",
                "test_rm_parity_cli.py::test_override_prompts[uchg_uappnd_pty]",
                "test_rm_parity_edges.py::test_interactive_i[ro_file_pty_legacy]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_4files_then_override_pty]",
                "test_rm_parity_edges.py::test_prompt_once_I[I_r_then_override_pty]",
                "test_rm_parity_fs.py::test_file_flags[uchg_file_0444_pty]",
                "test_rm_parity_fs.py::test_file_flags[uchg_file_pty]",
                "test_rm_parity_fs.py::test_file_flags[uchg_file_pty_no]",
                "test_rm_parity_fs.py::test_names[esc_name_prompt_pty]",
                "test_rm_parity_fs.py::test_permissions[file_0444_pty_no]",
                "test_rm_parity_fs.py::test_permissions[file_0444_pty_yes]",
                "test_rm_parity_fs.py::test_permissions[tree_0444_files_r_pty]",
            }
        ),
    ),
    "flags-pwx": Gap(
        reason=(
            "-P still opens a regular file for writing before removing it, so an unwritable one fails with "
            "'Permission denied' even under -f; -W (without -r) undeletes instead of removing: an existing name "
            "fails with 'File exists', a missing one with 'Operation not permitted', also under -f and -i; -x "
            "does not descend into a directory on another device, so the mount point is left for rmdir to fail "
            "with 'Resource busy'."
        ),
        cases=frozenset(
            {
                "test_rm_parity_cli.py::test_P_W_x[P_f_ro]",
                "test_rm_parity_cli.py::test_P_W_x[P_ro_devnull]",
                "test_rm_parity_cli.py::test_P_W_x[P_ro_pty]",
                "test_rm_parity_cli.py::test_P_W_x[W_dir]",
                "test_rm_parity_cli.py::test_P_W_x[W_emptystring]",
                "test_rm_parity_cli.py::test_P_W_x[W_missing]",
                "test_rm_parity_cli.py::test_P_W_x[W_nonexist]",
                "test_rm_parity_cli.py::test_P_W_x[Wf_nonexist]",
                "test_rm_parity_cli.py::test_P_W_x[Wv]",
                "test_rm_parity_cli.py::test_P_W_x[dW_missing]",
                "test_rm_parity_cli.py::test_P_W_x[fW_missing]",
                "test_rm_parity_edges.py::test_P_and_W[P_mode_000]",
                "test_rm_parity_edges.py::test_P_and_W[iW_existing]",
                "test_rm_parity_edges.py::test_P_and_W[iW_missing]",
                "test_rm_parity_fs.py::test_mount_points[x_mountpoint_r]",
            }
        ),
    ),
}


# Cases remmy runs: only the order of their lines differs, and by chance it can match rm's.
_ORDERING_WALKED = frozenset(
    {
        "test_rm_parity_cli.py::test_P_W_x[P_r_tree]",
        "test_rm_parity_cli.py::test_P_W_x[rWv_dir]",
        "test_rm_parity_cli.py::test_directories[Rv_tree]",
        "test_rm_parity_cli.py::test_directories[d_and_r_v]",
        "test_rm_parity_cli.py::test_directories[rfv_missing]",
        "test_rm_parity_cli.py::test_directories[rv_abs]",
        "test_rm_parity_cli.py::test_directories[rv_all]",
        "test_rm_parity_cli.py::test_directories[rv_dot_prefix]",
        "test_rm_parity_cli.py::test_directories[rv_missing]",
        "test_rm_parity_cli.py::test_directories[rv_nested_operands]",
        "test_rm_parity_cli.py::test_directories[rv_symlink_to_dir_slash]",
        "test_rm_parity_cli.py::test_directories[rv_trailing_slash]",
        "test_rm_parity_cli.py::test_directories[rv_trailing_slashes]",
        "test_rm_parity_cli.py::test_directories[rv_tree]",
        "test_rm_parity_cli.py::test_directories[rv_two_trees]",
        "test_rm_parity_cli.py::test_directories[v_separate_flags]",
        "test_rm_parity_edges.py::test_cwd_and_dot_shapes[rm_cwd_ancestor_abs]",
        "test_rm_parity_edges.py::test_cwd_and_dot_shapes[rm_cwd_then_relative]",
        "test_rm_parity_edges.py::test_misc[rv_raw_names_in_tree]",
        "test_rm_parity_fs.py::test_missing_and_directories[rdv_nested]",
        "test_rm_parity_fs.py::test_recursion_and_order[fs_rv_tree]",
    }
)

# Cases that use -i, -I or -x, which remmy refuses for now: they fail every time.
_ORDERING_REFUSED = frozenset(
    {
        "test_rm_parity_cli.py::test_P_W_x[x_flag]",
        "test_rm_parity_cli.py::test_interactive_i[ir_pty_all_y]",
        "test_rm_parity_cli.py::test_interactive_i[ir_trailing_slash]",
        "test_rm_parity_cli.py::test_interactive_i[ir_tree_all_y]",
        "test_rm_parity_cli.py::test_interactive_i[ir_tree_examine_y_rest_n]",
        "test_rm_parity_cli.py::test_interactive_i[ir_tree_keep_one_file]",
        "test_rm_parity_cli.py::test_interactive_i[ir_tree_y_then_eof]",
        "test_rm_parity_cli.py::test_interactive_i[ird_tree]",
        "test_rm_parity_cli.py::test_interactive_i[irv_tree_all_y]",
        "test_rm_parity_cli.py::test_prompt_once_I[I_and_i_r]",
        "test_rm_parity_cli.py::test_prompt_once_I[I_r_5files_and_dir_y]",
        "test_rm_parity_cli.py::test_prompt_once_I[I_r_dir_y]",
        "test_rm_parity_edges.py::test_interactive_i[irv_special_files_in_tree]",
    }
)

DEFERRED: dict[str, Gap] = {
    "ordering": Gap(
        reason=(
            "Out of scope: output whose order follows fts's depth-first post-order over readdir order, that is -v "
            "lines, -i prompts or errors of sibling entries where a subdirectory's output comes before a later "
            "sibling's. remmy removes sibling subdirectories in parallel and keeps doing so. Its order there "
            "depends on thread timing and matches rm's on some runs, so the cases remmy runs are timing "
            "dependent; those with -i, -I or -x, which remmy still refuses, fail every time and stay strict."
        ),
        cases=_ORDERING_WALKED | _ORDERING_REFUSED,
        timing_dependent=_ORDERING_WALKED,
    ),
}
