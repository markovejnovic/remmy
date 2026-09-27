"""Run -> Summary. Deletion times are right-skewed, so centres are medians and effects are ratios of medians with
bootstrap CIs. Trees are rebuilt for every run, so samples are independent and all resampling is unpaired."""

import math
from collections import defaultdict

import numpy as np
from schema import Cell, CellKey, Interval, Pair, Run, Summary
from scipy import stats as sp


def _boot_medians(x, rng, resamples):
    return np.median(x[rng.integers(0, x.size, (resamples, x.size))], axis=1)


def median_ci(x, *, resamples: int, seed: int, alpha: float = 0.05) -> Interval:
    """Percentile bootstrap CI of the median."""
    if x.size < 2:
        return Interval(math.nan, math.nan)
    lo, hi = np.percentile(_boot_medians(x, np.random.default_rng(seed), resamples), [50 * alpha, 100 - 50 * alpha])
    return Interval(float(lo), float(hi))


def bca_ratio_ci(tool, reference, *, resamples: int, seed: int, alpha: float = 0.05) -> tuple[float, Interval]:
    """Ratio of medians with a BCa bootstrap CI; groups resampled independently, acceleration from a jackknife
    pooled over both groups (Efron & Tibshirani, ch. 14)."""
    theta = float(np.median(tool) / np.median(reference))
    if tool.size < 2 or reference.size < 2:
        return theta, Interval(math.nan, math.nan)
    rng = np.random.default_rng(seed)
    boot = np.sort(_boot_medians(tool, rng, resamples) / _boot_medians(reference, rng, resamples))

    share = np.count_nonzero(boot < theta) / resamples
    z0 = sp.norm.ppf(min(max(share, 1 / (2 * resamples)), 1 - 1 / (2 * resamples)))

    jack = np.array(
        [np.median(np.delete(tool, i)) / np.median(reference) for i in range(tool.size)]
        + [np.median(tool) / np.median(np.delete(reference, j)) for j in range(reference.size)]
    )
    d = jack.mean() - jack
    denom = 6 * np.sum(d**2) ** 1.5
    a = float(np.sum(d**3) / denom) if denom else 0.0

    def adjusted(q: float) -> float:
        z = sp.norm.ppf(q)
        return float(sp.norm.cdf(z0 + (z0 + z) / (1 - a * (z0 + z))))

    lo, hi = sorted(float(v) for v in np.percentile(boot, [100 * adjusted(alpha / 2), 100 * adjusted(1 - alpha / 2)]))
    return theta, Interval(lo, hi)


def cliffs_delta(tool, reference) -> float:
    """P(tool > ref) - P(tool < ref). Negative when the tool is faster."""
    diff = tool[:, None] - reference[None, :]
    return float((np.count_nonzero(diff > 0) - np.count_nonzero(diff < 0)) / diff.size)


def mann_whitney_p(tool, reference) -> float:
    return float(sp.mannwhitneyu(tool, reference, alternative="two-sided").pvalue)


def holm(pvalues) -> list[float]:
    """Holm-Bonferroni step-down adjusted p-values."""
    adjusted = [0.0] * len(pvalues)
    running = 0.0
    for position, i in enumerate(sorted(range(len(pvalues)), key=lambda i: pvalues[i])):
        running = max(running, (len(pvalues) - position) * pvalues[i])
        adjusted[i] = min(1.0, running)
    return adjusted


def summarize(run: Run) -> Summary:
    plan = run.plan
    by_cell: dict[CellKey, list[float]] = defaultdict(list)
    for s in run.samples:
        by_cell[s.cell].append(s.seconds)
    secs = {k: np.array(v, dtype=float) for k, v in sorted(by_cell.items())}

    cells = []
    for key, x in secs.items():
        median = float(np.median(x))
        ci = median_ci(x, resamples=plan.bootstrap_resamples, seed=plan.seed)
        rate = plan.fixture(key.fixture).expected.files / median
        cells.append(Cell(key=key, n=x.size, median_seconds=median, median_ci95=ci, throughput_files_per_s=rate))

    families = defaultdict(list)
    for key, tool in secs.items():
        ref_key = CellKey(key.fixture, plan.reference, 1, key.cache)
        ref = secs.get(ref_key, np.empty(0))
        if key.tool == plan.reference or tool.size < 2 or ref.size < 2:
            continue
        ratio, ci = bca_ratio_ci(tool, ref, resamples=plan.bootstrap_resamples, seed=plan.seed)
        p, delta = mann_whitney_p(tool, ref), cliffs_delta(tool, ref)
        families[key.fixture, key.cache].append((key, ref_key, ratio, ci, p, delta))

    pairs = []
    # Holm within each (fixture, cache) family.
    for family in families.values():
        for (key, ref_key, ratio, ci, _p, delta), adj in zip(family, holm([f[4] for f in family]), strict=True):
            verdict = plan.rule.decide(ci, delta, adj)
            pairs.append(Pair(tool=key, reference=ref_key, time_ratio=ratio, time_ratio_ci95=ci, verdict=verdict))
    return Summary(cells=tuple(cells), pairs=tuple(sorted(pairs, key=lambda p: p.tool)))


def headline(summary: Summary):
    """One line per compared cell: its time relative to the reference."""
    for p in summary.pairs:
        ci = p.time_ratio_ci95
        speed = f"{p.speedup:.2f}x faster" if p.time_ratio < 1 else f"{1 / p.speedup:.2f}x slower"
        yield (
            f"{p.tool.fixture}/{p.tool.cache} {p.tool.label:<10} {p.time_ratio:.3f}x {p.reference.label}'s time"
            f" [95% CI {ci.lo:.3f}, {ci.hi:.3f}]  {speed} ({p.verdict})"
        )
