"""Property-based tests: generated names, trees, permissions, and operands.

Hypothesis shrinks any failure to a minimal tree and replays it from its
example database on the next run. ``HYPOTHESIS_PROFILE=thorough`` searches
harder.
"""

from __future__ import annotations

import os
from pathlib import Path

import fstree
import pytest
import strategies
from fstree import Symlink
from harness import run_remmy
from hypothesis import event, example, given, target
from hypothesis import strategies as st
from oracle import RM, compare_with_rm


@pytest.fixture(scope="module")
def base(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return tmp_path_factory.mktemp("prop")


@given(
    name=strategies.names,
    is_dir=st.booleans(),
    separator=st.sampled_from(["./", "--"]),
)
@example(name="é", is_dir=False, separator="./")
@example(name="x" * 255, is_dir=True, separator="./")
@example(name="-r", is_dir=True, separator="--")
@example(name="--help", is_dir=False, separator="--")
@example(name="nl\n", is_dir=True, separator="./")
def test_any_name_is_removable(remmy_bin: Path, base: Path, name: str, is_dir: bool, separator: str) -> None:
    with fstree.scratch(base) as d:
        work = fstree.build(d / "work", {name: {name: "x"} if is_dir else "x"})
        fstree.build(d, {"keep": ""})
        operand = ["./" + name] if separator == "./" else ["--", name]

        res = run_remmy(remmy_bin, ["-r", *operand], cwd=work)

        assert (res.returncode, res.stdout, res.stderr) == (0, "", ""), res
        assert fstree.listing(d) == {"work", "keep"}


@given(tree=strategies.trees, threads=st.integers(1, 16))
def test_any_tree_is_removed_without_escaping(remmy_bin: Path, base: Path, tree: fstree.Spec, threads: int) -> None:
    strategies.describe_tree(tree)
    event(f"threads: {strategies.bucket(threads)}")
    with fstree.scratch(base) as d:
        outside = fstree.build(d / "outside", {"file": "keep", "dir": {"x": "keep"}})
        fstree.build(d / "t", {"tree": tree, "escape": Symlink(str(outside))})
        fstree.build(d, {"keep": {"x": "k"}})
        before = {p: fstree.snapshot_dir(d / p) for p in ("outside", "keep")}

        res = run_remmy(remmy_bin, ["-r", "t"], cwd=d, threads=threads)

        assert (res.returncode, res.stdout, res.stderr) == (0, "", ""), res
        assert not fstree.exists(d / "t")
        assert {p: fstree.snapshot_dir(d / p) for p in before} == before


@given(tree=strategies.trees, threads=st.integers(1, 4), fd_limit=st.integers(12, 64))
def test_any_tree_is_removed_under_fd_limit(
    remmy_bin: Path, base: Path, tree: fstree.Spec, threads: int, fd_limit: int
) -> None:
    strategies.describe_tree(tree)
    depth, dirs, _ = strategies.tree_stats(tree)
    # Descriptor pressure: nesting and fan-out relative to what remmy may open.
    target(depth / fd_limit, label="depth per descriptor")
    target(dirs / fd_limit, label="directories per descriptor")
    event(f"deeper than fd limit: {depth >= fd_limit}")
    with fstree.scratch(base) as d:
        fstree.build(d / "t", tree)

        res = run_remmy(remmy_bin, ["-r", "t"], cwd=d, threads=threads, fd_limit=fd_limit)

        assert res.returncode == 0, res
        assert not fstree.exists(d / "t")


# --- Against /bin/rm ---------------------------------------------------------

MODES = st.sampled_from([0o000, 0o111, 0o333, 0o444, 0o555])

needs_rm = pytest.mark.skipif(not os.access(RM, os.X_OK), reason=f"{RM} not available")


@pytest.mark.differential
@needs_rm
@given(tree=strategies.trees, data=st.data())
def test_partial_failures_match_rm(
    remmy_bin: Path, base: Path, is_root: bool, tree: fstree.Spec, data: st.DataObject
) -> None:
    if is_root:
        pytest.skip("root bypasses permissions")
    dirs = ["t", *(f"t/{p}" for p in strategies.dir_paths(tree))]
    locks = data.draw(st.dictionaries(st.sampled_from(dirs), MODES, max_size=4), label="locks")
    strategies.describe_tree(tree)
    event(f"locked directories: {strategies.bucket(len(locks))}")
    for mode in set(locks.values()):
        event(f"lock mode used: {mode:03o}")
    target(float(len(locks)), label="locked directories")

    def recipe(root: Path) -> None:
        fstree.build(root / "t", tree)
        # Deepest first: locking a parent first would block reaching its children.
        for path in sorted(locks, key=lambda p: p.count("/"), reverse=True):
            os.chmod(root / path, locks[path])

    with fstree.scratch(base) as d:
        compare_with_rm(remmy_bin, d, recipe, ["-r", "t"])


@pytest.mark.differential
@needs_rm
@given(
    top=strategies.trees.filter(bool),
    data=st.data(),
    recursive=st.booleans(),
)
def test_operand_lists_match_rm(
    remmy_bin: Path, base: Path, top: fstree.Spec, data: st.DataObject, recursive: bool
) -> None:
    """Existing and missing top-level operands, in any order, with and without -r.

    Each name appears at most once: naming a directory twice races in remmy,
    which is covered separately in test_recursive.
    """
    operands = data.draw(
        st.lists(
            st.one_of(st.sampled_from(sorted(top)), strategies.names),
            min_size=1,
            max_size=8,
            unique_by=strategies.fs_key,
        ),
        label="operands",
    )

    existing = {strategies.fs_key(n) for n in top}
    missing = sum(strategies.fs_key(o) not in existing for o in operands)
    event(f"operands: {strategies.bucket(len(operands))}, missing: {strategies.bucket(missing)}")
    event(f"recursive: {recursive}")

    def recipe(root: Path) -> None:
        fstree.build(root, top)

    with fstree.scratch(base) as d:
        compare_with_rm(remmy_bin, d, recipe, [*(["-r"] if recursive else []), "--", *operands])
