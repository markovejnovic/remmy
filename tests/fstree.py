"""Build, snapshot, and tear down trees, fd-relative (``dir_fd=``) so paths may exceed PATH_MAX."""

import hashlib
import os
import shutil
import socket
import stat
import tempfile
from contextlib import contextmanager, suppress
from dataclasses import dataclass
from enum import Enum, auto
from pathlib import Path

import jaraco.path
from jaraco.path import Symlink  # noqa: F401 - re-exported: specs use fstree.Symlink

Spec = jaraco.path.FilesSpec


class Special(Enum):
    FIFO = auto()
    SOCKET = auto()


@dataclass(frozen=True)
class Hardlink:
    source: str  # relative to the link's directory; must be earlier in the spec


@jaraco.path.create.register
def _(content: Special, path: Path) -> None:
    if content is Special.FIFO:
        return os.mkfifo(path)
    # sun_path is ~104 bytes on macOS, so bind relative to the parent.
    cwd = os.getcwd()
    with socket.socket(socket.AF_UNIX) as s:
        try:
            os.chdir(path.parent)
            s.bind(path.name)
        finally:
            os.chdir(cwd)


@jaraco.path.create.register
def _(content: Hardlink, path: Path) -> None:
    os.link(path.parent / content.source, path)


def build(root: Path, spec: Spec) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    jaraco.path.build(spec, root)
    return root


def deep_chain(root: Path, depth: int, name: str = "d", leaf_files: int = 1) -> int:
    """Create ``root/name/name/...`` ``depth`` deep, ``leaf_files`` files per level; return the deepest path's length."""
    root.mkdir(parents=True, exist_ok=True)
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        for _ in range(depth):
            os.mkdir(name, dir_fd=fd)
            nxt = os.open(name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd)
            os.close(fd)
            fd = nxt
            for i in range(leaf_files):
                os.close(os.open(f"f{i}", os.O_WRONLY | os.O_CREAT, 0o644, dir_fd=fd))
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


KINDS = {
    stat.S_IFDIR: "dir",
    stat.S_IFREG: "file",
    stat.S_IFLNK: "symlink",
    stat.S_IFIFO: "fifo",
    stat.S_IFSOCK: "socket",
}


def _entry(st: os.stat_result, name: Path | str, dirfd: int | None) -> FileSnapshot:
    mode, digest = st.st_mode, None
    if stat.S_ISREG(mode):
        with open(os.open(name, os.O_RDONLY, dir_fd=dirfd), "rb") as f:
            digest = hashlib.file_digest(f, lambda: hashlib.blake2b(digest_size=16)).hexdigest()
    elif stat.S_ISLNK(mode):
        digest = os.readlink(name, dir_fd=dirfd)
    kind = KINDS.get(stat.S_IFMT(mode), oct(stat.S_IFMT(mode)))
    return FileSnapshot(
        kind, stat.S_IMODE(mode), 0 if stat.S_ISDIR(mode) else st.st_size, st.st_nlink, st.st_ino, digest
    )


def snapshot_dir(root: Path) -> dict[str, FileSnapshot]:
    """Map every path under ``root`` (inclusive, as "") to its identity."""
    try:
        st = os.lstat(root)
    except FileNotFoundError:
        return {}
    out = {"": _entry(st, root, None)}
    if stat.S_ISDIR(st.st_mode):
        for dirpath, dirnames, filenames, dirfd in os.fwalk(root, follow_symlinks=False):
            rel = "" if (rel := os.path.relpath(dirpath, root)) == "." else rel
            for n in dirnames + filenames:
                out[os.path.join(rel, n)] = _entry(os.lstat(n, dir_fd=dirfd), n, dirfd)
    return out


def exists(p: Path) -> bool:
    """True if anything, even a dangling symlink, lives at ``p``."""
    try:
        return bool(os.lstat(p))
    except FileNotFoundError:
        return False


def listing(root: Path) -> set[str]:
    return {k for k in snapshot_dir(root) if k}


@contextmanager
def chmod(p: Path, mode: int):
    """Temporarily change ``p``'s mode, restoring it unless ``p`` is gone."""
    old = stat.S_IMODE(os.lstat(p).st_mode)
    os.chmod(p, mode)
    try:
        yield
    finally:
        with suppress(FileNotFoundError):
            os.chmod(p, old)


@contextmanager
def scratch(parent: Path):
    """A fresh directory under ``parent``, force-removed afterwards (Hypothesis can't use per-example fixtures)."""
    d = Path(tempfile.mkdtemp(dir=parent))
    try:
        yield d
    finally:
        force_remove(d)


def chmod_nofollow(name: str, mode: int, *, dir_fd: int) -> None:
    # Linux cannot chmod a symlink itself, so symlinks are skipped everywhere.
    if not stat.S_ISLNK(os.stat(name, dir_fd=dir_fd, follow_symlinks=False).st_mode):
        os.chmod(name, mode, dir_fd=dir_fd)


def _clear_flags(path: str) -> None:
    # BSD file flags such as uchg block unlink even for the owner.
    with suppress(AttributeError, OSError):
        if os.lstat(path).st_flags:
            os.lchflags(path, 0)


def force_remove(root: Path) -> None:
    """Delete ``root`` regardless of permissions, file flags (``uchg``) or depth."""
    if not exists(root):
        return
    _clear_flags(str(root))
    if not stat.S_ISDIR(os.lstat(root).st_mode):
        return root.unlink()
    os.chmod(root, 0o700)
    for dirpath, dirnames, filenames, dirfd in os.fwalk(root, follow_symlinks=False):
        for d in dirnames:
            _clear_flags(os.path.join(dirpath, d))
            with suppress(FileNotFoundError):
                chmod_nofollow(d, 0o700, dir_fd=dirfd)
        for f in filenames:
            _clear_flags(os.path.join(dirpath, f))
    shutil.rmtree(root)
