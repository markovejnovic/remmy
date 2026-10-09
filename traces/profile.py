#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["click", "macholib"]
# ///
"""Profile a command like Instruments' Time Profiler, as a speedscope profile.

Samples the command's on-CPU threads at ~1 kHz with DTrace and, like
"Record Waiting Threads", times every stretch a thread spends blocked, both
with kernel and user stacks. Each event is logged with its thread and
timestamp, so the speedscope file has one timeline per thread (samples in
time order, weighted in microseconds) plus an "all threads" profile rooted at
the thread. Each stack ends in an [on-cpu] or [off-cpu] frame; keeping it at
the tip lets speedscope merge the shared trunk across consecutive samples.

DTrace leaves most kernel frames as raw addresses: on Apple silicon the
kernel's code (__TEXT_EXEC) is slid separately from its header, so the
runtime __TEXT_EXEC address is read straight from the kernel with kas_info(2)
and handed to `atos -textExecAddress` along with the matching Kernel Debug Kit.

Run as yourself; it asks sudo for DTrace and kas_info. Needs uv and the KDK
for the running kernel.
"""

import collections
import glob
import json
import os
import re
import shlex
import subprocess
import tempfile

import click
from macholib.MachO import MachO
from macholib.mach_o import LC_SEGMENT_64, LC_UUID

# On-CPU: a sample of the target every ~1 ms. 997 Hz rather than 1000 so
# sampling doesn't run in lockstep with timers.
# Off-CPU: when a thread gets back on a CPU, how long it was off, with the
# stack it blocked in (on-cpu fires in the returning thread's context).
# Events stream out per CPU, not in time order; the timestamps sort them.
HZ = 997
D_SCRIPT = f"""
profile-{HZ} /pid == $target/ {{
	printf("\\n@@EV on %d %d 0\\n", tid, timestamp); stack(); printf("\\n##\\n"); ustack();
}}
sched:::off-cpu /pid == $target/ {{ self->ts = timestamp; }}
sched:::on-cpu /self->ts/ {{
	printf("\\n@@EV off %d %d %d\\n", tid, self->ts, timestamp - self->ts); stack(); printf("\\n##\\n"); ustack();
	self->ts = 0;
}}
"""

# kas_info is root-only, so it runs in its own sudo'd interpreter. It returns
# the running kernel's slid vmaddr for each load command, in order.
KAS_INFO = """
import ctypes, os, sys
libc = ctypes.CDLL(None, use_errno=True)
size = ctypes.c_size_t(0)
if libc.kas_info(1, None, ctypes.byref(size)) == 0:
    bases = (ctypes.c_uint64 * (size.value // 8))()
    if libc.kas_info(1, bases, ctypes.byref(size)) == 0:
        print(*bases)
        sys.exit()
sys.exit("kas_info: " + os.strerror(ctypes.get_errno()))
"""

RAW_KERNEL_FRAME = re.compile(r"(?:kernel\.release\.\w+`)?(0xfffffe[0-9a-f]+)(?:\+(0x[0-9a-f]+))?")
Event = collections.namedtuple("Event", "kind tid start us frames")  # frames root first


def arm64e_header(path):
    return next(h for h in MachO(path).headers if h.header.cpusubtype & 0xFF == 2)  # CPU_SUBTYPE_ARM64E


def uuid(path):
    return next(cmd.uuid for lc, cmd, _ in arm64e_header(path).commands if lc.cmd == LC_UUID)


def text_exec_index(path):
    for i, (lc, cmd, _) in enumerate(arm64e_header(path).commands):
        if lc.cmd == LC_SEGMENT_64 and cmd.segname.rstrip(b"\0") == b"__TEXT_EXEC":
            return i
    raise click.ClickException(f"no __TEXT_EXEC segment in {path}")


def find_dsym(kernel):
    name, want = os.path.basename(kernel), uuid(kernel)
    for dwarf in glob.glob(f"/Library/Developer/KDKs/*.kdk/System/Library/Kernels/{name}.dSYM/Contents/Resources/DWARF/{name}"):
        if uuid(dwarf) == want:
            return dwarf
    raise click.ClickException(f"no KDK for {name}: install the Kernel Debug Kit for this macOS build")


def atos(dwarf, text_exec, addrs):
    """{address: function name} for the addresses atos can name."""
    addrs = sorted(addrs)
    out = subprocess.run(["atos", "-o", dwarf, "-arch", "arm64e", "-textExecAddress", hex(text_exec), *map(hex, addrs)],
                         check=True, capture_output=True, text=True).stdout
    names = (line.split(" (in ")[0].strip() for line in out.splitlines())
    return {a: n for a, n in zip(addrs, names) if n and not n.startswith("0x")}


def parse(raw):
    """Events from the D script's output. Stacks print leaf first, kernel then user."""
    for block in raw.split("\n@@EV ")[1:]:
        head, _, body = block.partition("\n")
        kind, tid, ts, dur = head.split()
        kstack, _, ustack = body.partition("\n##\n")
        leaf_first = [f.strip() for f in kstack.splitlines() + ustack.splitlines() if f.strip()]
        us = 1e6 / HZ if kind == "on" else int(dur) / 1e3
        yield Event(kind, int(tid), int(ts), us, leaf_first[::-1] + [f"[{kind}-cpu]"])


def speedscope(name, events, frame_index):
    """A sampled speedscope profile; samples keep the events' order."""
    weights = [round(e.us) for e in events]
    return {"type": "sampled", "name": name, "unit": "microseconds", "startValue": 0, "endValue": sum(weights),
            "samples": [[frame_index(f) for f in e.frames] for e in events], "weights": weights}


# Every block/wake allocates and frees self->ts; under contention that churns
# faster than the cleaner reclaims (macOS caps it at 50 Hz), and without a big
# dynvarsize DTrace drops off-CPU intervals. Big per-CPU buffers drained often
# keep the event stream from dropping too.
STREAM_OPTIONS = ["stackframes=100", "ustackframes=100", "dynvarsize=64m", "cleanrate=50hz",
                  "bufsize=32m", "switchrate=100hz"]


def record(command, env, path, d_script=D_SCRIPT, options=STREAM_OPTIONS):
    """Runs COMMAND under D_SCRIPT, with ENV (KEY=VALUE strings) added to its environment; returns the
    output with kernel frames named, also saved to PATH."""
    soc = re.search(r"RELEASE_ARM64_(T\d+)", os.uname().version)
    if not soc:
        raise click.ClickException("not an Apple silicon release kernel")
    kernel = f"/System/Library/Kernels/kernel.release.{soc.group(1).lower()}"
    dwarf = find_dsym(kernel)

    bases = subprocess.run(["sudo", "/usr/bin/python3", "-c", KAS_INFO], check=True, capture_output=True, text=True)
    text_exec = int(bases.stdout.split()[text_exec_index(kernel)])

    # DTrace writes into a temp dir we own, so its root-owned file can be removed.
    with tempfile.TemporaryDirectory() as tmp:
        raw_path = os.path.join(tmp, "out")
        # sudo resets the environment, so ENV goes through env(1) to DTrace, whose -c child inherits it.
        xopts = [a for o in options for a in ("-x", o)]
        subprocess.run(["sudo", "env", *env, "dtrace", "-q", *xopts,
                        "-n", d_script, "-o", raw_path, "-c", shlex.join(command)], check=True)
        with open(raw_path) as fh:
            raw = fh.read()

    unnamed = {int(m[1], 16) + int(m[2] or "0", 16) for m in RAW_KERNEL_FRAME.finditer(raw)}
    names = atos(dwarf, text_exec, unnamed) if unnamed else {}
    click.echo(f"named {len(names)} of {len(unnamed)} raw kernel addresses (__TEXT_EXEC at {hex(text_exec)})")

    def name(m):
        addr = int(m[1], 16) + int(m[2] or "0", 16)
        return f"{os.path.basename(kernel)}`{names[addr]}" if addr in names else m[0]

    raw = RAW_KERNEL_FRAME.sub(name, raw)
    with open(path, "w") as fh:
        fh.write(raw)
    return raw


@click.command(context_settings={"ignore_unknown_options": True})
@click.option("-o", "--out", default="profile", show_default=True, help="Output name.")
@click.option("-e", "--env", "env", multiple=True, metavar="KEY=VALUE",
              help="Set an environment variable for COMMAND (repeatable), e.g. -e REMMY_THREADS=16.")
@click.option("--from", "from_", type=click.Path(exists=True, dir_okay=False),
              help="Rebuild the speedscope file from an earlier OUT.dtrace.txt instead of recording.")
@click.argument("command", nargs=-1, type=click.UNPROCESSED)
def main(out, env, from_, command):
    """Profile COMMAND and write OUT.dtrace.txt and OUT.speedscope.json.

    Open the result with `npx speedscope OUT.speedscope.json`; pick a thread
    from the profile menu at the top, and use Time Order to see it over time.
    """
    if from_:
        with open(from_) as fh:
            raw = fh.read()
    elif command:
        bad = [e for e in env if not re.fullmatch(r"[A-Za-z_]\w*=.*", e)]
        if bad:
            raise click.BadParameter(f"expected KEY=VALUE, got {bad[0]!r}", param_hint="-e")
        raw = record(command, env, f"{out}.dtrace.txt")
    else:
        raise click.UsageError("give a COMMAND to profile, or --from an earlier OUT.dtrace.txt")

    by_thread = collections.defaultdict(list)
    for e in sorted(parse(raw), key=lambda e: e.start):
        by_thread[e.tid].append(e)
    # The main thread is the one that started in dyld; the rest go by first appearance.
    thread_name = {tid: f"{'main thread' if any('dyld`start' in e.frames[0] for e in evs) else 'thread'} {tid}"
                   for tid, evs in by_thread.items()}

    # Frames are shared across profiles: "mod`func+0x10" becomes func, with mod as its file.
    frames, index = [], {}

    def frame_index(frame):
        mod, _, func = frame.rpartition("`")
        key = (mod, re.sub(r"\+0x[0-9a-f]+$", "", func))  # one frame per function, whatever the offset
        if key not in index:
            index[key] = len(frames)
            frames.append({"name": key[1], **({"file": mod} if mod else {})})
        return index[key]

    everything = [e._replace(frames=[thread_name[e.tid]] + e.frames) for evs in by_thread.values() for e in evs]
    profiles = [speedscope("all threads", everything, frame_index)]
    profiles += [speedscope(thread_name[tid], evs, frame_index) for tid, evs in by_thread.items()]
    with open(f"{out}.speedscope.json", "w") as fh:
        json.dump({"$schema": "https://www.speedscope.app/file-format-schema.json", "name": out,
                   "shared": {"frames": frames}, "profiles": profiles, "activeProfileIndex": 0,
                   "exporter": "profile.py"}, fh)

    for tid, evs in by_thread.items():
        on = sum(e.us for e in evs if e.kind == "on") / 1e3
        off = sum(e.us for e in evs if e.kind == "off") / 1e3
        click.echo(f"  {thread_name[tid]:>20}  on-cpu {on:8.1f} ms  off-cpu {off:8.1f} ms")
    click.echo(f"open: npx speedscope {out}.speedscope.json")


if __name__ == "__main__":
    main()
