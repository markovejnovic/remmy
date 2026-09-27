"""The parity ledger (``parity_gaps``) names real cases, each exactly once."""

from __future__ import annotations

import inspect
from types import ModuleType

import parity_gaps
import pytest
import test_rm_parity_cli
import test_rm_parity_edges
import test_rm_parity_fs

MODULES = (test_rm_parity_cli, test_rm_parity_fs, test_rm_parity_edges)


def _case_keys(module: ModuleType) -> list[str]:
    keys = []
    for name, fn in inspect.getmembers(module, inspect.isfunction):
        if not name.startswith("test_"):
            continue
        for mark in getattr(fn, "pytestmark", []):
            if mark.name == "parametrize":
                keys += [f"{module.__name__}.py::{name}[{case.id}]" for case in mark.args[1]]
    return keys


def test_every_key_names_a_parity_case() -> None:
    cases = [key for module in MODULES for key in _case_keys(module)]
    assert len(cases) == len(set(cases)), "two parity cases share a key"
    unknown = sorted(parity_gaps.ledger().keys() - set(cases))
    assert unknown == []


def test_no_case_is_listed_twice() -> None:
    parity_gaps.ledger()  # asserts on a duplicate


@pytest.mark.parametrize("slug", [*parity_gaps.GAPS, *parity_gaps.DEFERRED])
def test_every_gap_has_cases_and_a_kebab_case_slug(slug: str) -> None:
    gap = parity_gaps.GAPS.get(slug) or parity_gaps.DEFERRED[slug]
    assert gap.cases
    assert gap.reason
    assert slug == slug.lower() and slug.replace("-", "").isalnum()


@pytest.mark.parametrize("slug", [*parity_gaps.GAPS, *parity_gaps.DEFERRED])
def test_timing_dependent_cases_belong_to_their_gap(slug: str) -> None:
    gap = parity_gaps.GAPS.get(slug) or parity_gaps.DEFERRED[slug]
    assert gap.timing_dependent <= gap.cases
