"""hyperfine --prepare hook, run outside the timer: prepare.py MKTREE TREE STATE [--purge] -- MKTREE_ARGS...

Builds TREE, syncs, waits until the volume's free space stops moving (so the build's writeback can't bleed into the
timed delete), writes the tree's counts to STATE, and with --purge evicts the fs cache (`sudo -n purge`). Any failure
exits non-zero, aborting hyperfine; since hyperfine hides this hook's stderr, the reason also goes to prepare.err.
"""

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def free_bytes(path: Path) -> int:
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize


def settle(volume: Path) -> int:
    """Poll free space until two consecutive readings agree."""
    previous = None
    for _ in range(50):
        if (current := free_bytes(volume)) == previous:
            return current
        previous = current
        time.sleep(0.02)
    return free_bytes(volume)


def main() -> int:
    # Split at "--" by hand: argparse's REMAINDER would also swallow --purge.
    argv = sys.argv[1:]
    split = argv.index("--") if "--" in argv else len(argv)
    purge = "--purge" in argv[:split]
    mktree, tree, state = (Path(a) for a in argv[:split] if a != "--purge")
    err = state.with_name("prepare.err")

    def fail(message: str) -> int:
        print(f"prepare: {message}", file=sys.stderr)
        err.write_text(message + "\n")
        return 1

    if os.path.lexists(tree):
        # A previous run's tool left something: never build on top of it.
        shutil.rmtree(tree, ignore_errors=True)
        if os.path.lexists(tree):
            return fail(f"stale tree {tree} cannot be cleared")

    built = subprocess.run([str(mktree), str(tree), *argv[split + 1 :]], capture_output=True, text=True, check=False)
    if built.returncode != 0:
        return fail(f"mktree failed: {built.stderr.strip()}")
    dirs, files, _ = built.stdout.strip().split(",")

    subprocess.run(["/bin/sync"], check=True)
    settle(tree.parent)
    state.write_text(json.dumps({"dirs": int(dirs), "files": int(files)}))

    if purge:
        purged = subprocess.run(["sudo", "-n", "/usr/sbin/purge"], capture_output=True, text=True, check=False)
        if purged.returncode != 0:
            return fail(f"purge failed (is sudo authorized?): {purged.stderr.strip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
