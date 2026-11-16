"""-i over whole trees, at every thread count: what each answer removes, and what an unanswered prompt costs.

remmy asks in a different order than rm's depth-first walk (README: Output and Prompt Ordering), so these check the
guarantees it keeps instead: an answer applies to the prompt printed just before it, a directory's examine prompt
comes before every prompt inside it and its remove prompt after them, and exactly what was answered yes is removed.
"""

import os
import re
import resource
import select
import signal
import subprocess
import time
from collections.abc import Callable
from pathlib import Path

import fstree
import pytest
from harness import remmy_env, run_remmy
from hypothesis import given
from hypothesis import strategies as st

# Plain names, so a path can be read back out of the prompts unambiguously.
_names = st.text("abcdefgh", min_size=1, max_size=3)
_trees = st.recursive(
    st.dictionaries(_names, st.just("x"), max_size=4),
    lambda children: st.dictionaries(_names, st.one_of(st.just("x"), children), max_size=4),
    max_leaves=30,
)
_PROMPT = re.compile(r"(remove|examine files in directory) ([a-ht/]+)\? ")


@pytest.fixture(scope="module")
def base(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return tmp_path_factory.mktemp("interactive")


def _paths(spec: fstree.Spec, prefix: str) -> dict[str, bool]:
    """Every path under ``prefix``, mapped to whether it is a directory."""
    out: dict[str, bool] = {}
    for name, node in spec.items():
        path = f"{prefix}/{name}"
        out[path] = isinstance(node, dict)
        if isinstance(node, dict):
            out |= _paths(node, path)
    return out


def _under(path: str, directory: str) -> bool:
    return path.startswith(directory + "/")


@given(tree=_trees, answers=st.lists(st.booleans(), max_size=80), threads=st.integers(1, 16))
def test_each_answer_applies_to_its_prompt(
    remmy_bin: Path, base: Path, tree: fstree.Spec, answers: list[bool], threads: int
) -> None:
    with fstree.scratch(base) as d:
        fstree.build(d, {"t": tree})
        stdin = b"".join(b"y\n" if yes else b"n\n" for yes in answers)
        res = run_remmy(remmy_bin, ["-ir", "t"], cwd=d, threads=threads, input=stdin)

        dirs = {"t": True} | _paths(tree, "t")
        prompts = [(kind, path) for kind, path in _PROMPT.findall(res.stderr)]
        # Past the last answer, stdin is at its end, which is a no.
        said_yes = {(kind, path): i < len(answers) and answers[i] for i, (kind, path) in enumerate(prompts)}
        assert len(said_yes) == len(prompts), f"a prompt was asked twice: {prompts}"

        order = {prompt: i for i, prompt in enumerate(prompts)}
        for (kind, path), i in order.items():
            inside = [order[p] for p in order if _under(p[1], path)]
            if kind == "examine files in directory":
                assert all(j > i for j in inside), f"{path}'s examine came after a prompt inside it: {prompts}"
                if not said_yes[(kind, path)]:
                    assert not inside, f"{path} was declined but its contents were asked about: {prompts}"
            elif dirs[path]:
                assert all(j < i for j in inside), f"{path}'s remove came before a prompt inside it: {prompts}"

        # A path is gone exactly when it was asked about, answered yes, and (a directory) was by then empty.
        def removed(path: str) -> bool:
            if not said_yes.get(("remove", path), False):
                return False
            return not dirs[path] or all(
                removed(p) for p in dirs if _under(p, path) and p.count("/") == path.count("/") + 1
            )

        expected_left = {p for p in dirs if not removed(p)}
        assert {p for p in dirs if fstree.exists(d / p)} == expected_left, res
        refused_rmdir = any(dirs[p] and said_yes.get(("remove", p), False) and p in expected_left for p in dirs)
        assert res.returncode == (1 if refused_rmdir else 0), res


def test_an_unanswered_prompt_uses_no_cpu(remmy_bin: Path, workdir: Path) -> None:
    """While a question waits for an answer, the workers must sleep, not spin: a person may take minutes."""
    fstree.build(workdir, {"d": {f"s{i}": {"f": "x"} for i in range(8)}})
    master, slave = os.openpty()
    # The yes lets the walk start; the next question then waits for an answer that never comes.
    os.write(master, b"y\n")
    proc = subprocess.Popen(
        [str(remmy_bin), "-ir", "d"], cwd=workdir, env=remmy_env(4), stdin=slave, stdout=-1, stderr=-1
    )
    try:
        seen, deadline = b"", time.monotonic() + 10
        while seen.count(b"? ") < 2 and time.monotonic() < deadline:
            if select.select([proc.stderr], [], [], 0.5)[0]:
                seen += os.read(proc.stderr.fileno(), 4096)
        assert seen.count(b"? ") >= 2, f"remmy never asked its second question: {seen!r}"
        time.sleep(1.0)
    finally:
        proc.send_signal(signal.SIGTERM)
        _, _, usage = os.wait4(proc.pid, 0)
        proc.returncode = 0  # reaped by wait4
        os.close(slave)
        os.close(master)
    cpu = usage.ru_utime + usage.ru_stime
    assert cpu < 0.2, f"remmy used {cpu:.2f}s of CPU over a second of waiting for an answer"


def _run_on_pty(
    remmy_bin: Path, args: list[str], cwd: Path, typed: bytes, threads: int, fd_limit: int | None = None
) -> tuple[int, str, str]:
    """Run remmy with a terminal on stdin and ``typed`` already typed into it."""
    master, slave = os.openpty()
    os.write(master, typed)
    limit = (lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))) if fd_limit else None
    try:
        proc = subprocess.run(
            [str(remmy_bin), *args],
            cwd=cwd,
            env=remmy_env(threads),
            stdin=slave,
            capture_output=True,
            timeout=60,
            preexec_fn=limit,
            check=False,
        )
    finally:
        os.close(slave)
        os.close(master)
    return proc.returncode, proc.stdout.decode(), proc.stderr.decode()


@pytest.mark.parametrize("threads", [1, 4, 16])
def test_each_unwritable_file_in_a_tree_is_asked_about_once(remmy_bin: Path, workdir: Path, threads: int) -> None:
    """At a terminal, without -f, every read-only file anywhere in the walk gets one override question."""
    subdirs = [f"s{i}" for i in range(6)]
    fstree.build(workdir, {"d": {s: {"ro": "x", "rw": "x"} for s in subdirs}})
    for s in subdirs:
        os.chmod(workdir / "d" / s / "ro", 0o444)

    rc, out, err = _run_on_pty(remmy_bin, ["-r", "d"], workdir, b"y\n" * len(subdirs), threads)

    asked = re.findall(r"override r--r--r-- \S+/\S+ for (d/s\d/ro)\? ", err)
    assert sorted(asked) == sorted(f"d/{s}/ro" for s in subdirs), err
    assert (rc, out, fstree.exists(workdir / "d")) == (0, "", False), err


def _drive(
    remmy_bin: Path, args: list[str], cwd: Path, steps: list[tuple[bytes, Callable[[], None] | None]], *, tty: bool
) -> tuple[int, str]:
    """Answer yes at each prompt in ``steps``, first running its action, and yes to every later one."""
    master = slave = None
    if tty:
        master, slave = os.openpty()
    stdin = slave if tty else subprocess.PIPE
    proc = subprocess.Popen([str(remmy_bin), *args], cwd=cwd, env=remmy_env(2), stdin=stdin, stderr=subprocess.PIPE)

    def answer(text: bytes) -> None:
        if tty:
            os.write(master, text)
        else:
            proc.stdin.write(text)
            proc.stdin.flush()

    seen = b""
    try:
        for prompt, action in steps:
            deadline = time.monotonic() + 10
            while prompt not in seen and time.monotonic() < deadline:
                if select.select([proc.stderr], [], [], 0.5)[0]:
                    seen += os.read(proc.stderr.fileno(), 4096)
            assert prompt in seen, f"never asked {prompt!r}: {seen!r}"
            if action is not None:
                action()
            answer(b"y\n")
        answer(b"y\n" * 20)
        if not tty:
            proc.stdin.close()
        seen += proc.stderr.read()
        rc = proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        for fd in (slave, master):
            if fd is not None:
                os.close(fd)
    return rc, seen.decode()


def _swap_for_symlink(work: Path) -> None:
    (work / "d/sub").rename(work / "moved")
    (work / "d/sub").symlink_to(work / "victim")


def _swap_for_directory(work: Path) -> None:
    (work / "d/sub").rename(work / "moved")
    (work / "victim").rename(work / "d/sub")


@pytest.mark.parametrize("swap", [_swap_for_symlink, _swap_for_directory], ids=["symlink", "directory"])
@pytest.mark.parametrize(
    ("args", "mode", "at", "tty"),
    [
        (["-ri", "d"], 0o644, b"remove d/sub/file? ", False),
        (["-ri", "d"], 0o644, b"examine files in directory d/sub? ", False),
        (["-r", "d"], 0o444, b"for d/sub/file? ", True),
    ],
    ids=["i-remove", "i-examine", "override"],
)
def test_a_directory_swapped_while_a_prompt_waits_is_not_followed(
    remmy_bin: Path, workdir: Path, swap, args: list[str], mode: int, at: bytes, tty: bool
) -> None:
    """A question can wait for minutes, so whatever a yes removes must still be the entry that was asked about:
    an ancestor renamed away and replaced, by a symlink or by another directory, must not redirect it."""
    fstree.build(workdir, {"d": {"sub": {"file": "mine"}}, "victim": {"file": "victim"}})
    os.chmod(workdir / "d/sub/file", mode)
    os.chmod(workdir / "victim/file", mode)
    victim = workdir / ("d/sub/file" if swap is _swap_for_directory else "victim/file")
    prefix = [(b"examine files in directory d? ", None)] if "-ri" in args else []
    before = [(b"examine files in directory d/sub? ", None)] if at.startswith(b"remove") else []

    rc, err = _drive(remmy_bin, args, workdir, [*prefix, *before, (at, lambda: swap(workdir))], tty=tty)

    assert victim.read_text() == "victim", err
    assert rc == 1, err


def test_a_write_protected_directory_past_path_max_is_asked_about(remmy_bin: Path, workdir: Path) -> None:
    """The override question for a directory deeper than PATH_MAX is still asked, and a no keeps it."""
    name = "d" * 200
    depth = 4096 // (len(name) + 1) + 2
    fstree.deep_chain(workdir / "t", depth, name, leaf_files=0)
    fd = os.open(workdir / "t", os.O_RDONLY | os.O_DIRECTORY)
    try:
        for _ in range(depth - 1):
            fd, parent = os.open(name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd), fd
            os.close(parent)
        os.chmod(name, 0o555, dir_fd=fd)
    finally:
        os.close(fd)

    rc, _, err = _run_on_pty(remmy_bin, ["-r", "t"], workdir, b"n\n", 1)

    assert re.search(r"override r-xr-xr-x \S+/\S+ for t/(d+/)+d+\? ", err), err
    assert (rc, fstree.exists(workdir / "t")) == (1, True), err


@pytest.mark.parametrize("fd_limit", [8, 12])
@pytest.mark.parametrize("override", [False, True], ids=["i", "override"])
def test_a_yes_waits_for_a_free_descriptor(remmy_bin: Path, workdir: Path, fd_limit: int, override: bool) -> None:
    """Short of descriptors, the prompter waits for the walk to free one rather than drop what a yes asked for."""
    spec = {"d": {f"s{i}": {f"t{j}": {"ro": "x"} for j in range(4)} for i in range(30)}}
    fstree.build(workdir, spec)
    for i in range(30):
        for j in range(4):
            os.chmod(workdir / f"d/s{i}/t{j}/ro", 0o444)

    if override:
        rc, _, err = _run_on_pty(remmy_bin, ["-r", "d"], workdir, b"y\n" * 130, 4, fd_limit)
    else:
        res = run_remmy(remmy_bin, ["-ri", "d"], cwd=workdir, threads=4, fd_limit=fd_limit, input=b"y\n" * 500)
        rc, err = res.returncode, res.stderr

    assert (rc, fstree.exists(workdir / "d")) == (0, False), err[-2000:]
