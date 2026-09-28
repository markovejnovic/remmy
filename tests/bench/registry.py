"""What the benchmark compares: tools, trees, sampling and the decision rule."""

import os
import re
import shutil
import sys
from collections.abc import Mapping
from pathlib import Path

from schema import Cache, DecisionRule, Fixture, Plan, Sampling, Tool

SEED = 20260914
PLANS = ("demo", "full")


def _brew(name: str) -> str:
    """Absolute path of a Homebrew tool, never a shell alias or shim."""
    for prefix in ("/opt/homebrew/bin", "/usr/local/bin"):
        if os.access(candidate := f"{prefix}/{name}", os.X_OK):
            return candidate
    return shutil.which(name) or f"/opt/homebrew/bin/{name}"


def perf_tools(remmy: Path, platform: str = sys.platform) -> tuple[Tool, ...]:
    """The timed competitors. Every one unlinks; none moves to a trash."""
    return _linux_tools(remmy) if platform == "linux" else _macos_tools(remmy)


def _macos_tools(remmy: Path) -> tuple[Tool, ...]:
    grm, bfs, r = _brew("grm"), _brew("bfs"), str(remmy)
    return (
        # BSD rm (fts): the baseline every ratio is taken against.
        Tool(name="rm", argv=("/bin/rm", "-rf", "--", "{tree}"), requires=("/bin/rm",)),
        # GNU coreutils rm; a separate baseline, never merged with BSD rm.
        Tool(name="grm", argv=(grm, "-rf", "--", "{tree}"), requires=(grm,)),
        # BSD find, depth-first. Absolute path: a shell's `find` may be a bfs alias.
        Tool(name="find", argv=("/usr/bin/find", "{tree}", "-depth", "-delete"), requires=("/usr/bin/find",)),
        # bfs, breadth-first: different directory locality than depth-first.
        Tool(name="bfs", argv=(bfs, "{tree}", "-delete"), requires=(bfs,)),
        # The strongest shell parallel deleter: parallel unlinks of every non-directory, then a serial directory
        # sweep. Runs via /bin/sh, so a ~1 ms shell fork is timed. (`-type f` would leave symlinks and the tree.)
        Tool(
            name="xargs",
            argv=(
                (
                    "/usr/bin/find {tree} -depth ! -type d -print0 | /usr/bin/xargs -0 -P{threads} -n256 /bin/rm -f"
                    " ; /usr/bin/find {tree} -depth -type d -delete"
                ),
            ),
            shell=True,
            requires=("/usr/bin/find", "/usr/bin/xargs", "/bin/rm"),
        ),
        # The subject. Its default thread count is 4 (the sweep includes it).
        Tool(name="remmy", argv=(r, "-r", "{tree}"), env={"REMMY_THREADS": "{threads}"}, requires=(r,)),
    )


def _linux_tools(remmy: Path) -> tuple[Tool, ...]:
    """The macOS line-up on a Linux box: rm and find are GNU here, so there is no separate grm."""
    bfs, r = shutil.which("bfs") or "/usr/bin/bfs", str(remmy)
    return (
        # GNU coreutils rm: the baseline every ratio is taken against.
        Tool(name="rm", argv=("/usr/bin/rm", "-rf", "--", "{tree}"), requires=("/usr/bin/rm",)),
        # GNU find, depth-first.
        Tool(name="find", argv=("/usr/bin/find", "{tree}", "-depth", "-delete"), requires=("/usr/bin/find",)),
        Tool(name="bfs", argv=(bfs, "{tree}", "-delete"), requires=(bfs,)),
        Tool(
            name="xargs",
            argv=(
                (
                    "/usr/bin/find {tree} -depth ! -type d -print0 | /usr/bin/xargs -0 -P{threads} -n256 /usr/bin/rm -f"
                    " ; /usr/bin/find {tree} -depth -type d -delete"
                ),
            ),
            shell=True,
            requires=("/usr/bin/find", "/usr/bin/xargs", "/usr/bin/rm"),
        ),
        Tool(name="remmy", argv=(r, "-r", "{tree}"), env={"REMMY_THREADS": "{threads}"}, requires=(r,)),
    )


# Each id names the tree's shape and file count, and is how results are labelled everywhere: pytest ids, chart
# files, and Bencher (demo/balanced-58k/warm/remmy@4). test_fixture_ids_match_their_trees keeps the counts honest.
FIXTURES = (
    # 585 dirs (8 subdirs per dir, 3 levels) x 100 empty files: the anchor every other tree varies one thing of.
    Fixture(id="balanced-58k", title="Anchor", depth=3, fanout=8, files=100),
    # 512 sibling dirs x 100 files: many small scan tasks at one level.
    Fixture(id="wide-51k", title="Shallow and wide", depth=1, fanout=512, files=100),
    # A 9-level binary tree, 1,023 dirs x 57 files: long dependency chains.
    Fixture(id="deep-58k", title="Deep and narrow", depth=9, fanout=2, files=57),
    # The anchor with 4 KiB files: freeing data blocks, not just inodes.
    Fixture(id="balanced-58k-4kib", title="Small files (4 KiB)", depth=3, fanout=8, files=100, size=4096),
    # 43 dirs x 45 files of 1 MiB (1.9 GiB): unlink cost dominated by freeing extents.
    Fixture(id="large-1mib-files", title="Large files (1 MiB)", depth=2, fanout=6, files=45, size=1 << 20),
    # 5 dirs x 25 files: startup cost.
    Fixture(id="tiny-125", title="Tiny tree", depth=1, fanout=4, files=25),
    # 1,111 dirs (10 per dir, 3 levels) x 300 files.
    Fixture(id="balanced-333k", title="Huge tree", depth=3, fanout=10, files=300),
    # The anchor's file count in one directory: a single scan task, no subdirs to spread.
    Fixture(id="flat-58k", title="Flat directory", depth=0, fanout=0, files=58_500),
)
ANCHOR = FIXTURES[0].id


def baseline_tool(name: str, remmy: Path) -> Tool:
    """Another build of remmy, timed exactly like the subject."""
    r = str(remmy)
    return Tool(name=name, argv=(r, "-r", "{tree}"), env={"REMMY_THREADS": "{threads}"}, requires=(r,))


def plan(name: str, remmy: Path, platform: str = sys.platform, baselines: Mapping[str, Path] | None = None) -> Plan:
    """``demo`` (minutes, warm only) or ``full`` (hours). ``baselines`` adds other remmy builds (name -> executable),
    each compared with remmy at every thread count."""
    baselines = dict(baselines or {})
    tools = perf_tools(remmy, platform)
    taken = {t.name for t in tools}
    for b in baselines:
        if not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", b) or b in taken:
            raise ValueError(f"bad baseline name {b!r}: lowercase, digits, . and -; not a tool's name")
    extra = tuple(baseline_tool(b, p) for b, p in baselines.items())
    common = {"reference": "rm", "rule": DecisionRule(), "seed": SEED, "baselines": tuple(baselines)}
    if name == "demo":
        return Plan(
            name="demo",
            fixtures=FIXTURES[:1],
            caches={ANCHOR: (Cache.WARM,)},
            tools=tuple(t for t in tools if t.name in ("rm", "xargs", "remmy")) + extra,
            threads=(1, 4),
            # 12 samples in 4 shuffled rounds: a PR is judged against main from this one run, so each cell's CI
            # has to be tight (6 samples gave +-1.3% on remmy@4 while runs differed by only 0.3%).
            sampling={Cache.WARM: Sampling(reps=12, rounds=4, warmup=1)},
            bootstrap_resamples=2000,
            **common,
        )
    if name == "full":
        return Plan(
            name="full",
            fixtures=FIXTURES,
            caches={f.id: (Cache.WARM, Cache.COLD) if f.id == ANCHOR else (Cache.WARM,) for f in FIXTURES},
            tools=tools + extra,
            threads=(1, 2, 4, 8),
            sampling={
                Cache.WARM: Sampling(reps=20, rounds=4, warmup=3),
                Cache.COLD: Sampling(reps=16, rounds=4, warmup=2),
            },
            bootstrap_resamples=10_000,
            **common,
        )
    raise ValueError(f"unknown plan {name!r}; expected demo or full")
