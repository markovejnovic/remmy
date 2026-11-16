"""-i over whole trees, at every thread count: what each answer removes, and what an unanswered prompt costs.

remmy asks in a different order than rm's depth-first walk (README: Output and Prompt Ordering), so these check the
guarantees it keeps instead: an answer applies to the prompt printed just before it, a directory's examine prompt
comes before every prompt inside it and its remove prompt after them, and exactly what was answered yes is removed.
"""

import os
import re
import select
import signal
import subprocess
import time
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
