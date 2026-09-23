"""Filesystem fixtures: build trees, snapshot them, and tear them down.

Everything here is fd-relative (``dir_fd=``) so trees deeper than PATH_MAX can
be built, inspected, and cleaned up.
"""

from __future__ import annotations

import errno
import hashlib
import os
import shutil
import socket
import stat
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from enum import Enum, auto
from pathlib import Path

import jaraco.path
from jaraco.path import Symlink  # noqa: F401 - re-exported: specs use fstree.Symlink


class Special(Enum):
    FIFO = auto()
    SOCKET = auto()


@dataclass(frozen=True)
class Hardlink:
    """A hard link to ``source``, relative to the link's own directory.

    The source must already exist, so list it earlier in the spec.
    """

    source: str


@jaraco.path.create.register
def _(content: Special, path: Path) -> None:
    match content:
        case Special.FIFO:
            os.mkfifo(path)
        case Special.SOCKET:
            # sun_path is ~104 bytes on macOS, so bind relative to the parent.
            cwd = os.getcwd()
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                os.chdir(path.parent)
                s.bind(path.name)
            finally:
                os.chdir(cwd)
                s.close()


@jaraco.path.create.register
def _(content: Hardlink, path: Path) -> None:
    os.link(path.parent / content.source, path)


Spec = jaraco.path.FilesSpec


def build(root: Path, spec: Spec) -> Path:
    """Create ``spec`` under ``root`` (which is created if missing)."""
    root.mkdir(parents=True, exist_ok=True)
    jaraco.path.build(spec, root)
    return root


def deep_chain(root: Path, depth: int, name: str = "d", leaf_files: int = 1) -> int:
    """Create ``root/name/name/...`` ``depth`` levels deep via dir fds.

    Each level also gets ``leaf_files`` regular files. Returns the byte length
    of the deepest path relative to ``root``'s parent.
    """
    root.mkdir(parents=True, exist_ok=True)
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        for _ in range(depth):
            os.mkdir(name, dir_fd=fd)
            nxt = os.open(name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd)
            os.close(fd)
            fd = nxt
            for i in range(leaf_files):
                f = os.open(f"f{i}", os.O_WRONLY | os.O_CREAT, 0o644, dir_fd=fd)
                os.close(f)
    finally:
        os.close(fd)
    return len(os.fsencode(root.name)) + depth * (len(os.fsencode(name)) + 1)


@dataclass(frozen=True)
class FileSnapshot:
    kind: str
    mode: int
    size: int
    nlink: int
    ino: int
    digest: str | None  # content hash for regular files, target for links


def snapshot_dir(root: Path) -> dict[str, FileSnapshot]:
    """Map every path under ``root`` (inclusive, as "") to its identity."""

    def _entry(st: os.stat_result, name: Path | str, dirfd: int | None) -> FileSnapshot:
        mode = st.st_mode
        digest = None
        if stat.S_ISREG(mode):
            h = hashlib.blake2b(digest_size=16)
            fd = os.open(name, os.O_RDONLY, dir_fd=dirfd)
            with os.fdopen(fd, "rb") as f:
                while chunk := f.read(1 << 16):
                    h.update(chunk)
            digest = h.hexdigest()
        elif stat.S_ISLNK(mode):
            digest = os.readlink(name, dir_fd=dirfd)
        kind = {
            stat.S_IFDIR: "dir",
            stat.S_IFREG: "file",
            stat.S_IFLNK: "symlink",
            stat.S_IFIFO: "fifo",
            stat.S_IFSOCK: "socket",
        }.get(stat.S_IFMT(mode), oct(stat.S_IFMT(mode)))
        size = 0 if stat.S_ISDIR(mode) else st.st_size
        return FileSnapshot(kind, stat.S_IMODE(mode), size, st.st_nlink, st.st_ino, digest)

    out: dict[str, FileSnapshot] = {}
    try:
        st = os.lstat(root)
    except FileNotFoundError:
        return out
    out[""] = _entry(st, root, None)
    if stat.S_ISDIR(st.st_mode):
        for dirpath, dirnames, filenames, dirfd in os.fwalk(root, follow_symlinks=False):
            rel = os.path.relpath(dirpath, root)
            rel = "" if rel == "." else rel
            for n in dirnames + filenames:
                s = os.lstat(n, dir_fd=dirfd)
                out[os.path.join(rel, n)] = _entry(s, n, dirfd)
    return out


def exists(p: Path) -> bool:
    """True if anything, even a dangling symlink, lives at ``p``."""
    try:
        os.lstat(p)
    except FileNotFoundError:
        return False
    return True


def listing(root: Path) -> set[str]:
    """Relative paths under ``root`` (without root itself)."""
    return {k for k in snapshot_dir(root) if k}


@contextmanager
def chmod(p: Path, mode: int) -> Iterator[None]:
    """Temporarily change ``p``'s mode, restoring it even if ``p`` moved."""
    old = stat.S_IMODE(os.lstat(p).st_mode)
    os.chmod(p, mode)
    try:
        yield
    finally:
        try:
            os.chmod(p, old)
        except FileNotFoundError:
            pass


@contextmanager
def scratch(parent: Path) -> Iterator[Path]:
    """A fresh directory under ``parent``, force-removed afterwards.

    For Hypothesis tests, which cannot use function-scoped fixtures per example.
    """
    d = Path(tempfile.mkdtemp(dir=parent))
    try:
        yield d
    finally:
        force_remove(d)


def force_remove(root: Path) -> None:
    """Delete ``root`` regardless of permissions, file flags (``uchg``) or depth."""
    try:
        st = os.lstat(root)
    except FileNotFoundError:
        return

    if not stat.S_ISDIR(st.st_mode):
        _clear_flags(str(root))
        root.unlink()
        return

    _clear_flags(str(root))
    os.chmod(root, 0o700)
    for dirpath, dirnames, filenames, dirfd in os.fwalk(root, follow_symlinks=False):
        for d in dirnames:
            try:
                _clear_flags(os.path.join(dirpath, d))
                os.chmod(d, 0o700, dir_fd=dirfd, follow_symlinks=False)
            except OSError as e:
                if e.errno != errno.ENOENT:
                    raise
        for f in filenames:
            _clear_flags(os.path.join(dirpath, f))

    shutil.rmtree(root)


def _clear_flags(path: str) -> None:
    """Drop BSD file flags such as ``uchg`` that block unlink even for the owner."""
    try:
        if os.lstat(path).st_flags:
            os.lchflags(path, 0)
    except (AttributeError, OSError):
        pass
