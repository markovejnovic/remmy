"""Benchmark charts: matplotlib, drawn from a Summary, in a light and a dark theme."""

from __future__ import annotations

import contextlib
import textwrap
from collections import defaultdict
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, NullLocator
from schema import Cell, Run, Summary

HIGHLIGHT = "remmy"
"""The tool the charts are about; drawn in the accent color, everything else recedes."""


@dataclass(frozen=True, slots=True)
class Theme:
    """Colors for one chart background. The background is painted in, so a chart reads on any page."""

    suffix: str
    surface: str
    ink: str
    ink_secondary: str
    muted: str
    grid: str
    baseline: str
    accent: str
    """remmy."""
    second: str
    """The other parallel tool."""
    rest: tuple[str, ...]
    """Single-threaded tools, darkest first."""


LIGHT = Theme(
    suffix="",
    surface="#fcfcfb",
    ink="#0b0b0b",
    ink_secondary="#52514e",
    muted="#898781",
    grid="#e1e0d9",
    baseline="#898781",
    accent="#2a78d6",
    second="#eb6834",
    rest=("#8f8e88", "#b1b0a9", "#cfcec7"),
)
DARK = Theme(
    suffix="-dark",
    surface="#1a1a19",
    ink="#ffffff",
    ink_secondary="#c3c2b7",
    muted="#898781",
    grid="#2c2c2a",
    baseline="#6b6a66",
    accent="#3987e5",
    second="#d95926",
    rest=("#8f8e88", "#6b6a66", "#4a4a47"),
)


def render(run: Run, summary: Summary, out: Path) -> list[Path]:
    """Write every chart the data supports, once per theme; return the files written."""
    out.mkdir(parents=True, exist_ok=True)
    charts: list[tuple[str, Callable[[Theme], plt.Figure]]] = []
    matrices = sorted({(c.key.fixture, c.key.cache) for c in summary.cells})
    for fixture, cache in matrices:
        cells = [c for c in summary.cells if (c.key.fixture, c.key.cache) == (fixture, cache)]
        charts.append(
            (f"time-{fixture}-{cache}", lambda t, c=cells, f=fixture, k=cache: _median_times(t, run, c, f, k))
        )
        if len(run.plan.threads) > 1:
            charts.append(
                (f"scaling-{fixture}-{cache}", lambda t, c=cells, f=fixture, k=cache: _scaling(t, run, c, f, k))
            )
    if len({f for f, _ in matrices}) > 1:
        charts.append(("speedup-by-tree", lambda t: _speedup_by_fixture(t, run, summary)))
    written = []
    for theme in (LIGHT, DARK):
        with _styled(theme):
            for name, draw in charts:
                written.append(_save(draw(theme), out / f"{name}{theme.suffix}.svg"))
    return written


@contextlib.contextmanager
def _styled(theme: Theme) -> Iterator[None]:
    rc = {
        "font.family": "sans-serif",
        "font.sans-serif": ["Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"],
        "font.size": 10,
        "text.color": theme.ink,
        "axes.labelcolor": theme.ink_secondary,
        "axes.labelsize": 9.5,
        "axes.edgecolor": theme.baseline,
        "axes.linewidth": 0.8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.titlelocation": "left",
        "axes.titlesize": 13,
        "axes.titleweight": "bold",
        "axes.titlepad": 22,
        "axes.axisbelow": True,
        "axes.grid": False,
        "grid.color": theme.grid,
        "grid.linewidth": 0.8,
        "xtick.color": theme.muted,
        "ytick.color": theme.muted,
        "xtick.labelcolor": theme.ink_secondary,
        "ytick.labelcolor": theme.ink_secondary,
        "xtick.major.size": 0,
        "ytick.major.size": 0,
        "xtick.major.pad": 6,
        "ytick.major.pad": 6,
        "legend.frameon": False,
        "legend.fontsize": 9.5,
        "figure.facecolor": theme.surface,
        "axes.facecolor": theme.surface,
        "savefig.facecolor": theme.surface,
        "svg.fonttype": "path",
    }
    with plt.rc_context(rc):
        yield


def _save(fig: plt.Figure, path: Path) -> Path:
    fig.savefig(path, bbox_inches="tight", pad_inches=0.3)
    plt.close(fig)
    return path


def _subtitle(ax: plt.Axes, theme: Theme, text: str) -> None:
    ax.text(0, 1.02, text, transform=ax.transAxes, color=theme.ink_secondary, fontsize=9.5, va="bottom")


def _tree(run: Run, fixture: str) -> str:
    f = run.plan.fixture(fixture)
    return f"{f.expected.files:,} files in {f.expected.dirs:,} directories"


def _label(run: Run, cell: Cell) -> str:
    swept = run.plan.tool(cell.key.tool).sweeps_threads
    return f"{cell.key.tool} ×{cell.key.threads}" if swept else cell.key.tool


def _color(theme: Theme, run: Run, tool: str) -> str:
    if tool == HIGHLIGHT:
        return theme.accent
    if run.plan.tool(tool).sweeps_threads:
        return theme.second
    return theme.rest[0]


def _duration(seconds: float) -> str:
    return f"{seconds:.2f} s" if seconds >= 1 else f"{seconds * 1000:.0f} ms"


def _median_times(theme: Theme, run: Run, cells: list[Cell], fixture: str, cache: str) -> plt.Figure:
    cells = sorted(cells, key=lambda c: c.median_seconds)
    fig, ax = plt.subplots(figsize=(8, 0.34 * len(cells) + 1.2))
    ys = range(len(cells))
    ax.barh(ys, [c.median_seconds for c in cells], height=0.62, color=[_color(theme, run, c.key.tool) for c in cells])
    ax.errorbar(
        [c.median_seconds for c in cells],
        ys,
        xerr=[
            [c.median_seconds - c.median_ci95.lo for c in cells],
            [c.median_ci95.hi - c.median_seconds for c in cells],
        ],
        fmt="none",
        ecolor=theme.ink_secondary,
        elinewidth=1,
        capsize=0,
    )
    for y, c in zip(ys, cells, strict=True):
        tip = max(c.median_seconds, c.median_ci95.hi) if c.median_ci95.defined else c.median_seconds
        ax.annotate(
            _duration(c.median_seconds),
            (tip, y),
            xytext=(5, 0),
            textcoords="offset points",
            va="center",
            fontsize=9,
            color=theme.ink if c.key.tool == HIGHLIGHT else theme.ink_secondary,
        )
    ax.set_yticks(ys, [_label(run, c) for c in cells])
    ax.invert_yaxis()
    ax.set_ylim(len(cells) - 0.4, -0.6)
    ax.spines["left"].set_visible(False)
    ax.spines["bottom"].set_visible(False)
    ax.xaxis.set_major_locator(NullLocator())
    for label in ax.get_yticklabels():
        if label.get_text().split(" ")[0] == HIGHLIGHT:
            label.set_color(theme.ink)
            label.set_fontweight("bold")
    ax.set_xlim(0, max(c.median_ci95.hi if c.median_ci95.defined else c.median_seconds for c in cells) * 1.12)
    ax.set_title(f"Median time to delete, {cache} cache")
    _subtitle(ax, theme, f"{_tree(run, fixture)}. Lower is better; whiskers are 95% CIs.")
    return fig


def _thousands(value: float, _pos: int) -> str:
    return f"{value / 1000:.0f}k" if value else "0"


def _scaling(theme: Theme, run: Run, cells: list[Cell], fixture: str, cache: str) -> plt.Figure:
    fig, ax = plt.subplots(figsize=(8, 4.6))
    swept: dict[str, list[Cell]] = defaultdict(list)
    flat: list[Cell] = []
    for c in cells:
        (swept[c.key.tool] if run.plan.tool(c.key.tool).sweeps_threads else flat).append(c)
    threads = run.plan.threads
    if flat:
        rates = [c.throughput_files_per_s for c in flat]
        ax.axhspan(min(rates), max(rates), color=theme.rest[1], alpha=0.45, linewidth=0)
        names = ", ".join(sorted(c.key.tool for c in flat))
        ax.annotate(
            f"single-threaded tools ({names})",
            (threads[-1], min(rates)),
            xytext=(0, -5),
            textcoords="offset points",
            ha="right",
            va="top",
            fontsize=9,
            color=theme.ink_secondary,
        )
    # remmy last, so it draws on top.
    for tool in sorted(swept, key=lambda t: t == HIGHLIGHT):
        members = sorted(swept[tool], key=lambda c: c.key.threads)
        xs = [c.key.threads for c in members]
        ys = [c.throughput_files_per_s for c in members]
        color = _color(theme, run, tool)
        ax.plot(xs, ys, color=color, linewidth=2, solid_capstyle="round", solid_joinstyle="round", label=tool, zorder=3)
        ax.scatter(xs, ys, s=42, color=color, edgecolors="none", zorder=4)
        peak = max(members, key=lambda c: c.throughput_files_per_s)
        ax.annotate(
            f"{peak.throughput_files_per_s / 1000:.0f}k",
            (peak.key.threads, peak.throughput_files_per_s),
            xytext=(0, 8),
            textcoords="offset points",
            ha="center",
            fontsize=9,
            color=theme.ink if tool == HIGHLIGHT else theme.ink_secondary,
        )
    ax.set_xscale("log", base=2)
    ax.set_xticks(threads, [str(t) for t in threads])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_xlim(threads[0] / 1.25, threads[-1] * 1.25)
    ax.set_ylim(bottom=0, top=ax.get_ylim()[1] * 1.08)
    ax.yaxis.set_major_formatter(FuncFormatter(_thousands))
    ax.grid(axis="y")
    ax.spines["left"].set_visible(False)
    ax.set_xlabel("threads (jobs, for xargs)")
    ax.set_title(f"Files deleted per second, {cache} cache")
    _subtitle(ax, theme, f"{_tree(run, fixture)}. Higher is better.")
    handles, labels = ax.get_legend_handles_labels()
    ax.legend(handles[::-1], labels[::-1], loc="upper left", handlelength=1.4)
    return fig


def _speedup_label(speedup: float) -> str:
    return f"{speedup:.1f}×" if speedup >= 0.95 else f"{speedup:.2f}×"


def _speedup_by_fixture(theme: Theme, run: Run, summary: Summary) -> plt.Figure:
    default = 4 if 4 in run.plan.threads else max(run.plan.threads)
    pairs = [
        p
        for p in summary.pairs
        if p.tool.cache == "warm" and (p.tool.threads == default or not run.plan.tool(p.tool.tool).sweeps_threads)
    ]
    fixtures = sorted({p.tool.fixture for p in pairs}, key=lambda f: int(f[1:]))
    tools = sorted(
        dict.fromkeys(p.tool.tool for p in pairs),
        key=lambda t: (t != HIGHLIGHT, not run.plan.tool(t).sweeps_threads, t),
    )
    rest = iter(theme.rest)
    colors = {t: _color(theme, run, t) if run.plan.tool(t).sweeps_threads else next(rest) for t in tools}
    width = min(0.8 / len(tools), 0.17)
    fig, ax = plt.subplots(figsize=(10, 4.8))
    for i, tool in enumerate(tools):
        by_fixture = {p.tool.fixture: p.speedup for p in pairs if p.tool.tool == tool}
        xs = [j + (i - (len(tools) - 1) / 2) * width for j, f in enumerate(fixtures) if f in by_fixture]
        heights = [by_fixture[f] for f in fixtures if f in by_fixture]
        swept = run.plan.tool(tool).sweeps_threads
        # Bars grow from 1× (no change), up when faster than the reference and down when slower.
        ax.bar(
            xs,
            [h - 1 for h in heights],
            width * 0.86,
            bottom=1,
            color=colors[tool],
            label=f"{tool} ×{default}" if swept else tool,
        )
        if tool == HIGHLIGHT:
            for x, h in zip(xs, heights, strict=True):
                ax.annotate(
                    _speedup_label(h),
                    # A slower bar hangs below 1×, so its label sits above the line, clear of the bars beside it.
                    (x, max(h, 1)),
                    xytext=(0, 4),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=9,
                    fontweight="bold",
                    color=theme.ink,
                )
    ax.set_yscale("log")
    ticks = [0.25, 0.5, 1, 2, 4]
    ax.set_yticks(ticks, [f"{t:g}×" for t in ticks])
    ax.yaxis.set_minor_locator(NullLocator())
    ax.set_ylim(0.4, 4.2)
    ax.axhline(1, color=theme.baseline, linewidth=1)
    ax.grid(axis="y")
    ax.spines["left"].set_visible(False)
    ax.spines["bottom"].set_visible(False)
    ax.set_xticks(range(len(fixtures)), [textwrap.fill(run.plan.fixture(f).title, 12) for f in fixtures])
    ax.set_title(f"Speedup over {run.plan.reference} by tree shape, warm cache")
    _subtitle(
        ax,
        theme,
        f"Median time ratio; above 1× is faster than {run.plan.reference}. Parallel tools at {default} threads.",
    )
    ax.legend(loc="upper left", bbox_to_anchor=(1, 1), handlelength=1, handleheight=1)
    return fig
