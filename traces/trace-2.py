#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["click", "macholib"]
# ///
"""Which kernel locks does a command contend on, and for how much of its time?

Runs the command under DTrace's lockstat provider, which fires whenever a
read/write lock or mutex acquisition has to spin or sleep, with the lock's
address and how long it waited. Everything is summed in the kernel, so it
doesn't drop events however many threads contend. Per lock it reports
acquisitions, contended acquisitions, time spent spinning and blocked, that
time as a share of the command's worker-thread time, and the call stacks that
waited on it. Shares add up across locks: a thread waits on one lock at a time.

Run as yourself; it asks sudo for DTrace and kas_info. Shares profile.py's
kernel symbolication, so it needs the same KDK.
"""

import collections
import ctypes
import importlib.util
import os
import re

import click

_spec = importlib.util.spec_from_file_location("profile_py", os.path.join(os.path.dirname(__file__), "profile.py"))
assert _spec and _spec.loader
profile = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(profile)

# lockstat probes are named provider:function:name, e.g.
# lockstat::lck_rw_lock_shared:rw-block (arg0 lock, arg1 wait in mach ticks,
# arg4 readers holding the lock when the wait started).
# Waits are mach ticks; anything negative or over a minute is a probe that fired
# without its start time recorded (seen on a few rw-spins), so it's counted, not summed.
MAX_WAIT_TICKS = 24_000_000 * 60
D_SCRIPT = f"""
BEGIN {{ start = timestamp; }}

lockstat:::rw-spin, lockstat:::rw-block, lockstat:::adaptive-spin, lockstat:::adaptive-block
/pid == $target && (int64_t)arg1 >= 0 && arg1 < {MAX_WAIT_TICKS}/
{{
	@wait[arg0, probefunc, probename] = sum(arg1);
	@waits[arg0, probefunc, probename] = count();
	@who[arg0, probefunc, probename, stack(32)] = sum(arg1);
}}

lockstat:::rw-spin, lockstat:::rw-block, lockstat:::adaptive-spin, lockstat:::adaptive-block
/pid == $target && ((int64_t)arg1 < 0 || arg1 >= {MAX_WAIT_TICKS})/
{{ @bogus = count(); }}

lockstat::lck_rw_lock_shared:rw-block
/pid == $target && arg4 > 0/
{{ @readers_inside[arg0] = count(); }}

sched:::on-cpu /pid == $target/ {{ @first[tid] = min(timestamp); }}
sched:::off-cpu /pid == $target/ {{ @last[tid] = max(timestamp); }}

END {{
	printf("@@WALL %d\\n", timestamp - start);
	printa("@@BOGUS %@d\\n", @bogus);
	printa("@@WAIT %p %s %s %@d %@d\\n", @wait, @waits);
	printa("@@RDR %p %@d\\n", @readers_inside);
	printa("@@THR %d %@d %@d\\n", @first, @last);
	printa("@@WHO %p %s %s %@d%k\\n@@END\\n", @who);
}}
"""
# Counting every acquisition makes the contention rate possible, but the probe fires
# inside the critical section of every lock taken, slowing the run ~10x and inflating
# contention. Opt-in only.
ACQUIRES = """
lockstat:::rw-acquire, lockstat:::adaptive-acquire
/pid == $target/
{ @acq[arg0, probefunc] = count(); }
END { printa("@@ACQ %p %s %@d\\n", @acq); }
"""
OPTIONS = ["stackframes=32", "aggsize=64m", "bufsize=16m"]

MODE = {"lck_rw_lock_shared": "read", "lck_rw_lock_exclusive": "write",
        "lck_rw_lock_shared_to_exclusive": "upgrade", "lck_mtx_lock": "mutex", "lck_mtx_lock_spin": "mutex"}
# Frames that are the locking machinery itself rather than who asked for the lock.
LOCK_FRAME = re.compile(r"^(lck_|IORWLock|IOLock|hw_lck|lockstat|dtrace|DYLD-STUB)")


def ticks_to_ns():
    class Timebase(ctypes.Structure):
        _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]
    tb = Timebase()
    ctypes.CDLL(None).mach_timebase_info(ctypes.byref(tb))
    return tb.numer / tb.denom


def func(frame):
    """`mod`func+0x10` -> func"""
    return re.sub(r"\+0x[0-9a-f]+$", "", frame.strip().rpartition("`")[2])


# The system call a wait happened under, from its outermost recognisable frame.
# remmy walks with openat + getdirentries64 and deletes with unlinkat (+ rmdir).
OPS = [("readdir (walk)", re.compile(r"^(getdirentries64|getdirentries_common|getattrlistbulk)$")),
       ("open (walk)", re.compile(r"^(openat_internal|open1|openat|vn_open_auth)$")),
       ("stat (walk)", re.compile(r"^(fstatat_internal|fstatat|stat64|lstat64|fstat)$")),
       ("unlink (delete)", re.compile(r"^(unlinkat_internal|unlinkat|unlink1)$")),
       ("rmdir (delete)", re.compile(r"^(rmdirat_internal|rmdir|rmdirat)$")),
       ("close", re.compile(r"^(close_nocancel|closef|close)$"))]


def operation(frames):
    for f in reversed(frames):  # outermost first
        for op, pattern in OPS:
            if pattern.match(f):
                return op
    return "other"


def caller(stack):
    """The first three frames above the locking machinery, innermost first."""
    frames = [func(f) for f in stack if f.strip()]
    return " < ".join([f for f in frames if not LOCK_FRAME.match(f)][:3]) or "?"


def report(raw, top):
    ns = ticks_to_ns()
    lines = raw.splitlines()
    wall = next(int(l.split()[1]) for l in lines if l.startswith("@@WALL"))

    acq = collections.defaultdict(collections.Counter)                       # lock -> mode -> acquisitions
    wait = collections.defaultdict(lambda: collections.defaultdict(float))   # lock -> (mode, spin|block) -> ns
    waits = collections.defaultdict(collections.Counter)                     # lock -> (mode, spin|block) -> count
    readers_inside, life, bogus = collections.Counter(), {}, 0
    for l in lines:
        f = l.split()
        if l.startswith("@@ACQ "):
            acq[int(f[1], 16)][MODE.get(f[2], f[2])] += int(f[3])
        elif l.startswith("@@BOGUS "):
            bogus += int(f[1])
        elif l.startswith("@@WAIT "):
            if not 0 <= int(f[4]) < MAX_WAIT_TICKS * int(f[5]):
                bogus += int(f[5])
                continue
            key = (MODE.get(f[2], f[2]), f[3].split("-")[1])
            wait[int(f[1], 16)][key] += int(f[4]) * ns
            waits[int(f[1], 16)][key] += int(f[5])
        elif l.startswith("@@RDR "):
            readers_inside[int(f[1], 16)] += int(f[2])
        elif l.startswith("@@THR "):
            if int(f[2]) and int(f[3]):
                life[int(f[1])] = (int(f[2]), int(f[3]))

    callers = collections.defaultdict(collections.Counter)                   # lock -> caller -> ns
    by_op = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))  # lock -> op -> mode/kind -> ns
    for block in raw.split("@@WHO ")[1:]:
        head, _, stack = block.partition("\n")
        f = head.split()
        # Older recordings keyed stacks by lock only: "lock ticks".
        lock, mode, ticks = (f[0], f"{MODE.get(f[1], f[1])} {f[2].split('-')[1]}", f[3]) if len(f) >= 4 else (f[0], "?", f[1])
        if int(ticks) < 0:
            continue
        frames = stack.split("@@END")[0].splitlines()
        callers[int(lock, 16)][caller(frames)] += int(ticks) * ns
        by_op[int(lock, 16)][operation([func(x) for x in frames if x.strip()])][mode] += int(ticks) * ns

    # The main thread is the first one to run; in remmy it only waits for the workers.
    main_tid = min(life, key=lambda t: life[t][0]) if life else None
    worker_ns = sum(e - s for t, (s, e) in life.items() if t != main_tid) or 1
    total = {lock: sum(w.values()) for lock, w in wait.items()}
    ms = lambda v: f"{v / 1e6:9.1f}"

    out = [f"wall {wall / 1e6:.1f} ms, {len(life)} threads, worker thread time {worker_ns / 1e6:.1f} ms "
           f"(excluding main thread {main_tid})",
           f"{len(wait)} locks contended, waiting {sum(total.values()) / 1e6:.1f} ms in total = "
           f"{100 * sum(total.values()) / worker_ns:.1f}% of worker thread time"
           + (f" ({bogus} impossible wait records ignored)" if bogus else ""), ""]
    out.append(f"{'lock':>18}  {'kind':5} {'acquires':>9} {'contended':>9} {'rate':>6}  "
               f"{'spin ms':>9} {'block ms':>9} {'total ms':>9} {'share':>6}  {'rdr-blk*':>8}  top caller")
    for lock in sorted(total, key=lambda k: total[k], reverse=True)[:top]:
        w, n = wait[lock], waits[lock]
        kinds = {m for m, _ in w} | set(acq[lock])
        kind = "mutex" if kinds == {"mutex"} else "rw"
        spin = sum(v for (_, k), v in w.items() if k == "spin")
        block = sum(v for (_, k), v in w.items() if k == "block")
        a, c = sum(acq[lock].values()), sum(n.values())
        top_caller = callers[lock].most_common(1)[0][0] if callers[lock] else "?"
        rate = f"{100 * c / a:5.1f}%" if a else "    - "
        acqs = f"{a:9d}" if a else "        -"
        out.append(f"{lock:#18x}  {kind:5} {acqs} {c:9d} {rate}  "
                   f"{ms(spin)} {ms(block)} {ms(total[lock])} {100 * total[lock] / worker_ns:5.1f}%  "
                   f"{readers_inside[lock]:8d}  {top_caller}")
    out += ["", "* rdr-blk: reader sleeps that started while other readers held the lock, i.e. readers held",
            "  back by a waiting writer (writer-priority locks)", ""]

    for lock in sorted(total, key=lambda k: total[k], reverse=True)[:min(top, 5)]:
        w = wait[lock]
        split = ", ".join(f"{m} {k} {v / 1e6:.1f} ms ({waits[lock][(m, k)]})" for (m, k), v in
                          sorted(w.items(), key=lambda kv: -kv[1]))
        out.append(f"{lock:#x}: {split}")
        modes = sorted({m for c in by_op[lock].values() for m in c})
        out.append(f"    {'by operation':16} " + " ".join(f"{m:>12}" for m in modes) + f" {'total':>9}")
        for op, c in sorted(by_op[lock].items(), key=lambda kv: -sum(kv[1].values())):
            out.append(f"    {op:16} " + " ".join(f"{c[m] / 1e6:12.1f}" for m in modes) + f" {sum(c.values()) / 1e6:9.1f}")
        out.append("    top callers:")
        for who, v in callers[lock].most_common(4):
            out.append(f"    {v / 1e6:9.1f} ms  {who}")
        out.append("")
    return "\n".join(out)


@click.command(context_settings={"ignore_unknown_options": True})
@click.option("-o", "--out", default="locks", show_default=True, help="Output name.")
@click.option("-e", "--env", "env", multiple=True, metavar="KEY=VALUE",
              help="Set an environment variable for COMMAND (repeatable), e.g. -e REMMY_THREADS=16.")
@click.option("--count-acquires", is_flag=True,
              help="Also count every acquisition, for a contention rate. Slows the run ~10x and inflates contention.")
@click.option("--top", default=15, show_default=True, help="How many locks to list.")
@click.option("--from", "from_", type=click.Path(exists=True, dir_okay=False),
              help="Re-analyse an earlier OUT.lockstat.txt instead of recording.")
@click.argument("command", nargs=-1, type=click.UNPROCESSED)
def main(out, env, count_acquires, top, from_, command):
    """Run COMMAND and write OUT.lockstat.txt (raw) and OUT.locks.txt (report)."""
    if from_:
        with open(from_) as fh:
            raw = fh.read()
    elif command:
        bad = [e for e in env if not re.fullmatch(r"[A-Za-z_]\w*=.*", e)]
        if bad:
            raise click.BadParameter(f"expected KEY=VALUE, got {bad[0]!r}", param_hint="-e")
        raw = profile.record(command, env, f"{out}.lockstat.txt", d_script=D_SCRIPT + (ACQUIRES if count_acquires else ""), options=OPTIONS)
    else:
        raise click.UsageError("give a COMMAND to trace, or --from an earlier OUT.lockstat.txt")

    text = report(raw, top)
    with open(f"{out}.locks.txt", "w") as fh:
        fh.write(text + "\n")
    click.echo(text)


if __name__ == "__main__":
    main()
