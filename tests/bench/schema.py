"""Types for remmy's comparison benchmark.

- A *tool* is a deletion program under comparison (``rm``, ``remmy``, ...).
- A *fixture* is a synthetic tree shape, built fresh before every timed run.
- A *cell* is one (fixture, tool, threads, cache) combination; its samples are
  the wall-clock times of repeated deletions.
- A *pair* compares a cell with the reference tool's cell in the same
  (fixture, cache). ``time_ratio = tool_median / reference_median``, so a ratio
  below 1 means the tool is faster; ``speedup = 1 / time_ratio``.
"""

from __future__ import annotations

import enum
import math
import re
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


class Cache(enum.StrEnum):
    """Filesystem cache state at the start of a timed deletion."""

    WARM = "warm"
    """Tree just created and synced: metadata is hot in memory."""
    COLD = "cold"
    """``purge`` ran after creation: approximates a cold filesystem cache."""


class Verdict(enum.StrEnum):
    DISTINCT = "distinct"
    TIE = "tie"
    INDETERMINATE = "indeterminate"


@dataclass(frozen=True, slots=True)
class Interval:
    """A closed interval ``[lo, hi]``; NaN bounds mean undefined."""

    lo: float
    hi: float

    def __post_init__(self) -> None:
        _require(not self.defined or self.lo <= self.hi, f"interval out of order: [{self.lo}, {self.hi}]")

    @property
    def defined(self) -> bool:
        return not (math.isnan(self.lo) or math.isnan(self.hi))

    def contains(self, x: float) -> bool:
        return self.defined and self.lo <= x <= self.hi

    def entirely_outside(self, band: Interval) -> bool:
        return self.defined and (self.hi < band.lo or self.lo > band.hi)


@dataclass(frozen=True, slots=True, order=True)
class CellKey:
    fixture: str
    tool: str
    threads: int
    cache: Cache

    @property
    def label(self) -> str:
        return f"{self.tool}@{self.threads}"

    def __str__(self) -> str:
        return f"{self.fixture}/{self.cache}/{self.label}"


# --- What is measured ------------------------------------------------------------


@dataclass(frozen=True, slots=True, kw_only=True)
class Tool:
    """A deletion program and exactly how it is invoked.

    ``argv`` uses ``{tree}`` (required) and optionally ``{threads}``; a tool
    whose argv mentions ``{threads}`` is swept over the plan's thread counts.
    ``shell`` tools are one pipeline string run through ``/bin/sh -c``.
    """

    name: str
    argv: tuple[str, ...]
    shell: bool = False
    requires: tuple[str, ...] = ()
    """Executables that must exist for the tool to be benchmarked."""

    def __post_init__(self) -> None:
        joined = " ".join(self.argv)
        _require("{tree}" in joined, f"{self.name}: argv never mentions {{tree}}")
        unknown = set(re.findall(r"\{[^}]*\}", joined)) - {"{tree}", "{threads}"}
        _require(not unknown, f"{self.name}: unknown placeholders {sorted(unknown)}")
        _require(not self.shell or len(self.argv) == 1, f"{self.name}: shell tools take one string")

    @property
    def sweeps_threads(self) -> bool:
        return any("{threads}" in a for a in self.argv)

    def argv_for(self, tree: Path, threads: int) -> list[str]:
        filled = [a.replace("{tree}", str(tree)).replace("{threads}", str(threads)) for a in self.argv]
        return ["/bin/sh", "-c", filled[0]] if self.shell else filled


@dataclass(frozen=True, slots=True, kw_only=True)
class TreeCounts:
    dirs: int
    files: int


@dataclass(frozen=True, slots=True, kw_only=True)
class Fixture:
    """A tree built by ``mktree``: ``fanout`` subdirectories per directory down
    to ``depth``, and ``files`` files of ``size`` bytes in every directory."""

    id: str
    title: str
    depth: int
    fanout: int
    files: int
    size: int = 0

    @property
    def expected(self) -> TreeCounts:
        dirs = sum(self.fanout**level for level in range(self.depth + 1))
        return TreeCounts(dirs=dirs, files=dirs * self.files)

    def mktree_args(self) -> list[str]:
        return [
            "--depth", str(self.depth), "--fanout", str(self.fanout),
            "--files", str(self.files), "--size", str(self.size),
        ]  # fmt: skip


@dataclass(frozen=True, slots=True, kw_only=True)
class DecisionRule:
    """Turns a pair's statistics into a verdict.

    - DISTINCT: the ratio CI lies entirely outside ``[1-margin, 1+margin]``
      and ``|cliffs_delta| > cliff_threshold``.
    - TIE: the ratio CI contains 1, or the Holm-adjusted p exceeds alpha.
    - INDETERMINATE otherwise.
    """

    margin: float = 0.05
    cliff_threshold: float = 0.33
    alpha: float = 0.05

    @property
    def tie_band(self) -> Interval:
        return Interval(1 - self.margin, 1 + self.margin)

    def decide(self, ratio_ci: Interval, cliffs_delta: float, holm_p: float) -> Verdict:
        if ratio_ci.entirely_outside(self.tie_band) and abs(cliffs_delta) > self.cliff_threshold:
            return Verdict.DISTINCT
        if not ratio_ci.defined or ratio_ci.contains(1.0) or holm_p > self.alpha:
            return Verdict.TIE
        return Verdict.INDETERMINATE


@dataclass(frozen=True, slots=True, kw_only=True)
class Sampling:
    reps: int
    """Timed runs per cell, split evenly across rounds."""
    rounds: int
    """Each round runs every cell once, in a freshly shuffled order."""
    warmup: int
    """Untimed runs before each cell's timed runs in a round."""

    def __post_init__(self) -> None:
        _require(self.reps >= 2 and self.reps % self.rounds == 0, f"reps {self.reps} must split over {self.rounds} rounds")

    @property
    def per_round(self) -> int:
        return self.reps // self.rounds


@dataclass(frozen=True, slots=True, kw_only=True)
class Plan:
    name: str
    fixtures: tuple[Fixture, ...]
    caches: Mapping[str, tuple[Cache, ...]]
    """Fixture id -> caches it is measured under."""
    tools: tuple[Tool, ...]
    threads: tuple[int, ...]
    sampling: Mapping[Cache, Sampling]
    reference: str
    rule: DecisionRule
    bootstrap_resamples: int
    seed: int

    def __post_init__(self) -> None:
        _require(self.reference in {t.name for t in self.tools}, f"reference {self.reference!r} is not a tool")
        _require(set(self.caches) == {f.id for f in self.fixtures}, "caches must name exactly the plan's fixtures")

    def fixture(self, fixture_id: str) -> Fixture:
        return next(f for f in self.fixtures if f.id == fixture_id)

    def tool(self, name: str) -> Tool:
        return next(t for t in self.tools if t.name == name)

    def cells(self, fixture: Fixture, cache: Cache) -> list[CellKey]:
        """All cells of one (fixture, cache) matrix, in registration order."""
        return [
            CellKey(fixture.id, t.name, n, cache)
            for t in self.tools
            for n in (self.threads if t.sweeps_threads else (1,))
        ]


# --- What was observed --------------------------------------------------------------


@dataclass(frozen=True, slots=True, kw_only=True)
class Sample:
    """One timed deletion of a fresh tree."""

    cell: CellKey
    seconds: float


@dataclass(frozen=True, slots=True, kw_only=True)
class Run:
    plan: Plan
    samples: tuple[Sample, ...]


# --- What was concluded --------------------------------------------------------------


@dataclass(frozen=True, slots=True, kw_only=True)
class Cell:
    key: CellKey
    n: int
    median_seconds: float
    median_ci95: Interval
    throughput_files_per_s: float


@dataclass(frozen=True, slots=True, kw_only=True)
class Pair:
    """A cell compared with the reference cell of the same (fixture, cache)."""

    tool: CellKey
    reference: CellKey
    time_ratio: float
    time_ratio_ci95: Interval
    verdict: Verdict

    @property
    def speedup(self) -> float:
        return 1 / self.time_ratio


@dataclass(frozen=True, slots=True, kw_only=True)
class Summary:
    cells: tuple[Cell, ...]
    pairs: tuple[Pair, ...]

    def pair(self, key: CellKey) -> Pair | None:
        return next((p for p in self.pairs if p.tool == key), None)
