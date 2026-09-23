"""The estimators and the decision rule, on synthetic data with known answers.

These run in the normal suite: a statistics bug would silently mislabel every
benchmark, so it is caught before any benchmark is trusted.
"""

from __future__ import annotations

import math
import random
from pathlib import Path

import numpy as np
import pytest
import registry
import runner
import schema
import stats
from hypothesis import given
from hypothesis import strategies as st
from schema import Cache, CellKey, Interval, Verdict

RESAMPLES = 4000


@pytest.fixture
def rng() -> np.random.Generator:
    return np.random.default_rng(20260914)


def test_bca_interval_covers_a_known_ratio(rng: np.random.Generator) -> None:
    reference = rng.lognormal(math.log(1.00), 0.20, 60)
    tool = rng.lognormal(math.log(0.43), 0.20, 60)

    ratio, ci = stats.bca_ratio_ci(tool, reference, resamples=RESAMPLES, seed=1)

    assert abs(ratio - 0.43) < 0.12
    assert ci.contains(0.43)
    assert ci.hi < 1.0


def test_bca_interval_covers_one_for_identical_distributions(rng: np.random.Generator) -> None:
    a, b = rng.lognormal(0, 0.2, 60), rng.lognormal(0, 0.2, 60)

    _, ci = stats.bca_ratio_ci(a, b, resamples=RESAMPLES, seed=2)

    assert ci.contains(1.0)


def test_bca_coverage_is_near_nominal(rng: np.random.Generator) -> None:
    """Over many experiments the 95% interval should contain the truth ~95% of the time."""
    hits = 0
    trials = 150
    for i in range(trials):
        tool = rng.lognormal(math.log(0.5), 0.25, 30)
        reference = rng.lognormal(0.0, 0.25, 30)
        _, ci = stats.bca_ratio_ci(tool, reference, resamples=1500, seed=i)
        hits += ci.contains(0.5)
    assert 0.88 <= hits / trials <= 0.99


def test_cliffs_delta_extremes() -> None:
    fast, slow = np.array([1.0, 2.0, 3.0]), np.array([4.0, 5.0, 6.0])
    assert stats.cliffs_delta(fast, slow) == -1.0
    assert stats.cliffs_delta(slow, fast) == 1.0
    assert stats.cliffs_delta(fast, fast) == 0.0


def test_mann_whitney_separates_distinct_groups(rng: np.random.Generator) -> None:
    assert stats.mann_whitney_p(rng.normal(0.5, 0.05, 30), rng.normal(1.0, 0.05, 30)) < 1e-6


def test_holm_adjusts() -> None:
    assert stats.holm([0.001, 0.04, 0.9]) == pytest.approx([0.003, 0.08, 0.9])


@given(st.lists(st.floats(0, 1), min_size=1, max_size=20))
def test_holm_is_monotone_and_bounded(pvalues: list[float]) -> None:
    adjusted = stats.holm(pvalues)
    assert all(p <= a <= 1 for a, p in zip(adjusted, pvalues, strict=True))
    order = sorted(range(len(pvalues)), key=lambda i: pvalues[i])
    assert [adjusted[i] for i in order] == sorted(adjusted)


def test_median_ci_covers_the_median(rng: np.random.Generator) -> None:
    x = rng.normal(2.0, 0.1, 200)
    assert stats.median_ci(x, resamples=RESAMPLES, seed=3).contains(2.0)


# --- Decision rule ---------------------------------------------------------------


RULE = schema.DecisionRule()


@pytest.mark.parametrize(
    ("ci", "delta", "p", "expected"),
    [
        (Interval(0.40, 0.46), -0.9, 0.001, Verdict.DISTINCT),
        (Interval(1.20, 1.40), 0.8, 0.001, Verdict.DISTINCT),
        (Interval(0.90, 1.10), -0.1, 0.300, Verdict.TIE),
        # As registered, DISTINCT is decided first and does not consult p.
        (Interval(0.80, 0.94), -0.9, 0.200, Verdict.DISTINCT),
        (Interval(0.90, 0.99), -0.2, 0.200, Verdict.TIE),  # holm p too high
        (Interval(0.80, 0.94), -0.1, 0.001, Verdict.INDETERMINATE),  # effect too small
        (Interval(0.90, 0.99), -0.9, 0.001, Verdict.INDETERMINATE),  # straddles the band edge
        (Interval(math.nan, math.nan), -0.9, 0.001, Verdict.TIE),
    ],
)
def test_decision_rule(ci: Interval, delta: float, p: float, expected: Verdict) -> None:
    assert RULE.decide(ci, delta, p) is expected


# --- End to end on a synthetic run -------------------------------------------------


MEDIANS = {"rm@1": 1.0, "remmy@1": 1.0, "remmy@4": 0.4, "xargs@1": 1.3, "xargs@4": 0.7}


def _synthetic_run(plan: schema.Plan, medians: dict[str, float], seed: int = 7) -> schema.Run:
    rng = np.random.default_rng(seed)
    cells = plan.cells(plan.fixtures[0], Cache.WARM)
    samples = tuple(
        schema.Sample(cell=cell, seconds=float(rng.lognormal(math.log(medians[cell.label]), 0.05)))
        for cell in cells
        for _ in range(plan.sampling[Cache.WARM].reps)
    )
    return schema.Run(plan=plan, samples=samples)


@pytest.fixture
def demo_plan() -> schema.Plan:
    return registry.plan("demo", Path("/nonexistent/remmy"))


def _verdicts(summary: schema.Summary) -> dict[str, Verdict]:
    return {p.tool.label: p.verdict for p in summary.pairs}


def test_synthetic_run_recovers_planted_effects(demo_plan: schema.Plan) -> None:
    summary = stats.summarize(_synthetic_run(demo_plan, MEDIANS))

    verdicts = _verdicts(summary)
    assert (
        verdicts["remmy@4"] is Verdict.DISTINCT and summary.pair(CellKey("F0", "remmy", 4, Cache.WARM)).time_ratio < 1
    )
    assert verdicts["remmy@1"] is Verdict.TIE
    assert verdicts["xargs@1"] is Verdict.DISTINCT


# --- Plan invariants ---------------------------------------------------------------


@pytest.mark.parametrize("name", registry.PLANS)
def test_registered_plans_are_valid(name: str) -> None:
    registry.plan(name, Path("/opt/remmy"))


def test_every_round_runs_every_cell_once(demo_plan: schema.Plan) -> None:
    cells = demo_plan.cells(demo_plan.fixtures[0], Cache.WARM)
    orders = runner.round_orders(cells, 4, random.Random(1))

    assert all(sorted(o) == sorted(cells) for o in orders)
    assert len({tuple(o) for o in orders}) > 1


def test_single_config_tools_never_sweep(demo_plan: schema.Plan) -> None:
    cells = demo_plan.cells(demo_plan.fixtures[0], Cache.WARM)
    assert [c.label for c in cells if c.tool == "rm"] == ["rm@1"]
    assert {c.threads for c in cells if c.tool == "remmy"} == set(demo_plan.threads)


@pytest.mark.parametrize(
    "bad",
    [
        {"argv": ("/bin/rm", "-rf")},  # no {tree}
        {"argv": ("/bin/rm", "{tree}", "{oops}")},  # unknown placeholder
        {"argv": ("a {tree}", "b"), "shell": True},  # a shell tool is one string
    ],
)
def test_tool_validation(bad: dict[str, object]) -> None:
    with pytest.raises(ValueError):
        schema.Tool(name="x", **bad)  # type: ignore[arg-type]


def test_interval_rejects_reversed_bounds() -> None:
    with pytest.raises(ValueError):
        Interval(2.0, 1.0)


def test_cell_key_labels() -> None:
    assert str(CellKey("F0", "remmy", 4, Cache.WARM)) == "F0/warm/remmy@4"
