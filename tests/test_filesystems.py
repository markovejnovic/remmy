"""The same removal contract on other filesystem types.

Each volume is a sparse disk image mounted once per session. Everything else
in the suite runs on the host's filesystem (usually case-insensitive APFS).
"""

from __future__ import annotations

import os
from collections.abc import Iterator
from pathlib import Path

import fstree
import pytest
import strategies
import volumes
from harness import run_remmy
from hypothesis import given
from hypothesis import strategies as st
from oracle import compare_with_rm
from pytest_check import check

pytestmark = pytest.mark.filesystems


@pytest.fixture(scope="session", params=volumes.FILESYSTEMS)
def volume(request: pytest.FixtureRequest, tmp_path_factory: pytest.TempPathFactory) -> Iterator[volumes.Volume]:
    try:
        with volumes.mounted(request.param, tmp_path_factory.mktemp("vol")) as v:
            yield v
    except volumes.VolumeUnavailable as e:
        pytest.skip(str(e))


@given(data=st.data(), threads=st.integers(1, 8))
def test_any_tree_is_removed(remmy_bin: Path, volume: volumes.Volume, data: st.DataObject, threads: int) -> None:
    tree = data.draw(strategies.trees_for(volume.caps), label="tree")
    with fstree.scratch(volume.root) as d:
        fstree.build(d / "t", tree)
        fstree.build(d, {"keep": {"x": "k"}})

        res = run_remmy(remmy_bin, ["-r", "t"], cwd=d, threads=threads)

        assert (res.returncode, res.stdout, res.stderr) == (0, "", ""), res
        assert fstree.listing(d) == {"keep", "keep/x"}


@given(data=st.data())
def test_any_operand_list_matches_rm(remmy_bin: Path, volume: volumes.Volume, data: st.DataObject) -> None:
    top = data.draw(strategies.trees_for(volume.caps).filter(bool), label="top")
    operands = data.draw(
        st.lists(
            st.one_of(st.sampled_from(sorted(top)), strategies.names_for(volume.caps)),
            min_size=1,
            max_size=6,
            unique_by=strategies.fs_key,
        ),
        label="operands",
    )
    recursive = data.draw(st.booleans(), label="recursive")

    with fstree.scratch(volume.root) as d:
        compare_with_rm(remmy_bin, d, lambda r: fstree.build(r, top), [*(["-r"] if recursive else []), "--", *operands])


def test_wide_and_deep(remmy_bin: Path, volume: volumes.Volume) -> None:
    with fstree.scratch(volume.root) as d:
        fstree.build(d / "wide", {f"f{i}": "" for i in range(5000)} | {f"d{i}": {"f": ""} for i in range(500)})
        fstree.deep_chain(d / "deep", depth=200, name="d", leaf_files=2)

        res = run_remmy(remmy_bin, ["-r", "wide", "deep"], cwd=d, threads=4)

        with check:
            assert (res.returncode, res.stderr) == (0, ""), res
        with check:
            assert fstree.listing(d) == set()


def test_names_differing_only_in_case(remmy_bin: Path, volume: volumes.Volume) -> None:
    if not volume.case_sensitive:
        pytest.skip(f"{volume.fs} is case-insensitive")
    with fstree.scratch(volume.root) as d:
        fstree.build(d, {"t": {"a": "", "A": {"x": ""}, "Dir": {"f": ""}, "dir": {"F": ""}}, "K": "", "k": ""})

        res = run_remmy(remmy_bin, ["-r", "t", "k"], cwd=d)

        with check:
            assert res.returncode == 0, res
        with check:
            assert fstree.listing(d) == {"K"}


def test_case_insensitive_operand_spelling(remmy_bin: Path, volume: volumes.Volume) -> None:
    if volume.case_sensitive:
        pytest.skip(f"{volume.fs} is case-sensitive")
    with fstree.scratch(volume.root) as d:
        fstree.build(d, {"Tree": {"Sub": {"f": ""}}})

        res = run_remmy(remmy_bin, ["-r", "tREE"], cwd=d)

        with check:
            assert res.returncode == 0, res
        with check:
            assert fstree.listing(d) == set()


@pytest.mark.differential
@given(data=st.data())
def test_partial_failures_match_rm(remmy_bin: Path, volume: volumes.Volume, is_root: bool, data: st.DataObject) -> None:
    if not volume.permissions or is_root:
        pytest.skip("permissions are not enforced here")
    tree = data.draw(strategies.trees_for(volume.caps), label="tree")
    dirs = ["t", *(f"t/{p}" for p in strategies.dir_paths(tree))]
    locks = data.draw(
        st.dictionaries(st.sampled_from(dirs), st.sampled_from([0o000, 0o333, 0o555]), max_size=3),
        label="locks",
    )

    def recipe(root: Path) -> None:
        fstree.build(root / "t", tree)
        for path in sorted(locks, key=lambda p: p.count("/"), reverse=True):
            os.chmod(root / path, locks[path])

    with fstree.scratch(volume.root) as d:
        compare_with_rm(remmy_bin, d, recipe, ["-r", "t"])
