"""Benchmark charts: plain matplotlib, drawn from a Summary."""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from schema import Cell, Run, Summary


def render(run: Run, summary: Summary, out: Path) -> list[Path]:
    """Write every chart the data supports; return the files written."""
    out.mkdir(parents=True, exist_ok=True)
    written = []
    matrices = sorted({(c.key.fixture, c.key.cache) for c in summary.cells})
    for fixture, cache in matrices:
        cells = [c for c in summary.cells if (c.key.fixture, c.key.cache) == (fixture, cache)]
        written.append(_save(_median_times(run, cells, fixture, cache), out / f"time-{fixture}-{cache}.png"))
        if len(run.plan.threads) > 1:
            written.append(_save(_scaling(run, cells, fixture, cache), out / f"scaling-{fixture}-{cache}.png"))
    if len({f for f, _ in matrices}) > 1:
        written.append(_save(_speedup_by_fixture(run, summary), out / "speedup-by-tree.png"))
    return written


def _save(fig: plt.Figure, path: Path) -> Path:
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path


def _label(run: Run, cell: Cell) -> str:
    swept = run.plan.tool(cell.key.tool).sweeps_threads
    return f"{cell.key.tool} ×{cell.key.threads}" if swept else cell.key.tool


def _median_times(run: Run, cells: list[Cell], fixture: str, cache: str) -> plt.Figure:
    cells = sorted(cells, key=lambda c: c.median_seconds, reverse=True)
    ms = [c.median_seconds * 1000 for c in cells]
    err = [
        [(c.median_seconds - c.median_ci95.lo) * 1000 for c in cells],
        [(c.median_ci95.hi - c.median_seconds) * 1000 for c in cells],
    ]
    fig, ax = plt.subplots(figsize=(8, 0.5 * len(cells) + 1.5))
    ax.barh([_label(run, c) for c in cells], ms, xerr=err, capsize=3)
    ax.set_xlabel("median time to delete (ms, 95% CI)")
    ax.set_title(f"{run.plan.fixture(fixture).title} tree ({fixture}), {cache} cache")
    ax.grid(axis="x", alpha=0.3)
    return fig


def _scaling(run: Run, cells: list[Cell], fixture: str, cache: str) -> plt.Figure:
    fig, ax = plt.subplots(figsize=(8, 5))
    swept: dict[str, list[Cell]] = defaultdict(list)
    for c in cells:
        if run.plan.tool(c.key.tool).sweeps_threads:
            swept[c.key.tool].append(c)
        else:
            ax.axhline(c.throughput_files_per_s, linestyle="--", color="gray")
            ax.annotate(
                c.key.tool,
                (1, c.throughput_files_per_s),
                xycoords=("axes fraction", "data"),
                xytext=(4, 0),
                textcoords="offset points",
                va="center",
            )
    for tool, members in swept.items():
        members.sort(key=lambda c: c.key.threads)
        ax.plot([c.key.threads for c in members], [c.throughput_files_per_s for c in members], marker="o", label=tool)
    ax.set_xscale("log", base=2)
    ax.set_xticks(run.plan.threads, [str(t) for t in run.plan.threads])
    ax.set_ylim(bottom=0)
    ax.set_xlabel("threads")
    ax.set_ylabel("files deleted per second")
    ax.set_title(f"Throughput by threads: {run.plan.fixture(fixture).title} tree ({fixture}), {cache} cache")
    ax.legend()
    ax.grid(alpha=0.3)
    return fig


def _speedup_by_fixture(run: Run, summary: Summary) -> plt.Figure:
    default = 4 if 4 in run.plan.threads else max(run.plan.threads)
    pairs = [
        p
        for p in summary.pairs
        if p.tool.cache == "warm" and (p.tool.threads == default or not run.plan.tool(p.tool.tool).sweeps_threads)
    ]
    fixtures = sorted({p.tool.fixture for p in pairs}, key=lambda f: int(f[1:]))
    tools = list(dict.fromkeys(p.tool.label for p in pairs))
    width = 0.8 / len(tools)
    fig, ax = plt.subplots(figsize=(10, 5))
    for i, label in enumerate(tools):
        by_fixture = {p.tool.fixture: p.speedup for p in pairs if p.tool.label == label}
        xs = [j + i * width for j, f in enumerate(fixtures) if f in by_fixture]
        ax.bar(xs, [by_fixture[f] for f in fixtures if f in by_fixture], width, label=label)
    ax.axhline(1, color="black", linewidth=0.8)
    ax.set_xticks([j + width * (len(tools) - 1) / 2 for j in range(len(fixtures))], fixtures)
    ax.set_ylabel(f"speedup vs {run.plan.reference} (median time ratio)")
    ax.set_title(f"Speedup vs {run.plan.reference} by tree, warm cache (parallel tools at {default} threads)")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    return fig
