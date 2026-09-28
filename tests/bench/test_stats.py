"""Estimators and the decision rule on synthetic data with known answers. These run in the normal suite: a
statistics bug would silently mislabel every benchmark."""

import json
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
def rng():
    return np.random.default_rng(20260914)


@pytest.fixture
def demo_plan():
    return registry.plan("demo", Path("/nonexistent/remmy"))


def test_bca_interval_covers_a_known_ratio(rng):
    reference, tool = rng.lognormal(0, 0.20, 60), rng.lognormal(math.log(0.43), 0.20, 60)
    ratio, ci = stats.bca_ratio_ci(tool, reference, resamples=RESAMPLES, seed=1)
    assert abs(ratio - 0.43) < 0.12
    assert ci.contains(0.43)
    assert ci.hi < 1.0


def test_bca_interval_covers_one_for_identical_distributions(rng):
    a, b = rng.lognormal(0, 0.2, 60), rng.lognormal(0, 0.2, 60)
    assert stats.bca_ratio_ci(a, b, resamples=RESAMPLES, seed=2)[1].contains(1.0)


def test_bca_coverage_is_near_nominal(rng):
    """Over many experiments the 95% interval should contain the truth ~95% of the time."""
    hits = 0
    for i in range(150):
        tool, reference = rng.lognormal(math.log(0.5), 0.25, 30), rng.lognormal(0.0, 0.25, 30)
        hits += stats.bca_ratio_ci(tool, reference, resamples=1500, seed=i)[1].contains(0.5)
    assert 0.88 <= hits / 150 <= 0.99


def test_cliffs_delta_extremes():
    fast, slow = np.array([1.0, 2.0, 3.0]), np.array([4.0, 5.0, 6.0])
    assert stats.cliffs_delta(fast, slow) == -1.0
    assert stats.cliffs_delta(slow, fast) == 1.0
    assert stats.cliffs_delta(fast, fast) == 0.0


def test_mann_whitney_separates_distinct_groups(rng):
    assert stats.mann_whitney_p(rng.normal(0.5, 0.05, 30), rng.normal(1.0, 0.05, 30)) < 1e-6


def test_holm_adjusts():
    assert stats.holm([0.001, 0.04, 0.9]) == pytest.approx([0.003, 0.08, 0.9])


@given(st.lists(st.floats(0, 1), min_size=1, max_size=20))
def test_holm_is_monotone_and_bounded(pvalues):
    adjusted = stats.holm(pvalues)
    assert all(p <= a <= 1 for a, p in zip(adjusted, pvalues, strict=True))
    order = sorted(range(len(pvalues)), key=lambda i: pvalues[i])
    assert [adjusted[i] for i in order] == sorted(adjusted)


def test_median_ci_covers_the_median(rng):
    assert stats.median_ci(rng.normal(2.0, 0.1, 200), resamples=RESAMPLES, seed=3).contains(2.0)


@pytest.mark.parametrize(
    ("ci", "delta", "p", "expected"),
    [
        (Interval(0.40, 0.46), -0.9, 0.001, Verdict.DISTINCT),
        (Interval(1.20, 1.40), 0.8, 0.001, Verdict.DISTINCT),
        (Interval(0.90, 1.10), -0.1, 0.300, Verdict.TIE),
        (Interval(0.80, 0.94), -0.9, 0.200, Verdict.DISTINCT),  # DISTINCT is decided first and ignores p
        (Interval(0.90, 0.99), -0.2, 0.200, Verdict.TIE),  # holm p too high
        (Interval(0.80, 0.94), -0.1, 0.001, Verdict.INDETERMINATE),  # effect too small
        (Interval(0.90, 0.99), -0.9, 0.001, Verdict.INDETERMINATE),  # straddles the band edge
        (Interval(math.nan, math.nan), -0.9, 0.001, Verdict.TIE),
    ],
)
def test_decision_rule(ci, delta, p, expected):
    assert schema.DecisionRule().decide(ci, delta, p) is expected


def test_synthetic_run_recovers_planted_effects(demo_plan):
    medians = {"rm@1": 1.0, "remmy@1": 1.0, "remmy@4": 0.4, "xargs@1": 1.3, "xargs@4": 0.7}
    rng = np.random.default_rng(7)
    cells = demo_plan.cells(demo_plan.fixtures[0], Cache.WARM)
    reps = demo_plan.sampling[Cache.WARM].reps
    draws = [(c, float(rng.lognormal(math.log(medians[c.label]), 0.05))) for c in cells for _ in range(reps)]
    samples = [schema.Sample(cell=c, seconds=s) for c, s in draws]
    summary = stats.summarize(schema.Run(plan=demo_plan, samples=tuple(samples)))
    verdicts = {p.tool.label: p.verdict for p in summary.pairs}
    assert verdicts["remmy@4"] is Verdict.DISTINCT
    assert summary.pair(CellKey("F0", "remmy", 4, Cache.WARM)).time_ratio < 1
    assert verdicts["remmy@1"] is Verdict.TIE
    assert verdicts["xargs@1"] is Verdict.DISTINCT


@pytest.mark.parametrize("platform", ["darwin", "linux"])
@pytest.mark.parametrize("name", registry.PLANS)
def test_registered_plans_are_valid(name, platform):
    plan = registry.plan(name, Path("/opt/remmy"), platform)
    # Every platform times the same fixtures against a tool named rm, so ratios mean the same thing.
    assert plan.fixtures == registry.plan(name, Path("/opt/remmy"), "darwin").fixtures
    assert plan.reference == "rm"


def test_every_round_runs_every_cell_once(demo_plan):
    cells = demo_plan.cells(demo_plan.fixtures[0], Cache.WARM)
    orders = runner.round_orders(cells, 4, random.Random(1))
    assert all(sorted(o) == sorted(cells) for o in orders)
    assert len({tuple(o) for o in orders}) > 1


def test_single_config_tools_never_sweep(demo_plan):
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
def test_tool_validation(bad):
    with pytest.raises(ValueError):
        schema.Tool(name="x", **bad)


def test_interval_rejects_reversed_bounds():
    with pytest.raises(ValueError):
        Interval(2.0, 1.0)


def test_cell_key_labels():
    assert str(CellKey("F0", "remmy", 4, Cache.WARM)) == "F0/warm/remmy@4"


def test_bmf_carries_the_summary_bencher_tracks(demo_plan, tmp_path):
    import bmf
    from session import BenchSession

    rng = np.random.default_rng(3)
    cells = demo_plan.cells(demo_plan.fixtures[0], Cache.WARM)
    samples = [schema.Sample(cell=c, seconds=float(rng.lognormal(0, 0.05))) for c in cells for _ in range(6)]
    session = BenchSession(plan=demo_plan, env=None, out=tmp_path, rng=random.Random(1), samples=samples)
    summary = session.summary()
    doc = bmf.convert(json.loads(session.write_json(summary).read_text()))

    assert set(doc) == {f"demo/F0/warm/{c.label}" for c in cells}
    remmy = summary.pair(CellKey("F0", "remmy", 4, Cache.WARM))
    ratio = doc["demo/F0/warm/remmy@4"]["time-ratio"]
    assert ratio == {"value": remmy.time_ratio, "lower_value": remmy.time_ratio_ci95.lo,
                     "upper_value": remmy.time_ratio_ci95.hi}  # fmt: skip
    # Only the subject is judged on a ratio; the reference and competitors are controls.
    assert all("time-ratio" not in m for name, m in doc.items() if "/remmy@" not in name)
    latency = doc["demo/F0/warm/rm@1"]["latency"]
    assert latency["lower_value"] <= latency["value"] <= latency["upper_value"]
    assert 0.5e9 < latency["value"] < 2e9  # ns
    rate = doc["demo/F0/warm/rm@1"]["throughput"]
    assert rate["lower_value"] <= rate["value"] <= rate["upper_value"]


def test_baseline_comparison_recovers_planted_regression(tmp_path):
    import bmf
    from session import BenchSession

    plan = registry.plan("demo", Path("/x/remmy"), baselines={"base": Path("/x/base")})
    assert "base" in {t.name for t in plan.tools}
    # remmy@4 regressed 10% against base@4; remmy@1 is unchanged.
    medians = {
        "rm@1": 1.0,
        "xargs@1": 1.3,
        "xargs@4": 0.7,
        "remmy@1": 1.0,
        "base@1": 1.0,
        "remmy@4": 0.44,
        "base@4": 0.4,
    }
    rng = np.random.default_rng(11)
    cells = plan.cells(plan.fixtures[0], Cache.WARM)
    samples = [
        schema.Sample(cell=c, seconds=float(rng.lognormal(math.log(medians[c.label]), 0.02)))
        for c in cells
        for _ in range(12)
    ]
    session = BenchSession(plan=plan, env=None, out=tmp_path, rng=random.Random(1), samples=samples)
    summary = session.summary()
    versus = {(p.tool.label, p.reference.label): p for p in summary.versus}
    assert set(versus) == {("remmy@1", "base@1"), ("remmy@4", "base@4")}
    assert versus["remmy@4", "base@4"].verdict is Verdict.DISTINCT
    assert 1.05 < versus["remmy@4", "base@4"].time_ratio < 1.15
    assert versus["remmy@1", "base@1"].verdict is Verdict.TIE

    doc = bmf.convert(json.loads(session.write_json(summary).read_text()))
    anchor = plan.fixtures[0].id
    assert "time-ratio-vs-base" in doc[f"demo/{anchor}/warm/remmy@4"]
    assert "time-ratio" in doc[f"demo/{anchor}/warm/base@4"]  # a baseline is judged against rm too
    assert "time-ratio" not in doc[f"demo/{anchor}/warm/xargs@4"]


@pytest.mark.parametrize("name", ["remmy", "rm", "Base", "has space", ""])
def test_baseline_names_are_checked(name):
    with pytest.raises(ValueError):
        registry.plan("demo", Path("/x/remmy"), baselines={name: Path("/x/base")})


def test_compare_fails_only_on_a_distinct_slowdown(tmp_path, capsys):
    import compare
    from session import BenchSession

    plan = registry.plan("demo", Path("/x/remmy"), baselines={"base": Path("/x/base")})
    rng = np.random.default_rng(5)

    def run_json(remmy4: float) -> Path:
        medians = {"rm@1": 1.0, "xargs@1": 1.3, "xargs@4": 0.7, "remmy@1": 1.0, "base@1": 1.0, "base@4": 0.4}
        medians["remmy@4"] = remmy4
        cells = plan.cells(plan.fixtures[0], Cache.WARM)
        samples = [
            schema.Sample(cell=c, seconds=float(rng.lognormal(math.log(medians[c.label]), 0.02)))
            for c in cells
            for _ in range(12)
        ]
        s = BenchSession(plan=plan, env=None, out=tmp_path / str(remmy4), rng=random.Random(1), samples=samples)
        return s.write_json(s.summary())

    assert compare.main([str(run_json(0.44)), "--fail-on", "base"]) == 1  # 10% slower
    assert "**Regression:**" in capsys.readouterr().out
    assert compare.main([str(run_json(0.36)), "--fail-on", "base"]) == 0  # 10% faster
    assert compare.main([str(run_json(0.401)), "--fail-on", "base"]) == 0  # unchanged
    assert compare.main([str(run_json(0.44))]) == 0  # reports without --fail-on


@pytest.mark.parametrize(("slowdown", "expected"), [(1.04, Verdict.DISTINCT), (1.005, Verdict.TIE)])
def test_versus_rule_resolves_small_shifts_at_measured_noise(slowdown, expected, tmp_path):
    """At the noise measured on the bench machine (~0.5% per sample), the 2% versus margin flags a 4% regression and
    calls a 0.5% one a tie."""
    from session import BenchSession

    plan = registry.plan("demo", Path("/x/remmy"), baselines={"base": Path("/x/base")})
    medians = {"rm@1": 1.0, "xargs@1": 1.3, "xargs@4": 0.7, "remmy@1": 1.0, "base@1": 1.0, "base@4": 0.4}
    medians["remmy@4"] = 0.4 * slowdown
    rng = np.random.default_rng(3)
    reps = plan.sampling[Cache.WARM].reps
    cells = plan.cells(plan.fixtures[0], Cache.WARM)
    samples = [
        schema.Sample(cell=c, seconds=float(rng.lognormal(math.log(medians[c.label]), 0.005)))
        for c in cells
        for _ in range(reps)
    ]
    summary = BenchSession(plan=plan, env=None, out=tmp_path, rng=random.Random(1), samples=samples).summary()
    pair = next(p for p in summary.versus if p.tool.label == "remmy@4")
    assert pair.verdict is expected
