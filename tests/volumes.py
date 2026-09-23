"""Scratch volumes of other filesystem types, backed by sparse disk images.

Nothing here needs root: ``hdiutil`` creates and mounts user-owned images.
Capabilities are probed on the mounted volume instead of assumed per type.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from collections.abc import Iterator
from contextlib import contextmanager, suppress
from dataclasses import dataclass
from pathlib import Path

from strategies import FsCaps

# hdiutil -fs names, as listed by `diskutil listFilesystems`.
FILESYSTEMS = (
    "APFS",
    "Case-sensitive APFS",
    "HFS+",
    "Case-sensitive HFS+",
    "ExFAT",
    "MS-DOS FAT32",
)


@dataclass(frozen=True)
class Volume:
    fs: str
    root: Path
    caps: FsCaps
    case_sensitive: bool
    permissions: bool


class VolumeUnavailable(Exception):
    pass


def _hdiutil(*args: str) -> str:
    if sys.platform != "darwin" or shutil.which("hdiutil") is None:
        raise VolumeUnavailable("hdiutil is macOS-only")
    proc = subprocess.run(["hdiutil", *args], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise VolumeUnavailable(f"hdiutil {args[0]} failed: {proc.stderr.strip()}")
    return proc.stdout


def _probe(root: Path) -> tuple[FsCaps, bool, bool]:
    p = root / ".probe"
    p.mkdir()
    try:
        symlinks = fifos = case_sensitive = permissions = True
        try:
            os.symlink("target", p / "l")
        except OSError:
            symlinks = False
        try:
            os.mkfifo(p / "f")
        except OSError:
            fifos = False
        (p / "a").write_text("")
        case_sensitive = not (p / "A").exists()
        os.chmod(p / "a", 0o400)
        permissions = (os.stat(p / "a").st_mode & 0o777) == 0o400
        dos = False
        try:
            (p / "x:y").write_text("")
        except OSError:
            dos = True
        nfd = not _round_trips(p / "names")
        caps = FsCaps(symlinks=symlinks, fifos=fifos, dos_names=dos, nfd_names=nfd)
        return caps, case_sensitive, permissions
    finally:
        shutil.rmtree(p, ignore_errors=True)


# Names that are not in NFD: a precomposed letter, and a character whose
# decomposition NFC never recombines (Unicode composition exclusion).
_NON_NFD_NAMES = ("\u00e9", "\u0a59", "\u2126")


def _round_trips(probe: Path) -> bool:
    """Whether every name the volume lists back can be unlinked by that name.

    macOS's exFAT and FAT drivers list some non-NFD names in a different form
    than they were created with and then fail to unlink the listed form.
    """
    probe.mkdir()
    for name in _NON_NFD_NAMES:
        (probe / name).write_text("")
    ok = True
    for listed in os.listdir(probe):
        try:
            os.unlink(probe / listed)
        except FileNotFoundError:
            ok = False
    return ok


@contextmanager
def mounted(fs: str, workdir: Path, size: str = "512m") -> Iterator[Volume]:
    """Create, attach, probe, and finally detach a volume of type ``fs``."""
    image = workdir / f"{fs.replace(' ', '_')}.sparseimage"
    mountpoint = workdir / f"{fs.replace(' ', '_')}.mnt"
    mountpoint.mkdir()
    _hdiutil(
        "create",
        "-quiet",
        "-size",
        size,
        "-type",
        "SPARSE",
        "-fs",
        fs,
        "-volname",
        "remmy-test",
        str(image.with_suffix("")),
    )
    _hdiutil("attach", "-quiet", "-nobrowse", "-noautoopen", "-mountpoint", str(mountpoint), str(image))
    try:
        caps, case_sensitive, permissions = _probe(mountpoint)
        yield Volume(fs, mountpoint, caps, case_sensitive, permissions)
    finally:
        with suppress(VolumeUnavailable):
            _hdiutil("detach", "-quiet", "-force", str(mountpoint))
        image.unlink(missing_ok=True)
