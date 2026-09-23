"""Drive hyperfine through a plan's matrices and collect the samples."""

from __future__ import annotations

import json
import random
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from schema import Cache, CellKey, Fixture, Plan, Sample, TreeCounts

HERE = Path(__file__).resolve().parent


class BenchmarkFailed(Exception):
    """hyperfine failed, a tool exited non-zero, or a tree was built wrong."""


@dataclass(frozen=True, slots=True, kw_only=True)
class Environment:
    hyperfine: str
    mktree: Path
    scratch: Path
    """Directory the trees are built in; its volume is what is measured."""
    python: str = sys.executable


def round_orders(cells: list[CellKey], rounds: int, rng: random.Random) -> list[list[CellKey]]:
    """A freshly shuffled cell order for every round, so no tool always runs first."""
    return [rng.sample(cells, len(cells)) for _ in range(rounds)]


def run_matrix(
    plan: Plan, fixture: Fixture, cache: Cache, env: Environment, *, rng: random.Random, progress: object = print
) -> list[Sample]:
    sampling = plan.sampling[cache]
    samples: list[Sample] = []
    for round_no, order in enumerate(round_orders(plan.cells(fixture, cache), sampling.rounds, rng), start=1):
        for cell in order:
            progress(f"round {round_no}/{sampling.rounds}: {cell}")  # type: ignore[operator]
            times = _time_cell(plan, fixture, cell, env, runs=sampling.per_round, warmup=sampling.warmup)
            samples += [Sample(cell=cell, seconds=t) for t in times]
    return samples


def _time_cell(
    plan: Plan, fixture: Fixture, cell: CellKey, env: Environment, *, runs: int, warmup: int
) -> list[float]:
    work = env.scratch / f"{cell.fixture}-{cell.cache}-{cell.tool}-{cell.threads}"
    work.mkdir(parents=True, exist_ok=True)
    tree, state, export = work / "tree", work / "state.json", work / "hyperfine.json"

    prepare = [env.python, str(HERE / "prepare.py"), str(env.mktree), str(tree), str(state)]
    if cell.cache is Cache.COLD:
        prepare.append("--purge")
    prepare += ["--", *fixture.mktree_args()]
    command = [
        env.hyperfine, "--shell=none", "--style=none",
        "--runs", str(runs), "--warmup", str(warmup),
        "--prepare", shlex.join(prepare),
        "--export-json", str(export),
        "--command-name", str(cell),
        shlex.join(plan.tool(cell.tool).argv_for(tree, cell.threads)),
    ]  # fmt: skip
    done = subprocess.run(command, capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise BenchmarkFailed(f"{cell}: hyperfine exited {done.returncode}\n{done.stderr.strip() or done.stdout.strip()}")

    (result,) = json.loads(export.read_text())["results"]
    built = TreeCounts(**json.loads(state.read_text()))
    if built != fixture.expected:
        raise BenchmarkFailed(f"{cell}: built {built}, expected {fixture.expected}")
    if any(code != 0 for code in result.get("exit_codes", [])):
        raise BenchmarkFailed(f"{cell}: non-zero exit codes {result['exit_codes']}")
    return [float(t) for t in result["times"]]
