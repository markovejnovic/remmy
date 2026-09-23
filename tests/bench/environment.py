"""Refuse to time on a machine whose state would distort the numbers (macOS)."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

SPOTLIGHT_CPU_PCT = 10.0
BACKUP_CPU_PCT = 10.0
XPROTECT_CPU_PCT = 10.0
"""XProtect scans processes that delete many files, which inflates tools that
spawn many deleting processes (xargs) far more than single-process ones."""


def _out(*argv: str) -> str:
    try:
        return subprocess.run(argv, capture_output=True, text=True, timeout=10, check=False).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def unfit(scratch: Path) -> str | None:
    """Why this machine should not be timed on right now, or None."""
    if "Battery Power" in _out("/usr/bin/pmset", "-g", "batt"):
        return "running on battery"
    if re.search(r"lowpowermode\s+1", _out("/usr/bin/pmset", "-g")):
        return "low power mode is on"
    limit = re.search(r"CPU_Speed_Limit\s*=\s*(\d+)", _out("/usr/bin/pmset", "-g", "therm"))
    if limit and limit.group(1) != "100":
        return f"CPU is thermally limited to {limit.group(1)}%"
    if "com.apple" in _out("/usr/bin/tmutil", "listlocalsnapshots", str(scratch)):
        return "local Time Machine snapshots exist on the scratch volume"
    spotlight = backup = xprotect = 0.0
    for line in _out("/bin/ps", "-A", "-o", "%cpu=,comm=").splitlines():
        cpu, _, command = line.strip().partition(" ")
        name = Path(command.strip()).name
        if name.startswith(("mds", "mdworker")):
            spotlight += float(cpu)
        elif name == "backupd":
            backup += float(cpu)
        elif name.startswith("XProtect") or name == "xprotectd":
            xprotect += float(cpu)
    if spotlight > SPOTLIGHT_CPU_PCT:
        return f"Spotlight is busy ({spotlight:.0f}% CPU)"
    if backup > BACKUP_CPU_PCT:
        return f"Time Machine is busy ({backup:.0f}% CPU)"
    if xprotect > XPROTECT_CPU_PCT:
        return f"XProtect is busy ({xprotect:.0f}% CPU)"
    return None
