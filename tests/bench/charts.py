"""Benchmark charts drawn from a Summary, in a light and a dark theme (background painted in, so any page works)."""

import textwrap
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, NullLocator
from schema import Cell, Run, Summary

HIGHLIGHT = "remmy"  # drawn in the accent color; everything else recedes
OFFSET = {"textcoords": "offset points", "fontsize": 9}


@dataclass(frozen=True, slots=True)
class Theme:
    suffix: str
    surface: str
    ink: str
    ink_secondary: str
    grid: str
    baseline: str
    accent: str  # remmy
    second: str  # the other parallel tool
    rest: tuple[str, ...]  # single-threaded tools, darkest first
    muted: str = "#898781"


LIGHT = Theme(
    "", "#fcfcfb", "#0b0b0b", "#52514e", "#e1e0d9", "#898781", "#2a78d6", "#eb6834", ("#8f8e88", "#b1b0a9", "#cfcec7")
)
DARK = Theme(
    "-dark", "#1a1a19", "#fff", "#c3c2b7", "#2c2c2a", "#6b6a66", "#3987e5", "#d95926", ("#8f8e88", "#6b6a66", "#4a4a47")
)


def render(run: Run, summary: Summary, out: Path) -> list[Path]:
    """Write every chart the data supports, once per theme; return the files written."""
    out.mkdir(parents=True, exist_ok=True)
    charts = []
    matrices = sorted({(c.key.fixture, c.key.cache) for c in summary.cells})
    for fixture, cache in matrices:
        cells = [c for c in summary.cells if (c.key.fixture, c.key.cache) == (fixture, cache)]
        charts.append((f"time-{fixture}-{cache}", _median_times, (cells, fixture, cache)))
        if len(run.plan.threads) > 1:
            charts.append((f"scaling-{fixture}-{cache}", _scaling, (cells, fixture, cache)))
    if len({f for f, _ in matrices}) > 1:
        charts.append(("speedup-by-tree", _speedup_by_fixture, (summary,)))
    written = []
    for theme in (LIGHT, DARK):
        with plt.rc_context(_rc(theme)):
            for name, draw, args in charts:
                fig = draw(theme, run, *args)
                fig.savefig(path := out / f"{name}{theme.suffix}.svg", bbox_inches="tight", pad_inches=0.3)
                plt.close(fig)
                written.append(path)
    return written


def _rc(t: Theme) -> dict:
    return {
        "font.family": "sans-serif",
        "font.sans-serif": ["Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"],
        "font.size": 10,
        "text.color": t.ink,
        "axes.labelcolor": t.ink_secondary,
        "axes.edgecolor": t.baseline,
        "axes.linewidth": 0.8,
        "axes.titlelocation": "left",
        "axes.titlesize": 13,
        "axes.titleweight": "bold",
        "axes.titlepad": 22,
        "axes.axisbelow": True,
        "grid.color": t.grid,
        "grid.linewidth": 0.8,
        "svg.fonttype": "path",
        **dict.fromkeys(["axes.spines.top", "axes.spines.right", "axes.grid", "legend.frameon"], False),
        **dict.fromkeys(["axes.labelsize", "legend.fontsize"], 9.5),
        **dict.fromkeys(["xtick.color", "ytick.color"], t.muted),
        **dict.fromkeys(["xtick.labelcolor", "ytick.labelcolor"], t.ink_secondary),
        **dict.fromkeys(["xtick.major.size", "ytick.major.size"], 0),
        **dict.fromkeys(["xtick.major.pad", "ytick.major.pad"], 6),
        **dict.fromkeys(["figure.facecolor", "axes.facecolor", "savefig.facecolor"], t.surface),
    }


def _titles(ax, theme: Theme, title: str, subtitle: str) -> None:
    ax.set_title(title)
    ax.text(0, 1.02, subtitle, transform=ax.transAxes, color=theme.ink_secondary, fontsize=9.5, va="bottom")


def _tree(run: Run, fixture: str) -> str:
    e = run.plan.fixture(fixture).expected
    return f"{e.files:,} files in {e.dirs:,} directories"


def _swept(run: Run, tool: str) -> bool:
    return run.plan.tool(tool).sweeps_threads


def _color(theme: Theme, run: Run, tool: str) -> str:
    return theme.accent if tool == HIGHLIGHT else theme.second if _swept(run, tool) else theme.rest[0]


def _ink(theme: Theme, tool: str) -> str:
    return theme.ink if tool == HIGHLIGHT else theme.ink_secondary


def _median_times(theme: Theme, run: Run, cells: list[Cell], fixture: str, cache: str):
    cells = sorted(cells, key=lambda c: c.median_seconds)
    fig, ax = plt.subplots(figsize=(8, 0.34 * len(cells) + 1.2))
    ys = range(len(cells))
    meds = [c.median_seconds for c in cells]
    ax.barh(ys, meds, height=0.62, color=[_color(theme, run, c.key.tool) for c in cells])
    err = [[c.median_seconds - c.median_ci95.lo for c in cells], [c.median_ci95.hi - c.median_seconds for c in cells]]
    ax.errorbar(meds, ys, xerr=err, fmt="none", ecolor=theme.ink_secondary, elinewidth=1, capsize=0)
    tips = [max(c.median_seconds, c.median_ci95.hi) if c.median_ci95.defined else c.median_seconds for c in cells]
    for y, c, tip in zip(ys, cells, tips, strict=True):
        s = c.median_seconds
        text = f"{s:.2f} s" if s >= 1 else f"{s * 1000:.0f} ms"
        ax.annotate(text, (tip, y), xytext=(5, 0), va="center", color=_ink(theme, c.key.tool), **OFFSET)
    labels = [f"{c.key.tool} ×{c.key.threads}" if _swept(run, c.key.tool) else c.key.tool for c in cells]
    ax.set_yticks(ys, labels)
    ax.invert_yaxis()
    ax.set_ylim(len(cells) - 0.4, -0.6)
    ax.spines[["left", "bottom"]].set_visible(False)
    ax.xaxis.set_major_locator(NullLocator())
    for label in ax.get_yticklabels():
        if label.get_text().split(" ")[0] == HIGHLIGHT:
            label.set_color(theme.ink)
            label.set_fontweight("bold")
    ax.set_xlim(0, max(tips) * 1.12)
    sub = f"{_tree(run, fixture)}. Lower is better; whiskers are 95% CIs."
    _titles(ax, theme, f"Median time to delete, {cache} cache", sub)
    return fig


def _scaling(theme: Theme, run: Run, cells: list[Cell], fixture: str, cache: str):
    fig, ax = plt.subplots(figsize=(8, 4.6))
    swept = defaultdict(list)
    for c in cells:
        swept[c.key.tool if _swept(run, c.key.tool) else None].append(c)
    flat = swept.pop(None, [])
    threads = run.plan.threads
    if flat:
        rates = [c.throughput_files_per_s for c in flat]
        ax.axhspan(min(rates), max(rates), color=theme.rest[1], alpha=0.45, linewidth=0)
        text = f"single-threaded tools ({', '.join(sorted(c.key.tool for c in flat))})"
        style = {"ha": "right", "va": "top", "color": theme.ink_secondary}
        ax.annotate(text, (threads[-1], min(rates)), xytext=(0, -5), **style, **OFFSET)
    for tool in sorted(swept, key=lambda t: t == HIGHLIGHT):  # remmy last, so it draws on top
        members = sorted(swept[tool], key=lambda c: c.key.threads)
        xs, ys = [c.key.threads for c in members], [c.throughput_files_per_s for c in members]
        color = _color(theme, run, tool)
        ax.plot(xs, ys, color=color, linewidth=2, solid_capstyle="round", solid_joinstyle="round", label=tool, zorder=3)
        ax.scatter(xs, ys, s=42, color=color, edgecolors="none", zorder=4)
        peak = max(members, key=lambda c: c.throughput_files_per_s)
        xy = (peak.key.threads, peak.throughput_files_per_s)
        ax.annotate(f"{xy[1] / 1000:.0f}k", xy, xytext=(0, 8), ha="center", color=_ink(theme, tool), **OFFSET)
    ax.set_xscale("log", base=2)
    ax.set_xticks(threads, [str(t) for t in threads])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_xlim(threads[0] / 1.25, threads[-1] * 1.25)
    ax.set_ylim(bottom=0, top=ax.get_ylim()[1] * 1.08)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v / 1000:.0f}k" if v else "0"))
    ax.grid(axis="y")
    ax.spines["left"].set_visible(False)
    ax.set_xlabel("threads (jobs, for xargs)")
    _titles(ax, theme, f"Files deleted per second, {cache} cache", f"{_tree(run, fixture)}. Higher is better.")
    handles, labels = ax.get_legend_handles_labels()
    ax.legend(handles[::-1], labels[::-1], loc="upper left", handlelength=1.4)
    return fig


def _speedup_by_fixture(theme: Theme, run: Run, summary: Summary):
    default = 4 if 4 in run.plan.threads else max(run.plan.threads)
    shown = [p for p in summary.pairs if p.tool.threads == default or not _swept(run, p.tool.tool)]
    pairs = [p for p in shown if p.tool.cache == "warm"]
    fixtures = sorted({p.tool.fixture for p in pairs}, key=lambda f: int(f[1:]))
    tools = sorted({p.tool.tool for p in pairs}, key=lambda t: (t != HIGHLIGHT, not _swept(run, t), t))
    rest = iter(theme.rest)
    width = min(0.8 / len(tools), 0.17)
    fig, ax = plt.subplots(figsize=(10, 4.8))
    for i, tool in enumerate(tools):
        color = _color(theme, run, tool) if _swept(run, tool) else next(rest)
        by_fixture = {p.tool.fixture: p.speedup for p in pairs if p.tool.tool == tool}
        xs = [j + (i - (len(tools) - 1) / 2) * width for j, f in enumerate(fixtures) if f in by_fixture]
        heights = [by_fixture[f] for f in fixtures if f in by_fixture]
        label = f"{tool} ×{default}" if _swept(run, tool) else tool
        # Bars grow from 1× (no change): up when faster than the reference, down when slower.
        ax.bar(xs, [h - 1 for h in heights], width * 0.86, bottom=1, color=color, label=label)
        if tool == HIGHLIGHT:
            for x, h in zip(xs, heights, strict=True):
                # A slower bar hangs below 1×, so its label sits above the line, clear of the bars beside it.
                text = f"{h:.1f}×" if h >= 0.95 else f"{h:.2f}×"
                style = {"ha": "center", "va": "bottom", "fontweight": "bold", "color": theme.ink}
                ax.annotate(text, (x, max(h, 1)), xytext=(0, 4), **style, **OFFSET)
    ax.set_yscale("log")
    ticks = [0.25, 0.5, 1, 2, 4]
    ax.set_yticks(ticks, [f"{t:g}×" for t in ticks])
    ax.yaxis.set_minor_locator(NullLocator())
    ax.set_ylim(0.4, 4.2)
    ax.axhline(1, color=theme.baseline, linewidth=1)
    ax.grid(axis="y")
    ax.spines[["left", "bottom"]].set_visible(False)
    ax.set_xticks(range(len(fixtures)), [textwrap.fill(run.plan.fixture(f).title, 12) for f in fixtures])
    ref = run.plan.reference
    sub = f"Median time ratio; above 1× is faster than {ref}. Parallel tools at {default} threads."
    _titles(ax, theme, f"Speedup over {ref} by tree shape, warm cache", sub)
    ax.legend(loc="upper left", bbox_to_anchor=(1, 1), handlelength=1, handleheight=1)
    return fig
