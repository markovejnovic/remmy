"""Scratch volumes of other filesystem types: user-owned sparse disk images, no root needed."""

import os
import shutil
import subprocess
import sys
from contextlib import contextmanager, suppress
from dataclasses import dataclass
from pathlib import Path

# hdiutil -fs names, as listed by `diskutil listFilesystems`.
FILESYSTEMS = ("APFS", "Case-sensitive APFS", "HFS+", "Case-sensitive HFS+")


@dataclass(frozen=True)
class Volume:
    fs: str
    root: Path
    case_sensitive: bool
    permissions: bool


class VolumeUnavailable(Exception):
    pass


def _hdiutil(*args: str) -> None:
    if sys.platform != "darwin" or shutil.which("hdiutil") is None:
        raise VolumeUnavailable("hdiutil is macOS-only")
    proc = subprocess.run(["hdiutil", *args], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise VolumeUnavailable(f"hdiutil {args[0]} failed: {proc.stderr.strip()}")


@contextmanager
def mounted(fs: str, workdir: Path, size: str = "512m"):
    """Create, attach, probe, and finally detach a volume of type ``fs``."""
    stem = workdir / fs.replace(" ", "_")
    mnt, image = stem.with_suffix(".mnt"), stem.with_suffix(".sparseimage")
    mnt.mkdir()
    _hdiutil("create", "-quiet", "-size", size, "-type", "SPARSE", "-fs", fs, "-volname", "remmy-test", str(stem))
    _hdiutil("attach", "-quiet", "-nobrowse", "-noautoopen", "-mountpoint", str(mnt), str(image))
    try:
        probe = mnt / ".probe"
        probe.mkdir()
        (probe / "a").write_text("")
        os.chmod(probe / "a", 0o400)
        permissions = (os.stat(probe / "a").st_mode & 0o777) == 0o400
        case_sensitive = not (probe / "A").exists()
        shutil.rmtree(probe)
        yield Volume(fs, mnt, case_sensitive, permissions)
    finally:
        with suppress(VolumeUnavailable):
            _hdiutil("detach", "-quiet", "-force", str(mnt))
        image.unlink(missing_ok=True)
