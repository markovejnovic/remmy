"""hyperfine ``--prepare`` hook: build a fresh tree before every timed run.

    prepare.py MKTREE TREE STATE [--purge] -- MKTREE_ARGS...

Builds TREE with mktree, flushes it with ``sync``, then waits until the
volume's free space stops moving so the build's writeback cannot bleed into
the timed delete. Writes the built tree's counts to STATE. With ``--purge``,
finally evicts the filesystem cache (``sudo -n purge``; root must already be
authorized).

Runs outside the timer. Any failure exits non-zero, which aborts hyperfine:
a run is never timed on a tree that was not built exactly as specified.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

SETTLE_POLLS = 50
SETTLE_INTERVAL = 0.02


def free_bytes(path: Path) -> int:
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize


def settle(volume: Path) -> int:
    """Poll free space until two consecutive readings agree; return it."""
    previous = None
    for _ in range(SETTLE_POLLS):
        current = free_bytes(volume)
        if current == previous:
            return current
        previous = current
        time.sleep(SETTLE_INTERVAL)
    return free_bytes(volume)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mktree", type=Path)
    parser.add_argument("tree", type=Path)
    parser.add_argument("state", type=Path)
    parser.add_argument("--purge", action="store_true")
    parser.add_argument("mktree_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    mktree_args = [a for a in args.mktree_args if a != "--"]

    if os.path.lexists(args.tree):
        # A previous run's tool left something: never build on top of it.
        shutil.rmtree(args.tree, ignore_errors=True)
        if os.path.lexists(args.tree):
            print(f"prepare: stale tree {args.tree} cannot be cleared", file=sys.stderr)
            return 1

    built = subprocess.run(
        [str(args.mktree), str(args.tree), *mktree_args], capture_output=True, text=True, check=False
    )
    if built.returncode != 0:
        print(f"prepare: mktree failed: {built.stderr.strip()}", file=sys.stderr)
        return 1
    dirs, files, _secs = built.stdout.strip().split(",")

    subprocess.run(["/bin/sync"], check=True)
    settle(args.tree.parent)
    args.state.write_text(json.dumps({"dirs": int(dirs), "files": int(files)}))

    if args.purge:
        purged = subprocess.run(["sudo", "-n", "/usr/sbin/purge"], capture_output=True, text=True, check=False)
        if purged.returncode != 0:
            print(f"prepare: purge failed (is sudo authorized?): {purged.stderr.strip()}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
