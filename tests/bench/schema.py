"""Benchmark types. A cell is one (fixture, tool, threads, cache); a pair compares a cell with the reference tool's
cell in the same (fixture, cache): time_ratio = tool_median / reference_median, so below 1 means faster."""

import enum
import math
import re
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


class Cache(enum.StrEnum):
    WARM = "warm"  # tree just created and synced
    COLD = "cold"  # `purge` ran after creation


class Verdict(enum.StrEnum):
    DISTINCT = "distinct"
    TIE = "tie"
    INDETERMINATE = "indeterminate"


@dataclass(frozen=True, slots=True)
class Interval:
    """Closed [lo, hi]; NaN bounds mean undefined."""

    lo: float
    hi: float

    def __post_init__(self):
        _require(not self.defined or self.lo <= self.hi, f"interval out of order: [{self.lo}, {self.hi}]")

    @property
    def defined(self) -> bool:
        return not (math.isnan(self.lo) or math.isnan(self.hi))

    def contains(self, x: float) -> bool:
        return self.defined and self.lo <= x <= self.hi

    def entirely_outside(self, band: "Interval") -> bool:
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

    def __str__(self):
        return f"{self.fixture}/{self.cache}/{self.label}"


@dataclass(frozen=True, slots=True, kw_only=True)
class Tool:
    """argv uses {tree} and optionally {threads} (then the tool is swept over the plan's thread counts). env is set
    outside the timer (never via /usr/bin/env, whose extra exec would be timed). shell tools run via /bin/sh -c."""

    name: str
    argv: tuple[str, ...]
    shell: bool = False
    env: Mapping[str, str] = field(default_factory=dict)
    requires: tuple[str, ...] = ()  # executables that must exist

    def __post_init__(self):
        joined = " ".join(self.argv)
        _require("{tree}" in joined, f"{self.name}: argv never mentions {{tree}}")
        unknown = set(re.findall(r"\{[^}]*\}", " ".join((joined, *self.env.values())))) - {"{tree}", "{threads}"}
        _require(not unknown, f"{self.name}: unknown placeholders {sorted(unknown)}")
        _require(not self.shell or len(self.argv) == 1, f"{self.name}: shell tools take one string")

    @property
    def sweeps_threads(self) -> bool:
        return any("{threads}" in a for a in (*self.argv, *self.env.values()))

    def env_for(self, threads: int) -> dict[str, str]:
        return {k: v.replace("{threads}", str(threads)) for k, v in self.env.items()}

    def argv_for(self, tree: Path, threads: int) -> list[str]:
        filled = [a.replace("{tree}", str(tree)).replace("{threads}", str(threads)) for a in self.argv]
        return ["/bin/sh", "-c", filled[0]] if self.shell else filled


@dataclass(frozen=True, slots=True, kw_only=True)
class TreeCounts:
    dirs: int
    files: int


@dataclass(frozen=True, slots=True, kw_only=True)
class Fixture:
    """A mktree tree: `fanout` subdirs per dir down to `depth`, `files` files of `size` bytes in every dir."""

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
        return [a for k in ("depth", "fanout", "files", "size") for a in (f"--{k}", str(getattr(self, k)))]


@dataclass(frozen=True, slots=True, kw_only=True)
class DecisionRule:
    """DISTINCT: ratio CI entirely outside [1-margin, 1+margin] and |cliffs_delta| > cliff_threshold.
    TIE: ratio CI contains 1 (or is undefined), or Holm-adjusted p > alpha. INDETERMINATE otherwise."""

    margin: float = 0.05
    cliff_threshold: float = 0.33
    alpha: float = 0.05

    def decide(self, ratio_ci: Interval, cliffs_delta: float, holm_p: float) -> Verdict:
        band = Interval(1 - self.margin, 1 + self.margin)
        if ratio_ci.entirely_outside(band) and abs(cliffs_delta) > self.cliff_threshold:
            return Verdict.DISTINCT
        if not ratio_ci.defined or ratio_ci.contains(1.0) or holm_p > self.alpha:
            return Verdict.TIE
        return Verdict.INDETERMINATE


@dataclass(frozen=True, slots=True, kw_only=True)
class Sampling:
    reps: int  # timed runs per cell, split evenly across rounds
    rounds: int  # each round runs every cell once, freshly shuffled
    warmup: int  # untimed runs before each cell's timed runs in a round

    def __post_init__(self):
        _require(self.reps >= 2 and self.reps % self.rounds == 0, f"reps {self.reps} must split over {self.rounds}")

    @property
    def per_round(self) -> int:
        return self.reps // self.rounds


@dataclass(frozen=True, slots=True, kw_only=True)
class Plan:
    name: str
    fixtures: tuple[Fixture, ...]
    caches: Mapping[str, tuple[Cache, ...]]  # fixture id -> caches it is measured under
    tools: tuple[Tool, ...]
    threads: tuple[int, ...]
    sampling: Mapping[Cache, Sampling]
    reference: str
    rule: DecisionRule
    bootstrap_resamples: int
    seed: int

    def __post_init__(self):
        _require(self.reference in {t.name for t in self.tools}, f"reference {self.reference!r} is not a tool")
        _require(set(self.caches) == {f.id for f in self.fixtures}, "caches must name exactly the plan's fixtures")

    def fixture(self, fixture_id: str) -> Fixture:
        return next(f for f in self.fixtures if f.id == fixture_id)

    def tool(self, name: str) -> Tool:
        return next(t for t in self.tools if t.name == name)

    def cells(self, fixture: Fixture, cache: Cache) -> list[CellKey]:
        return [
            CellKey(fixture.id, t.name, n, cache)
            for t in self.tools
            for n in (self.threads if t.sweeps_threads else (1,))
        ]


@dataclass(frozen=True, slots=True, kw_only=True)
class Sample:
    cell: CellKey
    seconds: float


@dataclass(frozen=True, slots=True, kw_only=True)
class Run:
    plan: Plan
    samples: tuple[Sample, ...]


@dataclass(frozen=True, slots=True, kw_only=True)
class Cell:
    key: CellKey
    n: int
    median_seconds: float
    median_ci95: Interval
    throughput_files_per_s: float


@dataclass(frozen=True, slots=True, kw_only=True)
class Pair:
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
