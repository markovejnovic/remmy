"""What the benchmark compares: tools, trees, sampling and the decision rule."""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from schema import (
    Cache,
    DecisionRule,
    Fixture,
    Plan,
    Sampling,
    Tool,
)

SEED = 20260914
REFERENCE = "rm"


def _brew(name: str) -> str:
    """Absolute path of a Homebrew tool, never a shell alias or shim."""
    for prefix in ("/opt/homebrew/bin", "/usr/local/bin"):
        candidate = Path(prefix) / name
        if os.access(candidate, os.X_OK):
            return str(candidate)
    return shutil.which(name) or f"/opt/homebrew/bin/{name}"


def perf_tools(remmy: Path) -> tuple[Tool, ...]:
    """The timed competitors. Every one unlinks; none moves to a trash."""
    grm, bfs = _brew("grm"), _brew("bfs")
    return (
        # BSD rm (fts). The baseline every ratio is taken against.
        Tool(
            name="rm",
            argv=("/bin/rm", "-rf", "--", "{tree}"),
            requires=("/bin/rm",),
        ),
        # GNU coreutils rm; a separate baseline, never merged with BSD rm.
        Tool(
            name="grm",
            argv=(grm, "-rf", "--", "{tree}"),
            requires=(grm,),
        ),
        # BSD find, depth-first. Absolute path: a shell's `find` may be a bfs alias.
        Tool(
            name="find",
            argv=("/usr/bin/find", "{tree}", "-depth", "-delete"),
            requires=("/usr/bin/find",),
        ),
        # bfs, breadth-first traversal: different directory locality than depth-first.
        Tool(
            name="bfs",
            argv=(bfs, "{tree}", "-delete"),
            requires=(bfs,),
        ),
        # The strongest shell parallel deleter: parallel unlinks of every non-directory,
        # then a serial directory sweep. Runs via /bin/sh, so a ~1 ms shell fork is
        # inside its timer. (`-type f` would silently leave symlinks and the tree.)
        Tool(
            name="xargs",
            argv=(
                (
                    "/usr/bin/find {tree} -depth ! -type d -print0"
                    " | /usr/bin/xargs -0 -P{threads} -n256 /bin/rm -f"
                    " ; /usr/bin/find {tree} -depth -type d -delete"
                ),
            ),
            shell=True,
            requires=("/usr/bin/find", "/usr/bin/xargs", "/bin/rm"),
        ),
        # The subject. Its default thread count is 4 (the sweep includes it).
        Tool(
            name="remmy",
            argv=("/usr/bin/env", "REMMY_THREADS={threads}", str(remmy), "-r", "{tree}"),
            requires=(str(remmy),),
        ),
    )


FIXTURES = (
    Fixture(id="F0", title="Anchor", depth=3, fanout=8, files=100),
    Fixture(id="F1", title="Shallow and wide", depth=1, fanout=512, files=100),
    Fixture(id="F2", title="Deep and narrow", depth=9, fanout=2, files=57),
    Fixture(id="F3", title="Small files (4 KiB)", depth=3, fanout=8, files=100, size=4096),
    Fixture(id="F4", title="Large files (1 MiB)", depth=2, fanout=6, files=45, size=1 << 20),
    Fixture(id="F5", title="Tiny tree", depth=1, fanout=4, files=25),
    Fixture(id="F6", title="Huge tree", depth=3, fanout=10, files=300),
)

RULE = DecisionRule(margin=0.05, cliff_threshold=0.33, alpha=0.05)


def plan(name: str, remmy: Path) -> Plan:
    """The named plan: ``demo`` (minutes, warm only) or ``full`` (hours)."""
    tools = perf_tools(remmy)
    if name == "demo":
        return Plan(
            name="demo",
            fixtures=(FIXTURES[0],),
            caches={"F0": (Cache.WARM,)},
            tools=tuple(t for t in tools if t.name in ("rm", "xargs", "remmy")),
            threads=(1, 4),
            sampling={Cache.WARM: Sampling(reps=6, rounds=2, warmup=1)},
            reference=REFERENCE,
            rule=RULE,
            bootstrap_resamples=2000,
            seed=SEED,
        )
    if name == "full":
        return Plan(
            name="full",
            fixtures=FIXTURES,
            caches={f.id: (Cache.WARM, Cache.COLD) if f.id == "F0" else (Cache.WARM,) for f in FIXTURES},
            tools=tools,
            threads=(1, 2, 4, 8),
            sampling={
                Cache.WARM: Sampling(reps=20, rounds=4, warmup=3),
                Cache.COLD: Sampling(reps=16, rounds=4, warmup=2),
            },
            reference=REFERENCE,
            rule=RULE,
            bootstrap_resamples=10_000,
            seed=SEED,
        )
    raise ValueError(f"unknown plan {name!r}; expected demo or full")


PLANS = ("demo", "full")
