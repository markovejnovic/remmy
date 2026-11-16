"""Ledger of the parity cases remmy fails today, one entry per gap; conftest marks each as a strict xfail.

So a fix turns its cases into XPASS (a failure) until they are deleted here, and a regression fails outright.
GAPS is ordered foundations first; a case blocked by several gaps sits under the last one it needs. DEFERRED is
out of scope by decision. ``timing_dependent`` cases may pass by chance, so their xfail is not strict and a fix
must remove them by hand. Keys are ``<module file>::<test>[<case id>]``.

Harness self-check: ``pytest tests/test_rm_parity_*.py --runxfail --remmy /bin/rm`` must pass everything.
"""

from dataclasses import dataclass

import pytest


@dataclass(frozen=True)
class Gap:
    reason: str
    cases: frozenset[str]
    timing_dependent: frozenset[str] = frozenset()


def _keys(spec: str) -> frozenset[str]:
    """Lines of ``<cli|fs|edges> <test name without test_> <case id>...``."""
    out = set()
    for line in spec.strip().splitlines():
        mod, test, *ids = line.split()
        out |= {f"test_rm_parity_{mod}.py::test_{test}[{i}]" for i in ids}
    return frozenset(out)


def item_key(item: pytest.Item) -> str:
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
    known = ledger()
    for item in items:
        if item.get_closest_marker("parity") is not None and (entry := known.get(key := item_key(item))):
            slug, gap = entry
            reason = f"parity {'deferred' if slug in DEFERRED else 'gap'} {slug!r} (see parity_gaps.py)"
            item.add_marker(pytest.mark.xfail(strict=key not in gap.timing_dependent, reason=reason))


_OPERAND_ORDER = _keys("""
edges cwd_and_dot_shapes rm_cwd_abs_then_dot
fs recursion_and_order duplicate_dir_operands_r overlap_parent_then_child rv_mixed_operands_fail
""")

GAPS = {
    "operand-order": Gap(
        "Operands are handled strictly in order: an operand, including a directory's whole walk, is finished before "
        "the next one is looked at, so later operands see earlier removals ('rm -rv d d' reports 'd: No such file or "
        "directory') and output of different operands never interleaves. remmy walks a directory operand while it "
        "goes on with the next operands. Until then these outcomes depend on thread timing.",
        _OPERAND_ORDER,
        _OPERAND_ORDER,
    ),
    "walk-semantics": Gap(
        "fts(3) behaviour inside a walk: a directory that can be listed but not searched (0444) is reported once as "
        "'<dir>: Permission denied' and neither emptied nor rmdir'd; names a listing returns that then vanish on open "
        "(the HFS+ private metadata directories at a volume root) are skipped silently, so a mount point ends with "
        "only 'Resource busy'; an operand 'l/' with l pointing at '.' or '..' walks through the link and reports the "
        "final rmdir's ENOENT.",
        _keys("""
edges mount_operands r_mountpoint_operand rx_empty_nested_mount
edges symlinks_to_directories r_symlink_to_dot_slash r_symlink_to_dotdot_slash
fs mount_points mountpoint_r_no_x x_operand_is_mount
fs permissions r_subdir_0444
"""),
    ),
    "override-prompt": Gap(
        "With a tty on stdin and neither -f nor -i in effect, an unwritable file, or one with user flags, gets "
        "'override <strmode> <user>/<group> [<flags> ]for <path>? ' (strmode without the type, so setuid and sticky "
        "show as S and T; flags like 'uappnd,uchg') and is removed only on yes, directories at rmdir time; rm does "
        "not clear uchg/uappnd, so a yes still ends in 'Operation not permitted'. A pipe, /dev/null or closed stdin "
        "never prompts.",
        _keys("""
cli override_prompts hidden_flag_pty nodump_flag_ro_pty ro_dir_r_pty_n ro_dir_r_pty_y ro_emptydir_d_pty
cli override_prompts ro_emptydir_r_pty_n ro_emptydir_r_pty_y ro_file_000_pty ro_file_1444_pty
cli override_prompts ro_file_4444_pty ro_file_555_pty ro_file_LANG_C_pty ro_file_in_tree_pty_n
cli override_prompts ro_file_pty_eof ro_file_pty_n ro_file_pty_y ro_file_pty_yes_word ro_file_r_pty
cli override_prompts ro_file_setgid_pty ro_file_sticky_x_pty ro_files_pty_mixed uappnd_pty uchg_dir_r_pty
cli override_prompts uchg_file_pty_n uchg_file_pty_y uchg_ro_file_pty_n uchg_uappnd_pty
edges interactive_i ro_file_pty_legacy
edges prompt_once_I I_4files_then_override_pty I_r_then_override_pty
fs file_flags uchg_file_0444_pty uchg_file_pty uchg_file_pty_no
fs names esc_name_prompt_pty
fs permissions file_0444_pty_no file_0444_pty_yes tree_0444_files_r_pty
"""),
    ),
    "flags-pwx": Gap(
        "-P still opens a regular file for writing before removing it, so an unwritable one fails with 'Permission "
        "denied' even under -f; -W (without -r) undeletes instead of removing: an existing name fails with 'File "
        "exists', a missing one with 'Operation not permitted', also under -f and -i; -x does not descend into a "
        "directory on another device, so the mount point is left for rmdir to fail with 'Resource busy'.",
        _keys("""
cli P_W_x P_f_ro P_ro_devnull P_ro_pty W_dir W_emptystring W_missing W_nonexist Wf_nonexist Wv dW_missing fW_missing
cli usage_and_getopt all_flags all_flags_valid_no_W
edges P_and_W P_mode_000 iW_existing iW_missing
fs mount_points x_mountpoint_r
"""),
    ),
}

# Cases remmy runs whose line order alone differs; it can match rm's by chance.
_ORDERING_WALKED = _keys("""
cli P_W_x P_r_tree rWv_dir
cli directories Rv_tree d_and_r_v rfv_missing rv_abs rv_all rv_dot_prefix rv_missing rv_nested_operands
cli directories rv_symlink_to_dir_slash rv_trailing_slash rv_trailing_slashes rv_tree rv_two_trees
cli directories v_separate_flags
cli prompt_once_I I_r_5files_and_dir_y I_r_dir_y
edges cwd_and_dot_shapes rm_cwd_ancestor_abs rm_cwd_then_relative
edges misc rv_raw_names_in_tree
fs missing_and_directories rdv_nested
fs recursion_and_order fs_rv_tree
""")
# Cases that fail every time: -i asks about a directory's entries before what is inside its earlier
# subdirectories, where fts asks depth first; and -x, which remmy refuses.
_ORDERING_REFUSED = _keys("""
cli P_W_x x_flag
cli interactive_i ir_pty_all_y ir_trailing_slash ir_tree_all_y ir_tree_keep_one_file ird_tree irv_tree_all_y
cli prompt_once_I I_and_i_r
""")

DEFERRED = {
    "ordering": Gap(
        "Out of scope: output whose order follows fts's depth-first post-order over readdir order (-v lines, -i "
        "prompts or errors of sibling entries where a subdirectory's output precedes a later sibling's). remmy "
        "removes sibling subdirectories in parallel, so its order there depends on thread timing.",
        _ORDERING_WALKED | _ORDERING_REFUSED,
        _ORDERING_WALKED,
    ),
}
