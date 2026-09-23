"""Turn a :class:`~schema.Run` into a :class:`~schema.Summary`.

Deletion times are right-skewed, so every centre is a median and every effect
is a ratio of medians with a bootstrap interval. Trees are rebuilt for every
run, so tool and reference samples are independent: all resampling is
unpaired.
"""

from __future__ import annotations

import math
from collections import defaultdict
from collections.abc import Iterable, Sequence

import numpy as np
from schema import Cache, Cell, CellKey, DecisionRule, Interval, Pair, Run, Sample, Summary
from scipy import stats as sp

Array = np.ndarray

# --- Estimators -----------------------------------------------------------------


def median_ci(x: Array, *, resamples: int, seed: int, alpha: float = 0.05) -> Interval:
    """Percentile bootstrap CI of the median."""
    if x.size < 2:
        return Interval(math.nan, math.nan)
    rng = np.random.default_rng(seed)
    boot = np.median(x[rng.integers(0, x.size, (resamples, x.size))], axis=1)
    lo, hi = np.percentile(boot, [100 * alpha / 2, 100 * (1 - alpha / 2)])
    return Interval(float(lo), float(hi))


def bca_ratio_ci(
    tool: Array, reference: Array, *, resamples: int, seed: int, alpha: float = 0.05
) -> tuple[float, Interval]:
    """Ratio of medians with a BCa (bias-corrected, accelerated) bootstrap CI.

    The two groups are resampled independently. ``z0`` comes from the share of
    bootstrap ratios below the estimate; the acceleration ``a`` from a jackknife
    pooled over both groups (Efron & Tibshirani, ch. 14).
    """
    theta = float(np.median(tool) / np.median(reference))
    if tool.size < 2 or reference.size < 2:
        return theta, Interval(math.nan, math.nan)
    rng = np.random.default_rng(seed)
    boot_t = np.median(tool[rng.integers(0, tool.size, (resamples, tool.size))], axis=1)
    boot_r = np.median(reference[rng.integers(0, reference.size, (resamples, reference.size))], axis=1)
    boot = np.sort(boot_t / boot_r)

    share = np.count_nonzero(boot < theta) / resamples
    share = min(max(share, 1 / (2 * resamples)), 1 - 1 / (2 * resamples))
    z0 = sp.norm.ppf(share)

    jack = np.concatenate(
        [
            [np.median(np.delete(tool, i)) / np.median(reference) for i in range(tool.size)],
            [np.median(tool) / np.median(np.delete(reference, j)) for j in range(reference.size)],
        ]
    )
    d = jack.mean() - jack
    denom = 6 * np.sum(d**2) ** 1.5
    a = float(np.sum(d**3) / denom) if denom else 0.0

    def adjusted(z: float) -> float:
        return float(sp.norm.cdf(z0 + (z0 + z) / (1 - a * (z0 + z))))

    lo_p, hi_p = adjusted(sp.norm.ppf(alpha / 2)), adjusted(sp.norm.ppf(1 - alpha / 2))
    lo, hi = sorted(float(v) for v in np.percentile(boot, [100 * lo_p, 100 * hi_p]))
    return theta, Interval(lo, hi)


def cliffs_delta(tool: Array, reference: Array) -> float:
    """P(tool > ref) - P(tool < ref). Negative when the tool is faster."""
    diff = tool[:, None] - reference[None, :]
    return float((np.count_nonzero(diff > 0) - np.count_nonzero(diff < 0)) / diff.size)


def mann_whitney_p(tool: Array, reference: Array) -> float:
    """Two-sided Mann-Whitney U p-value."""
    return float(sp.mannwhitneyu(tool, reference, alternative="two-sided").pvalue)


def holm(pvalues: Sequence[float]) -> list[float]:
    """Holm-Bonferroni step-down adjusted p-values."""
    adjusted = [0.0] * len(pvalues)
    running = 0.0
    for position, i in enumerate(sorted(range(len(pvalues)), key=lambda i: pvalues[i])):
        running = max(running, (len(pvalues) - position) * pvalues[i])
        adjusted[i] = min(1.0, running)
    return adjusted


# --- Summary --------------------------------------------------------------------


def _seconds(samples: Iterable[Sample]) -> Array:
    return np.fromiter((s.seconds for s in samples), dtype=float)


def summarize(run: Run) -> Summary:
    plan = run.plan
    by_cell: dict[CellKey, list[Sample]] = defaultdict(list)
    for s in run.samples:
        by_cell[s.cell].append(s)

    cells = []
    for key, members in sorted(by_cell.items()):
        seconds = _seconds(members)
        median = float(np.median(seconds))
        cells.append(
            Cell(
                key=key,
                n=len(members),
                median_seconds=median,
                median_ci95=median_ci(seconds, resamples=plan.bootstrap_resamples, seed=plan.seed),
                throughput_files_per_s=plan.fixture(key.fixture).expected.files / median,
            )
        )
    return Summary(cells=tuple(cells), pairs=tuple(_pairs(run, by_cell, plan.rule)))


def _pairs(run: Run, by_cell: dict[CellKey, list[Sample]], rule: DecisionRule) -> list[Pair]:
    plan = run.plan
    families: dict[tuple[str, Cache], list[tuple[CellKey, CellKey, float, Interval, float, float]]] = defaultdict(list)
    for key in sorted(by_cell):
        if key.tool == plan.reference:
            continue
        ref_key = CellKey(key.fixture, plan.reference, 1, key.cache)
        tool, ref = _seconds(by_cell[key]), _seconds(by_cell.get(ref_key, []))
        if tool.size < 2 or ref.size < 2:
            continue
        ratio, ratio_ci = bca_ratio_ci(tool, ref, resamples=plan.bootstrap_resamples, seed=plan.seed)
        families[(key.fixture, key.cache)].append(
            (key, ref_key, ratio, ratio_ci, mann_whitney_p(tool, ref), cliffs_delta(tool, ref))
        )

    pairs = []
    # Holm within each (fixture, cache) family.
    for family in families.values():
        for (key, ref_key, ratio, ratio_ci, _p, delta), adj in zip(family, holm([f[4] for f in family]), strict=True):
            pairs.append(
                Pair(
                    tool=key,
                    reference=ref_key,
                    time_ratio=ratio,
                    time_ratio_ci95=ratio_ci,
                    verdict=rule.decide(ratio_ci, delta, adj),
                )
            )
    return sorted(pairs, key=lambda p: p.tool)


def headline(summary: Summary) -> Iterable[str]:
    """One line per compared cell: its time relative to the reference."""
    for p in summary.pairs:
        ci = p.time_ratio_ci95
        speed = f"{p.speedup:.2f}x faster" if p.time_ratio < 1 else f"{1 / p.speedup:.2f}x slower"
        yield (
            f"{p.tool.fixture}/{p.tool.cache} {p.tool.label:<10} {p.time_ratio:.3f}x {p.reference.label}'s time"
            f" [95% CI {ci.lo:.3f}, {ci.hi:.3f}]  {speed} ({p.verdict})"
        )
