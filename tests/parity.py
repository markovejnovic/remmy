"""Exact parity with BSD ``/bin/rm``: same bytes out, same exit code, same tree left.

Each :class:`Case` is run twice, one program after the other, on the same setup
rebuilt at the same absolute path from the same cwd. ``/bin/rm`` runs first (the
oracle) and then the program under test. Nothing is compared against recorded
text, so output that depends on the machine, such as the ``user/group`` in
override prompts, is handled because ``/bin/rm`` produces it live.

Both programs are started through a symlink in a per-test ``bin`` directory.
The symlink's basename is ``rm``, ``unlink`` or ``remmy``. BSD rm takes its
message prefix from the executed file's basename (``getprogname()``) and
chooses unlink mode from ``argv[0]``, so both programs see the same name and
the same ``argv[0]``.

Every run is confined by ``sandbox_init`` to writes inside the case's box. A
program that fails to refuse ``/``, ``.`` or ``..`` can then only fail the
test; it cannot delete anything outside the test's temp directory.

A plain module rather than a conftest, like ``harness``.
"""

from __future__ import annotations

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
import termios
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

import fstree
import pytest

RM = Path("/bin/rm")

NOT_YET_AT_PARITY = pytest.mark.xfail(
    strict=True,
    reason="remmy does not yet match /bin/rm byte for byte; add the case to AT_PARITY once it does",
)
"""Applied by ``conftest`` to every ``parity`` case missing from :data:`AT_PARITY`.
Strict, so a case that starts passing fails (XPASS) until it is listed."""

AT_PARITY = frozenset(
    (
        "test_rm_parity_cli.py::test_directories[r_tree]",
        "test_rm_parity_cli.py::test_invocation_names[unlink_bin_direct]",
        "test_rm_parity_cli.py::test_invocation_names[unlink_dash]",
        "test_rm_parity_cli.py::test_invocation_names[unlink_dashdash]",
        "test_rm_parity_cli.py::test_invocation_names[unlink_mode_dashdash_dashdash]",
        "test_rm_parity_cli.py::test_invocation_names[unlink_one]",
        "test_rm_parity_cli.py::test_invocation_names[unlink_readonly_pty]",
        "test_rm_parity_cli.py::test_invocation_names[unlink_symlink]",
        "test_rm_parity_cli.py::test_legacy_command_mode[legacy_ro_devnull]",
        "test_rm_parity_cli.py::test_operands[dash_operand_alone_present]",
        "test_rm_parity_cli.py::test_operands[dash_operand_present]",
        "test_rm_parity_cli.py::test_operands[dashfile_via_dashdash]",
        "test_rm_parity_cli.py::test_operands[file_plain]",
        "test_rm_parity_cli.py::test_override_prompts[ro_file_devnull]",
        "test_rm_parity_cli.py::test_override_prompts[ro_file_pipe]",
        "test_rm_parity_cli.py::test_override_prompts[ro_symlink_pty]",
        "test_rm_parity_cli.py::test_override_prompts[wo_file_pty]",
        "test_rm_parity_cli.py::test_usage_and_getopt[dashdash_twice]",
        "test_rm_parity_edges.py::test_unlink_mode[unlink_dangling_symlink]",
        "test_rm_parity_edges.py::test_unlink_mode[unlink_fifo]",
        "test_rm_parity_edges.py::test_unlink_mode[unlink_ro_devnull]",
        "test_rm_parity_edges.py::test_unlink_mode[unlink_ro_pty_y]",
        "test_rm_parity_edges.py::test_unlink_mode[unlink_symlink_to_dir]",
        "test_rm_parity_fs.py::test_invocation_names[argv0_path_to_unlink_name]",
        "test_rm_parity_fs.py::test_invocation_names[argv0_symlink_unlink_dd]",
        "test_rm_parity_fs.py::test_invocation_names[argv0_symlink_unlink]",
        "test_rm_parity_fs.py::test_links_and_special_files[fifo_socket_in_tree]",
        "test_rm_parity_fs.py::test_links_and_special_files[symlink_to_dir]",
        "test_rm_parity_fs.py::test_permissions[file_0444_closed_stdin]",
        "test_rm_parity_fs.py::test_permissions[file_0444_pipe]",
        "test_rm_parity_fs.py::test_permissions[symlink_to_0444_pty]",
        "test_rm_parity_fs.py::test_recursion_and_order[r_tree_quiet]",
    )
)
"""Parity cases remmy already passes, as ``<module file>::<test id>``. A listed
case that regresses fails outright; list a case when remmy starts to pass it."""
PROGS = ("rm", "unlink", "remmy")
"""Names of the symlinks a case may execute; each points at the program under test."""

RUN_TIMEOUT = 20.0
"""Seconds before a run is killed. /bin/rm never gets near this. A program that
misses the ``/`` guard, or waits for an answer on a tty that never comes, will."""

INLINE_CONTENT = 256
"""Regular files up to this size are compared by content, larger ones by hash."""

HOUSEKEEPING = (
    ".fseventsd",
    ".Trashes",
    ".TemporaryItems",
    ".DS_Store",
    ".Spotlight-V100",
    ".journal",
    ".HFS+ Private",
)
"""Entries a mounted scratch volume creates on its own; left out of snapshots."""


# --------------------------------------------------------------------------- setup


@dataclass(frozen=True)
class File:
    path: str | bytes
    content: str | bytes = ""
    mode: int | None = None
    flags: tuple[str, ...] = ()


@dataclass(frozen=True)
class Dir:
    path: str | bytes
    mode: int | None = None
    flags: tuple[str, ...] = ()


@dataclass(frozen=True)
class Symlink:
    path: str | bytes
    target: str | bytes
    flags: tuple[str, ...] = ()  # set on the link itself (lchflags)


@dataclass(frozen=True)
class Fifo:
    path: str | bytes


@dataclass(frozen=True)
class Socket:
    path: str | bytes


@dataclass(frozen=True)
class Hardlink:
    path: str | bytes
    source: str | bytes  # relative to the case root


@dataclass(frozen=True)
class DeepChain:
    """``path/name/name/...`` ``depth`` levels deep with a file ``leaf`` at the
    bottom. It is built through directory fds, so it can go past PATH_MAX."""

    path: str | bytes
    name: str
    depth: int


@dataclass(frozen=True)
class Mount:
    """An empty 16 MB HFS+ volume attached at ``path`` (hdiutil, no root needed).
    Later entries can put files inside it."""

    path: str | bytes


Entry = File | Dir | Symlink | Fifo | Socket | Hardlink | DeepChain | Mount

FLAG_BITS = {
    "nodump": stat.UF_NODUMP,
    "uchg": stat.UF_IMMUTABLE,
    "uappnd": stat.UF_APPEND,
    "opaque": stat.UF_OPAQUE,
    "hidden": stat.UF_HIDDEN,
}
FLAG_NAMES = {bit: name for name, bit in FLAG_BITS.items()}


# --------------------------------------------------------------------------- stdin


@dataclass(frozen=True)
class Stdin:
    kind: str  # "devnull" | "closed" | "pipe" | "pty"
    data: bytes = b""

    def __str__(self) -> str:
        return self.kind if self.kind in ("devnull", "closed") else f"{self.kind} {self.data!r}"


DEVNULL = Stdin("devnull")
CLOSED = Stdin("closed")


def pipe(data: str | bytes = b"") -> Stdin:
    """stdin is a pipe holding ``data``, then EOF."""
    return Stdin("pipe", os.fsencode(data))


def pty(data: str | bytes = b"") -> Stdin:
    """stdin is a pty and ``data`` is typed into it up front, as the probes did.
    Send ``"\\x04"`` (VEOF) for an EOF at the start of a line."""
    return Stdin("pty", os.fsencode(data))


# --------------------------------------------------------------------------- cases


FULL_PATH = "<full-path>"
"""Use as ``argv0`` to pass the symlink's absolute path as ``argv[0]``."""


@dataclass(frozen=True)
class Case:
    id: str
    args: Sequence[str | bytes]
    setup: Sequence[Entry] = ()
    cwd: str = "."
    prog: str = "rm"
    """Basename of the symlink that is executed: rm, unlink or remmy."""
    argv0: str | bytes | None = None
    """``argv[0]``. The default is ``prog``, the way a shell passes a command it
    found on PATH. :data:`FULL_PATH` passes the symlink's absolute path."""
    env: Mapping[str, str] = field(default_factory=dict)
    stdin: Stdin = DEVNULL

    def __post_init__(self) -> None:
        assert self.prog in PROGS, self.prog

    @property
    def needs_unprivileged(self) -> bool:
        """Root bypasses modes and user flags, so these cases mean nothing as root."""
        return any(getattr(e, "mode", None) is not None or getattr(e, "flags", ()) for e in self.setup)

    @property
    def needs_mount(self) -> bool:
        return any(isinstance(e, Mount) for e in self.setup)


def case_id(case: Case) -> str:
    return case.id


# --------------------------------------------------------------------------- outcome


@dataclass(frozen=True)
class Node:
    kind: str
    mode: str
    flags: str = ""
    content: bytes | str | None = None  # bytes, a hash for large files, or a symlink target
    nlink: int | None = None  # regular files only
    mountpoint: bool = False


@dataclass
class Outcome:
    argv: list[bytes]
    returncode: int | None  # None: killed after RUN_TIMEOUT
    stdout: bytes
    stderr: bytes
    tty: bytes  # everything read back from the pty master (echo, plus anything written to /dev/tty)
    tree: dict[str, Node]


# --------------------------------------------------------------------------- sandbox


SANDBOX_FAILED = 125
"""Exit status of a child whose ``sandbox_init`` failed, before anything ran."""


def _sandbox_init() -> ctypes._CFuncPtr:
    # Loaded on first use: the module is imported (and skipped) on Linux too.
    lib = ctypes.CDLL("/usr/lib/libSystem.dylib", use_errno=True)
    fn = lib.sandbox_init
    fn.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_char_p)]
    fn.restype = ctypes.c_int
    return fn


def _profile(box: Path) -> bytes:
    return (
        "(version 1)(allow default)(deny file-write*)"
        f'(allow file-write* (subpath "{box}"))'
        '(allow file-write* (literal "/dev/null") (literal "/dev/tty") (regex #"^/dev/ttys[0-9]+$"))'
    ).encode()


# --------------------------------------------------------------------------- the runner


def _bind_socket(path: bytes) -> None:
    # sun_path holds ~104 bytes, so bind relative to the parent directory.
    cwd = os.getcwd()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        os.chdir(os.path.dirname(path))
        s.bind(os.path.basename(path))
    finally:
        os.chdir(cwd)
        s.close()


class Parity:
    """The shared layout for one test: ``bin/``, the ``box/`` the sandbox allows
    writes in, and the case root ``box/root``. They sit at fixed absolute paths
    so that both runs print the same bytes."""

    def __init__(self, tmp_path: Path, remmy_bin: Path) -> None:
        base = Path(os.path.realpath(tmp_path))
        self.bin = base / "bin"
        self.box = base / "box"
        self.root = self.box / "root"
        self.images = base / "images"
        self.remmy_bin = remmy_bin
        self.bin.mkdir()
        self._attached: list[str] = []
        self._images: list[Path] = []
        self._template: Path | None = None

    # -- symlinks -------------------------------------------------------------

    def _point_bin_at(self, target: Path) -> None:
        for name in PROGS:
            link = self.bin / name
            if fstree.exists(link):
                link.unlink()
            link.symlink_to(target)

    # -- building -------------------------------------------------------------

    def _p(self, rel: str | bytes) -> bytes:
        return os.path.join(os.fsencode(self.root), os.fsencode(rel))

    def build(self, setup: Sequence[Entry]) -> None:
        self.root.mkdir(parents=True)
        later: list[tuple[bytes, int | None, tuple[str, ...]]] = []
        for e in setup:
            p = self._p(e.path)
            match e:
                case File():
                    with open(p, "wb") as f:
                        f.write(os.fsencode(e.content))
                case Dir():
                    os.makedirs(p, exist_ok=True)
                case Symlink():
                    os.symlink(os.fsencode(e.target), p)
                case Fifo():
                    os.mkfifo(p)
                case Socket():
                    _bind_socket(p)
                case Hardlink():
                    os.link(self._p(e.source), p)
                case DeepChain():
                    fstree.deep_chain(Path(os.fsdecode(p)), e.depth, e.name, leaf_files=0)
                    self._touch_leaf(p, e.name, e.depth)
                case Mount():
                    self._mount(p)
            if getattr(e, "mode", None) is not None or getattr(e, "flags", ()):
                later.append((p, getattr(e, "mode", None), getattr(e, "flags", ())))
        # Deepest first, so a locked directory does not stop its children.
        later.sort(key=lambda x: -x[0].count(b"/"))
        for p, mode, _ in later:
            if mode is not None:
                os.chmod(p, mode, follow_symlinks=False)
        for p, _, flags in later:
            if flags:
                bits = 0
                for name in flags:
                    bits |= FLAG_BITS[name]
                os.chflags(p, bits, follow_symlinks=False)

    @staticmethod
    def _touch_leaf(top: bytes, name: str, depth: int) -> None:
        fd = os.open(top, os.O_RDONLY | os.O_DIRECTORY)
        try:
            for _ in range(depth):
                nxt = os.open(name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd)
                os.close(fd)
                fd = nxt
            os.close(os.open("leaf", os.O_WRONLY | os.O_CREAT, 0o644, dir_fd=fd))
        finally:
            os.close(fd)

    def _mount(self, mountpoint: bytes) -> None:
        if shutil.which("hdiutil") is None:
            pytest.skip("needs hdiutil")
        self.images.mkdir(exist_ok=True)
        if self._template is None:
            stem = self.images / "template"
            subprocess.run(
                [
                    "hdiutil",
                    "create",
                    "-quiet",
                    "-size",
                    "16m",
                    "-type",
                    "SPARSE",
                    "-fs",
                    "HFS+",
                    "-volname",
                    "rmx",
                    stem,
                ],
                check=True,
                capture_output=True,
            )
            self._template = stem.with_suffix(".sparseimage")
        # A fresh copy per build: each run starts from the same empty volume.
        image = self.images / f"v{len(self._attached)}-{time.monotonic_ns()}.sparseimage"
        shutil.copyfile(self._template, image)
        os.makedirs(mountpoint, exist_ok=True)
        out = subprocess.run(
            ["hdiutil", "attach", "-nobrowse", "-noautoopen", "-mountpoint", os.fsdecode(mountpoint), image],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self._attached.append(out.split()[0])  # /dev/diskN, the whole image
        self._images.append(image)

    def teardown(self) -> None:
        while self._attached:
            dev = self._attached.pop()
            subprocess.run(["hdiutil", "detach", "-force", dev], capture_output=True, check=False)
        # Each copy is 16 MB on disk, and pytest keeps the last three sessions.
        while self._images:
            self._images.pop().unlink(missing_ok=True)
        fstree.force_remove(self.box)

    def close(self) -> None:
        """Drop the image template once both runs are done."""
        if self._template is not None:
            self._template.unlink(missing_ok=True)

    # -- running --------------------------------------------------------------

    def _env(self, case: Case) -> dict[str, str]:
        env = {
            k: v
            for k, v in os.environ.items()
            if k not in ("POSIXLY_CORRECT", "LANG") and not k.startswith(("LC_", "REMMY_", "DYLD_"))
        }
        env["LANG"] = "en_US.UTF-8"
        env.setdefault("COMMAND_MODE", "unix2003")
        env.update(case.env)
        return env

    def _argv(self, case: Case) -> tuple[bytes, list[bytes]]:
        exe = os.fsencode(self.bin / case.prog)
        match case.argv0:
            case None:
                argv0 = os.fsencode(case.prog)
            case str() if case.argv0 == FULL_PATH:
                argv0 = exe
            case _:
                argv0 = os.fsencode(case.argv0)
        root = os.fsencode(self.root)
        args = [os.fsencode(a).replace(b"{ROOT}", root) for a in case.args]
        return exe, [argv0, *args]

    def run(self, case: Case) -> Outcome:
        exe, argv = self._argv(case)
        profile = _profile(self.box)
        sandbox_init = _sandbox_init()
        master = slave = None
        if case.stdin.kind == "pty":
            master, slave = os.openpty()
            stdin: int = slave
            # Typed before the child exists, while our slave fd holds the pty
            # open: the line discipline queues and echoes it right away, so the
            # echo on the master never depends on how fast the child exits.
            if case.stdin.data:
                os.write(master, case.stdin.data)
        elif case.stdin.kind == "pipe":
            stdin = subprocess.PIPE
        else:
            stdin = subprocess.DEVNULL
        kind = case.stdin.kind

        def preexec() -> None:
            # Runs after setsid(): the pty becomes the controlling terminal, and
            # without a pty there is none, so /dev/tty can never reach the
            # terminal pytest runs in.
            if kind == "pty":
                fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            elif kind == "closed":
                os.close(0)
            err = ctypes.c_char_p()
            if sandbox_init(profile, 0, ctypes.byref(err)) != 0:
                os._exit(SANDBOX_FAILED)

        proc = subprocess.Popen(
            argv,
            executable=exe,
            cwd=self.root / case.cwd,
            env=self._env(case),
            stdin=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
            preexec_fn=preexec,  # noqa: PLW1509 - sandbox_init and TIOCSCTTY must run in the child; the suite is single-threaded
        )
        # Our slave fd stays open until the master is drained: on the last close
        # of the slave, macOS discards the master's unread output (the echo and
        # anything written to /dev/tty), which would make ``tty`` depend on timing.
        try:
            out, err, tty, rc = self._communicate(proc, case.stdin, master)
        finally:
            if slave is not None:
                os.close(slave)
        assert rc != SANDBOX_FAILED or err, "sandbox_init failed in the child"
        return Outcome(argv, rc, out, err, tty, snapshot(self.box))

    @staticmethod
    def _communicate(
        proc: subprocess.Popen[bytes], stdin: Stdin, master: int | None
    ) -> tuple[bytes, bytes, bytes, int | None]:
        if proc.stdin is not None:
            try:
                proc.stdin.write(stdin.data)
            except BrokenPipeError:
                pass
            try:
                proc.stdin.close()
            except BrokenPipeError:
                pass

        bufs: dict[int, bytearray] = {}
        sel = selectors.DefaultSelector()
        assert proc.stdout is not None and proc.stderr is not None
        out_fd, err_fd = proc.stdout.fileno(), proc.stderr.fileno()
        for f in (proc.stdout, proc.stderr):
            os.set_blocking(f.fileno(), False)
            sel.register(f.fileno(), selectors.EVENT_READ)
            bufs[f.fileno()] = bytearray()
        if master is not None:
            os.set_blocking(master, False)
            sel.register(master, selectors.EVENT_READ)
            bufs[master] = bytearray()

        deadline = time.monotonic() + RUN_TIMEOUT
        timed_out = False
        open_pipes = {out_fd, err_fd}
        while open_pipes:
            left = deadline - time.monotonic()
            if left <= 0:
                timed_out = True
                break
            for key, _ in sel.select(timeout=min(left, 0.5)):
                fd = key.fd
                try:
                    chunk = os.read(fd, 65536)
                except BlockingIOError:
                    continue
                except OSError as e:  # EIO: the pty's slave side is gone
                    if e.errno != errno.EIO:
                        raise
                    chunk = b""
                if chunk:
                    bufs[fd] += chunk
                else:
                    sel.unregister(fd)
                    open_pipes.discard(fd)
        if timed_out:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        rc = proc.wait()
        if master is not None:
            # Drain what is left (echo of the typed answers), then let go.
            while True:
                try:
                    chunk = os.read(master, 65536)
                except (BlockingIOError, OSError):
                    break
                if not chunk:
                    break
                bufs[master] += chunk
            os.close(master)
        sel.close()
        proc.stdout.close()
        proc.stderr.close()
        tty = bytes(bufs[master]) if master is not None else b""
        return bytes(bufs[out_fd]), bytes(bufs[err_fd]), tty, None if timed_out else rc

    def outcome(self, target: Path, case: Case) -> Outcome:
        """Build ``case.setup``, run ``target`` on it, snapshot, and tear down."""
        self._point_bin_at(target)
        try:
            self.build(case.setup)
            return self.run(case)
        finally:
            self.teardown()


# --------------------------------------------------------------------------- snapshot


def _kind(mode: int) -> str:
    return {
        stat.S_IFDIR: "dir",
        stat.S_IFREG: "file",
        stat.S_IFLNK: "symlink",
        stat.S_IFIFO: "fifo",
        stat.S_IFSOCK: "socket",
    }.get(stat.S_IFMT(mode), oct(stat.S_IFMT(mode)))


def _flag_names(bits: int) -> str:
    names = [name for bit, name in FLAG_NAMES.items() if bits & bit]
    rest = bits & ~sum(FLAG_NAMES)
    return ",".join(names + ([hex(rest)] if rest else []))


def snapshot(top: Path) -> dict[str, Node]:
    """Every path under ``top`` (``top`` itself is "."), as an identity-free :class:`Node`.

    Modes and flags are recorded first. The walk then clears flags and opens up
    permissions so it can read everything, which is fine because teardown
    follows. It works through directory fds, so it goes past PATH_MAX.
    """
    out: dict[str, Node] = {}
    try:
        st = os.lstat(top)
    except FileNotFoundError:
        return out
    top_dev = st.st_dev
    out["."] = Node(_kind(st.st_mode), oct(stat.S_IMODE(st.st_mode)), _flag_names(st.st_flags))

    def walk(dfd: int, rel: bytes, dev: int) -> None:
        for name in sorted(os.listdir(dfd)):
            if dev != top_dev and name.startswith(HOUSEKEEPING):
                continue
            bname = os.fsencode(name)
            path = os.path.join(rel, bname) if rel else bname
            st = os.lstat(name, dir_fd=dfd)
            kind = _kind(st.st_mode)
            content: bytes | str | None = None
            nlink = None
            if st.st_flags:
                os.lchflags(os.path.join(os.fsencode(top), path), 0)
            if kind == "file":
                nlink = st.st_nlink
                if not st.st_mode & stat.S_IRUSR:
                    os.chmod(name, stat.S_IMODE(st.st_mode) | stat.S_IRUSR, dir_fd=dfd)
                fd = os.open(name, os.O_RDONLY, dir_fd=dfd)
                with os.fdopen(fd, "rb") as f:
                    data = f.read()
                content = data if len(data) <= INLINE_CONTENT else "blake2b:" + hashlib.blake2b(data).hexdigest()
            elif kind == "symlink":
                content = os.readlink(bname, dir_fd=dfd)
            out[os.fsdecode(path)] = Node(
                kind,
                oct(stat.S_IMODE(st.st_mode)),
                _flag_names(st.st_flags),
                content,
                nlink,
                st.st_dev != dev,
            )
            if kind == "dir":
                if stat.S_IMODE(st.st_mode) & 0o700 != 0o700:
                    os.chmod(name, stat.S_IMODE(st.st_mode) | 0o700, dir_fd=dfd)
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


# --------------------------------------------------------------------------- compare


def _byte_diff(name: str, ref: bytes, got: bytes) -> list[str]:
    def lines(b: bytes) -> list[str]:
        return [repr(ln) for ln in b.splitlines(keepends=True)] or ["b''"]

    diff = difflib.unified_diff(lines(ref), lines(got), f"{name} (rm)", f"{name} (remmy)", lineterm="", n=2)
    return [f"{name} differs:", *(f"    {ln}" for ln in list(diff)[:80])]


def _tree_diff(ref: dict[str, Node], got: dict[str, Node]) -> list[str]:
    only_ref = sorted(ref.keys() - got.keys())
    only_got = sorted(got.keys() - ref.keys())
    changed = sorted(k for k in ref.keys() & got.keys() if ref[k] != got[k])
    out = ["tree differs:"]
    out += [f"    only rm left:    {k!r} {ref[k]}" for k in only_ref[:30]]
    out += [f"    only remmy left: {k!r} {got[k]}" for k in only_got[:30]]
    for k in changed[:30]:
        out += [f"    {k!r}:", f"        rm:    {ref[k]}", f"        remmy: {got[k]}"]
    return out


def mismatches(ref: Outcome, got: Outcome) -> list[str]:
    out: list[str] = []
    if ref.returncode != got.returncode:
        show = {None: "killed after timeout"}
        out.append(
            f"exit code: rm {show.get(ref.returncode, ref.returncode)}, remmy {show.get(got.returncode, got.returncode)}"
        )
    for name in ("stdout", "stderr", "tty"):
        a, b = getattr(ref, name), getattr(got, name)
        if a != b:
            out += _byte_diff(name, a, b)
    if ref.tree != got.tree:
        out += _tree_diff(ref.tree, got.tree)
    return out


def assert_parity(case: Case, remmy_bin: Path, tmp_path: Path, *, is_root: bool = False) -> None:
    """Run ``case`` under /bin/rm and then under ``remmy_bin``; every difference fails the test."""
    if not os.access(RM, os.X_OK):
        pytest.skip(f"{RM} not available")
    if is_root and case.needs_unprivileged:
        pytest.skip("root bypasses modes and file flags")
    p = Parity(tmp_path, remmy_bin)
    try:
        ref = p.outcome(RM, case)
        assert ref.returncode is not None, f"/bin/rm itself timed out on {case.id}; the case is broken"
        got = p.outcome(remmy_bin, case)
    finally:
        p.close()
    problems = mismatches(ref, got)
    if problems:
        header = [
            f"remmy differs from /bin/rm on case {case.id!r}",
            f"  argv:  {[os.fsdecode(a) for a in ref.argv]!r} (exe {p.bin / case.prog})",
            f"  cwd:   {p.root / case.cwd}",
            f"  stdin: {case.stdin}",
            *([f"  env:   {dict(case.env)}"] if case.env else []),
        ]
        pytest.fail("\n".join(header + problems), pytrace=False)
