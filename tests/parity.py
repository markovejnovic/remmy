"""Exact parity with BSD /bin/rm: each Case runs under /bin/rm (the oracle), then under remmy, on the same setup
rebuilt at the same absolute path; stdout, stderr, tty echo, exit code and leftover tree must all match.

Both programs run through a symlink named rm, unlink or remmy: BSD rm takes its message prefix from the executed
file's basename (getprogname) and unlink mode from argv[0]. Every run is confined by sandbox_init to writes inside
the case's box, so a program that fails to refuse /, . or .. can only fail the test.
"""

import contextlib
import ctypes
import difflib
import errno
import fcntl
import hashlib
import os
import selectors
import shutil
import signal
import socket
import stat
import subprocess
import sys
import termios
import time
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import fstree
import pytest

RM = Path("/bin/rm")
PROGS = ("rm", "unlink", "remmy")
# /bin/rm never gets near this; a program that misses the / guard, or waits on a tty for an answer, will.
RUN_TIMEOUT = 20.0
INLINE_CONTENT = 256  # regular files up to this size are compared by content, larger ones by hash
# Entries a mounted scratch volume creates on its own; left out of snapshots.
HOUSEKEEPING = (".fseventsd", ".Trashes", ".TemporaryItems", ".DS_Store", ".Spotlight-V100", ".journal", ".HFS+ ")
SANDBOX_FAILED = 125
FULL_PATH = "<full-path>"  # as argv0: pass the symlink's absolute path as argv[0]
UNLINK: dict[str, Any] = {"prog": "unlink", "argv0": FULL_PATH}  # Case(**UNLINK): run as unlink via its full path
FLAG_BITS = {"nodump": stat.UF_NODUMP, "uchg": stat.UF_IMMUTABLE, "uappnd": stat.UF_APPEND, "opaque": stat.UF_OPAQUE}
FLAG_BITS["hidden"] = stat.UF_HIDDEN
MARKS = [pytest.mark.parity, pytest.mark.skipif(sys.platform != "darwin", reason="BSD rm parity is macOS-only")]


@dataclass(frozen=True)
class Entry:
    path: str | bytes
    arg: str | bytes = ""  # File content, Symlink target, Hardlink source (case-root relative) or DeepChain dir name
    depth: int = 0  # DeepChain: path/arg/arg/... depth levels with a file "leaf" at the bottom, past PATH_MAX
    mode: int | None = None
    flags: tuple[str, ...] = ()  # set with lchflags, so on a Symlink it is the link's own


# Mount: an empty 16 MB HFS+ volume attached at path (hdiutil, no root needed).
File, Dir, Symlink, Fifo, Socket, Hardlink, DeepChain, Mount = (
    type(n, (Entry,), {}) for n in ["File", "Dir", "Symlink", "Fifo", "Socket", "Hardlink", "DeepChain", "Mount"]
)


@dataclass(frozen=True)
class Stdin:
    kind: str  # devnull | closed | pipe | pty
    data: bytes = b""

    def __str__(self) -> str:
        return self.kind if self.kind in ("devnull", "closed") else f"{self.kind} {self.data!r}"


DEVNULL, CLOSED = Stdin("devnull"), Stdin("closed")


def pipe(data: str | bytes = b"") -> Stdin:
    return Stdin("pipe", os.fsencode(data))


def pty(data: str | bytes = b"") -> Stdin:
    """A pty with ``data`` typed up front; "\\x04" (VEOF) is an EOF at the start of a line."""
    return Stdin("pty", os.fsencode(data))


Y, N, TY, TN = pipe("y\n"), pipe("n\n"), pty("y\n"), pty("n\n")  # the common one-line answers


@dataclass(frozen=True)
class Case:
    id: str
    args: str | Sequence[str | bytes]
    setup: tuple[Entry, ...] = ()
    cwd: str = "."
    prog: str = "rm"  # the symlink executed
    argv0: str | bytes | None = None  # default prog, as a shell passes it; FULL_PATH for the symlink's path
    env: dict[str, str] = field(default_factory=dict)
    stdin: Stdin = DEVNULL

    def __post_init__(self) -> None:
        if isinstance(self.args, str):  # "-rv d" is shorthand for ["-rv", "d"]
            object.__setattr__(self, "args", self.args.split())


def case_id(case: Case) -> str:
    return case.id


@dataclass(frozen=True)
class Node:
    kind: str
    mode: str
    flags: str = ""
    content: bytes | str | None = None  # bytes, a hash for large files, or a symlink target
    nlink: int | None = None
    mountpoint: bool = False


@dataclass
class Outcome:
    argv: list[bytes]
    returncode: int | None  # None: killed after RUN_TIMEOUT
    stdout: bytes
    stderr: bytes
    tty: bytes  # everything read back from the pty master: echo plus anything written to /dev/tty
    tree: dict[str, Node]


def _sandbox_init():
    lib = ctypes.CDLL("/usr/lib/libSystem.dylib", use_errno=True)  # loaded lazily: the module is imported on Linux
    fn = lib.sandbox_init
    fn.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_char_p)]
    fn.restype = ctypes.c_int
    return fn


class Parity:
    """Fixed per-test layout (bin/, the sandboxed box/, case root box/root) so both runs print the same bytes."""

    def __init__(self, tmp_path: Path) -> None:
        base = Path(os.path.realpath(tmp_path))
        self.bin, self.box, self.images, self.root = base / "bin", base / "box", base / "images", base / "box/root"
        self.bin.mkdir()
        self.attached: list[str] = []
        self.image_files: list[Path] = []
        self.template: Path | None = None

    def _p(self, rel: str | bytes) -> bytes:
        return os.path.join(os.fsencode(self.root), os.fsencode(rel))

    def build(self, setup: tuple[Entry, ...]) -> None:
        self.root.mkdir(parents=True)
        for e in setup:
            p = self._p(e.path)
            match e:
                case File():
                    Path(os.fsdecode(p)).write_bytes(os.fsencode(e.arg))
                case Dir():
                    os.makedirs(p, exist_ok=True)
                case Symlink():
                    os.symlink(os.fsencode(e.arg), p)
                case Fifo():
                    os.mkfifo(p)
                case Socket():
                    # sun_path holds ~104 bytes, so bind relative to the parent directory.
                    cwd, s = os.getcwd(), socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    try:
                        os.chdir(os.path.dirname(p))
                        s.bind(os.path.basename(p))
                    finally:
                        os.chdir(cwd)
                        s.close()
                case Hardlink():
                    os.link(self._p(e.arg), p)
                case DeepChain():
                    os.makedirs(p, exist_ok=True)
                    fd = os.open(p, os.O_RDONLY | os.O_DIRECTORY)
                    try:
                        for _ in range(e.depth):
                            os.mkdir(e.arg, dir_fd=fd)
                            fd, old = os.open(e.arg, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd), fd
                            os.close(old)
                        os.close(os.open("leaf", os.O_WRONLY | os.O_CREAT, 0o644, dir_fd=fd))
                    finally:
                        os.close(fd)
                case Mount():
                    self._mount(p)
        # Deepest first, so a locked directory does not stop its children.
        locked = sorted((e for e in setup if e.mode is not None or e.flags), key=lambda e: -self._p(e.path).count(b"/"))
        for e in locked:
            if e.mode is not None:
                os.chmod(self._p(e.path), e.mode, follow_symlinks=False)
        for e in locked:
            if e.flags:
                os.chflags(self._p(e.path), sum(FLAG_BITS[n] for n in e.flags), follow_symlinks=False)

    def _mount(self, mountpoint: bytes) -> None:
        if shutil.which("hdiutil") is None:
            pytest.skip("needs hdiutil")
        self.images.mkdir(exist_ok=True)
        if self.template is None:
            stem = self.images / "template"
            opts = ["create", "-quiet", "-size", "16m", "-type", "SPARSE", "-fs", "HFS+", "-volname", "rmx"]
            subprocess.run(["hdiutil", *opts, stem], check=True, capture_output=True)
            self.template = stem.with_suffix(".sparseimage")
        # A fresh copy per build: each run starts from the same empty volume.
        image = self.images / f"v{len(self.attached)}-{time.monotonic_ns()}.sparseimage"
        shutil.copyfile(self.template, image)
        os.makedirs(mountpoint, exist_ok=True)
        cmd = ["hdiutil", "attach", "-nobrowse", "-noautoopen", "-mountpoint", os.fsdecode(mountpoint), image]
        self.attached.append(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.split()[0])
        self.image_files.append(image)

    def teardown(self) -> None:
        while self.attached:
            subprocess.run(["hdiutil", "detach", "-force", self.attached.pop()], capture_output=True, check=False)
        while self.image_files:  # 16 MB each, and pytest keeps the last three sessions
            self.image_files.pop().unlink(missing_ok=True)
        fstree.force_remove(self.box)

    def run(self, case: Case) -> Outcome:
        exe = os.fsencode(self.bin / case.prog)
        argv0 = exe if case.argv0 == FULL_PATH else os.fsencode(case.prog if case.argv0 is None else case.argv0)
        argv = [argv0, *(os.fsencode(a).replace(b"{ROOT}", os.fsencode(self.root)) for a in case.args)]
        env = {k: v for k, v in os.environ.items() if k not in ("POSIXLY_CORRECT", "LANG")}
        env = {k: v for k, v in env.items() if not k.startswith(("LC_", "REMMY_", "DYLD_"))}
        env = {"COMMAND_MODE": "unix2003", **env, "LANG": "en_US.UTF-8", **case.env}
        profile = (
            f'(version 1)(allow default)(deny file-write*)(allow file-write* (subpath "{self.box}"))'
            '(allow file-write* (literal "/dev/null") (literal "/dev/tty") (regex #"^/dev/ttys[0-9]+$"))'
        ).encode()
        sandbox_init, kind = _sandbox_init(), case.stdin.kind
        master = slave = None
        stdin = {"pipe": subprocess.PIPE, "devnull": subprocess.DEVNULL, "closed": subprocess.DEVNULL}.get(kind)
        if kind == "pty":
            master, slave = os.openpty()
            stdin = slave
            # Typed before the child exists, while our slave fd holds the pty open: the line discipline queues and
            # echoes it right away, so the echo never depends on how fast the child exits.
            if case.stdin.data:
                os.write(master, case.stdin.data)

        def preexec() -> None:
            # After setsid(): the pty becomes the controlling terminal; without one /dev/tty can't reach pytest's.
            if kind == "pty":
                fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            elif kind == "closed":
                os.close(0)
            if sandbox_init(profile, 0, ctypes.byref(ctypes.c_char_p())) != 0:
                os._exit(SANDBOX_FAILED)

        proc = subprocess.Popen(
            argv,
            executable=exe,
            cwd=self.root / case.cwd,
            env=env,
            stdin=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
            preexec_fn=preexec,  # noqa: PLW1509 - sandbox_init and TIOCSCTTY must run in the child
        )
        # The slave stays open until the master is drained: on the last close of the slave, macOS discards the
        # master's unread output, which would make ``tty`` depend on timing.
        try:
            out, err, tty, rc = _communicate(proc, case.stdin.data, master)
        finally:
            if slave is not None:
                os.close(slave)
        assert rc != SANDBOX_FAILED or err, "sandbox_init failed in the child"
        return Outcome(argv, rc, out, err, tty, snapshot(self.box))

    def outcome(self, target: Path, case: Case) -> Outcome:
        for name in PROGS:
            link = self.bin / name
            if os.path.lexists(link):
                link.unlink()
            link.symlink_to(target)
        try:
            self.build(case.setup)
            return self.run(case)
        finally:
            self.teardown()


def _communicate(proc: subprocess.Popen, data: bytes, master: int | None) -> tuple[bytes, bytes, bytes, int | None]:
    if proc.stdin is not None:
        with contextlib.suppress(BrokenPipeError):
            proc.stdin.write(data)
        with contextlib.suppress(BrokenPipeError):
            proc.stdin.close()
    assert proc.stdout and proc.stderr
    pipes = [proc.stdout.fileno(), proc.stderr.fileno()]
    fds = pipes + ([master] if master is not None else [])
    bufs = {fd: bytearray() for fd in fds}
    sel = selectors.DefaultSelector()
    for fd in fds:
        os.set_blocking(fd, False)
        sel.register(fd, selectors.EVENT_READ)
    deadline, open_pipes = time.monotonic() + RUN_TIMEOUT, set(pipes)
    while open_pipes and (left := deadline - time.monotonic()) > 0:
        for key, _ in sel.select(timeout=min(left, 0.5)):
            try:
                chunk = os.read(key.fd, 65536)
            except BlockingIOError:
                continue
            except OSError as e:  # EIO: the pty's slave side is gone
                if e.errno != errno.EIO:
                    raise
                chunk = b""
            if chunk:
                bufs[key.fd] += chunk
            else:
                sel.unregister(key.fd)
                open_pipes.discard(key.fd)
    if timed_out := bool(open_pipes):
        with contextlib.suppress(ProcessLookupError):
            os.killpg(proc.pid, signal.SIGKILL)
    rc = proc.wait()
    if master is not None:  # drain the rest (echo of the typed answers)
        with contextlib.suppress(OSError):
            while chunk := os.read(master, 65536):
                bufs[master] += chunk
        os.close(master)
    sel.close()
    proc.stdout.close()
    proc.stderr.close()
    tty = bytes(bufs[master]) if master is not None else b""
    return bytes(bufs[pipes[0]]), bytes(bufs[pipes[1]]), tty, None if timed_out else rc


KINDS = {stat.S_IFDIR: "dir", stat.S_IFREG: "file", stat.S_IFLNK: "symlink", stat.S_IFIFO: "fifo"}
KINDS[stat.S_IFSOCK] = "socket"


def _kind(mode: int) -> str:
    return KINDS.get(stat.S_IFMT(mode), oct(stat.S_IFMT(mode)))


def _lines(b: bytes) -> list[str]:
    return [repr(ln) for ln in b.splitlines(keepends=True)] or ["b''"]


def _flag_names(bits: int) -> str:
    rest = bits & ~sum(FLAG_BITS.values())
    return ",".join([n for n, b in FLAG_BITS.items() if bits & b] + ([hex(rest)] if rest else []))


def snapshot(top: Path) -> dict[str, Node]:
    """Every path under ``top`` (itself ".") as an identity-free Node, via dir fds so it goes past PATH_MAX.

    Modes and flags are recorded first; the walk then clears flags and opens permissions, as teardown follows.
    """
    try:
        st = os.lstat(top)
    except FileNotFoundError:
        return {}
    out = {".": Node(_kind(st.st_mode), oct(stat.S_IMODE(st.st_mode)), _flag_names(st.st_flags))}
    top_dev = st.st_dev

    def walk(dfd: int, rel: bytes, dev: int) -> None:
        for name in sorted(os.listdir(dfd)):
            if dev != top_dev and name.startswith(HOUSEKEEPING):
                continue
            path = os.path.join(rel, os.fsencode(name)) if rel else os.fsencode(name)
            st = os.lstat(name, dir_fd=dfd)
            k, perm, content, nlink = _kind(st.st_mode), stat.S_IMODE(st.st_mode), None, None
            if st.st_flags:
                os.lchflags(os.path.join(os.fsencode(top), path), 0)
            if k == "file":
                nlink = st.st_nlink
                if not perm & stat.S_IRUSR:
                    os.chmod(name, perm | stat.S_IRUSR, dir_fd=dfd)
                with os.fdopen(os.open(name, os.O_RDONLY, dir_fd=dfd), "rb") as f:
                    data = f.read()
                content = data if len(data) <= INLINE_CONTENT else "blake2b:" + hashlib.blake2b(data).hexdigest()
            elif k == "symlink":
                content = os.readlink(os.fsencode(name), dir_fd=dfd)
            out[os.fsdecode(path)] = Node(k, oct(perm), _flag_names(st.st_flags), content, nlink, st.st_dev != dev)
            if k == "dir":
                if perm & 0o700 != 0o700:
                    os.chmod(name, perm | 0o700, dir_fd=dfd)
                cfd = os.open(name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=dfd)
                try:
                    walk(cfd, path, st.st_dev)
                finally:
                    os.close(cfd)

    if stat.S_ISDIR(st.st_mode):
        if st.st_flags:
            os.chflags(top, 0)
        os.chmod(top, stat.S_IMODE(st.st_mode) | 0o700)
        fd = os.open(top, os.O_RDONLY | os.O_DIRECTORY)
        try:
            walk(fd, b"", top_dev)
        finally:
            os.close(fd)
    return out


def mismatches(ref: Outcome, got: Outcome) -> list[str]:
    out = []
    if ref.returncode != got.returncode:
        rm, remmy = ("killed after timeout" if o.returncode is None else o.returncode for o in (ref, got))
        out.append(f"exit code: rm {rm}, remmy {remmy}")
    for name in ("stdout", "stderr", "tty"):
        a, b = getattr(ref, name), getattr(got, name)
        if a != b:
            diff = difflib.unified_diff(_lines(a), _lines(b), f"{name} (rm)", f"{name} (remmy)", lineterm="", n=2)
            out += [f"{name} differs:", *(f"    {ln}" for ln in list(diff)[:80])]
    if ref.tree != got.tree:
        r, g = ref.tree, got.tree
        out.append("tree differs:")
        out += [f"    only rm left:    {k!r} {r[k]}" for k in sorted(r.keys() - g.keys())[:30]]
        out += [f"    only remmy left: {k!r} {g[k]}" for k in sorted(g.keys() - r.keys())[:30]]
        for k in sorted(k for k in r.keys() & g.keys() if r[k] != g[k])[:30]:
            out += [f"    {k!r}:", f"        rm:    {r[k]}", f"        remmy: {g[k]}"]
    return out


def assert_parity(case: Case, remmy_bin: Path, tmp_path: Path, *, is_root: bool = False) -> None:
    """Run ``case`` under /bin/rm and then under ``remmy_bin``; every difference fails the test."""
    if not os.access(RM, os.X_OK):
        pytest.skip(f"{RM} not available")
    if is_root and any(e.mode is not None or e.flags for e in case.setup):
        pytest.skip("root bypasses modes and file flags")
    p = Parity(tmp_path)
    try:
        ref = p.outcome(RM, case)
        assert ref.returncode is not None, f"/bin/rm itself timed out on {case.id}; the case is broken"
        got = p.outcome(remmy_bin, case)
    finally:
        if p.template is not None:
            p.template.unlink(missing_ok=True)
    if problems := mismatches(ref, got):
        header = [
            f"remmy differs from /bin/rm on case {case.id!r}",
            f"  argv:  {[os.fsdecode(a) for a in ref.argv]!r} (exe {p.bin / case.prog})",
            f"  cwd:   {p.root / case.cwd}",
            f"  stdin: {case.stdin}",
            *([f"  env:   {case.env}"] if case.env else []),
        ]
        pytest.fail("\n".join(header + problems), pytrace=False)


def parity_test(*cases: Case, slow: bool = False):
    """A test running assert_parity over ``cases``; assign it to a ``test_*`` module global (that is its name)."""

    @pytest.mark.parametrize("case", list(cases), ids=case_id)
    def test(case: Case, remmy_bin: Path, tmp_path: Path, is_root: bool) -> None:
        assert_parity(case, remmy_bin, tmp_path, is_root=is_root)

    for mark in [*MARKS, *([pytest.mark.slow] if slow else [])]:
        test = mark(test)
    return test
