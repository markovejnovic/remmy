"""Refuse to time on a machine whose state would distort the numbers (macOS, and the Linux bench box)."""

import os
import re
import subprocess
import sys
import time
from pathlib import Path

BUSY_CPU_PCT = 10.0


def _out(*argv: str) -> str:
    try:
        return subprocess.run(argv, capture_output=True, text=True, timeout=10, check=False).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def unfit(scratch: Path) -> str | None:
    """Why this machine should not be timed on right now, or None."""
    return _unfit_linux() if sys.platform == "linux" else _unfit_macos(scratch)


def _cpu_times(cpus: set[int]) -> tuple[int, int]:
    """(busy, total) jiffies summed over `cpus`, from /proc/stat."""
    busy = total = 0
    for line in Path("/proc/stat").read_text().splitlines():
        name, *fields = line.split()
        if not name.startswith("cpu") or name == "cpu" or int(name[3:]) not in cpus:
            continue
        ticks = [int(f) for f in fields]
        total += sum(ticks[:8])  # guest time is already counted in user
        busy += sum(ticks[:8]) - ticks[3] - ticks[4]  # minus idle and iowait
    return busy, total


def _unfit_linux() -> str | None:
    cpus = os.sched_getaffinity(0)
    governors = {
        g.read_text().strip()
        for cpu in cpus
        if (g := Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor")).exists()
    }
    if governors - {"performance"}:
        return f"CPU frequency governor is {', '.join(sorted(governors))}, not performance"
    meminfo = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
    if int(meminfo["SwapTotal"].split()[0]) - int(meminfo["SwapFree"].split()[0]) > 0:
        return "swap is in use"
    # Only the CPUs this process may run on: the bench box keeps its housekeeping cores out of our affinity.
    busy0, total0 = _cpu_times(cpus)
    time.sleep(1)
    busy1, total1 = _cpu_times(cpus)
    pct = 100 * (busy1 - busy0) / max(total1 - total0, 1)
    if pct > BUSY_CPU_PCT:
        return f"the benchmark CPUs are busy ({pct:.0f}% over 1 s)"
    return None


def _unfit_macos(scratch: Path) -> str | None:
    if "Battery Power" in _out("/usr/bin/pmset", "-g", "batt"):
        return "running on battery"
    if re.search(r"lowpowermode\s+1", _out("/usr/bin/pmset", "-g")):
        return "low power mode is on"
    limit = re.search(r"CPU_Speed_Limit\s*=\s*(\d+)", _out("/usr/bin/pmset", "-g", "therm"))
    if limit and limit.group(1) != "100":
        return f"CPU is thermally limited to {limit.group(1)}%"
    if "com.apple" in _out("/usr/bin/tmutil", "listlocalsnapshots", str(scratch)):
        return "local Time Machine snapshots exist on the scratch volume"
    # XProtect scans processes that delete many files, which inflates tools that spawn many deleting processes
    # (xargs) far more than single-process ones.
    busy = {"Spotlight": 0.0, "Time Machine": 0.0, "XProtect": 0.0}
    for line in _out("/bin/ps", "-A", "-o", "%cpu=,comm=").splitlines():
        cpu, _, command = line.strip().partition(" ")
        name = Path(command.strip()).name
        if name.startswith(("mds", "mdworker")):
            busy["Spotlight"] += float(cpu)
        elif name == "backupd":
            busy["Time Machine"] += float(cpu)
        elif name.startswith("XProtect") or name == "xprotectd":
            busy["XProtect"] += float(cpu)
    for what, pct in busy.items():
        if pct > BUSY_CPU_PCT:
            return f"{what} is busy ({pct:.0f}% CPU)"
    return None
