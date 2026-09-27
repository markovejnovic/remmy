"""The parity ledger (parity_gaps) names real cases, each exactly once."""

import inspect

import parity_gaps
import pytest
import test_rm_parity_cli
import test_rm_parity_edges
import test_rm_parity_fs

SLUGS = [*parity_gaps.GAPS, *parity_gaps.DEFERRED]


def test_every_key_names_a_parity_case() -> None:
    cases = [
        f"{module.__name__}.py::{name}[{case.id}]"
        for module in (test_rm_parity_cli, test_rm_parity_fs, test_rm_parity_edges)
        for name, fn in inspect.getmembers(module, inspect.isfunction)
        if name.startswith("test_")
        for mark in getattr(fn, "pytestmark", [])
        if mark.name == "parametrize"
        for case in mark.args[1]
    ]
    assert len(cases) == len(set(cases)), "two parity cases share a key"
    assert sorted(parity_gaps.ledger().keys() - set(cases)) == []


def test_no_case_is_listed_twice() -> None:
    parity_gaps.ledger()  # asserts on a duplicate


@pytest.mark.parametrize("slug", SLUGS)
def test_every_gap_has_cases_and_a_kebab_case_slug(slug: str) -> None:
    gap = parity_gaps.GAPS.get(slug) or parity_gaps.DEFERRED[slug]
    assert gap.cases
    assert gap.reason
    assert slug == slug.lower() and slug.replace("-", "").isalnum()


@pytest.mark.parametrize("slug", SLUGS)
def test_timing_dependent_cases_belong_to_their_gap(slug: str) -> None:
    gap = parity_gaps.GAPS.get(slug) or parity_gaps.DEFERRED[slug]
    assert gap.timing_dependent <= gap.cases
