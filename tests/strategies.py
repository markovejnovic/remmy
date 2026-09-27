"""Hypothesis strategies for file names and directory trees (APFS and HFS+ rules)."""

import unicodedata

import fstree
from fstree import Special, Symlink
from hypothesis import event, target
from hypothesis import strategies as st


def fs_key(name: str) -> str:
    """Equal for names a case- and normalization-insensitive filesystem treats as the same."""
    return unicodedata.normalize("NFD", unicodedata.normalize("NFD", name).casefold())


# Any single path component: lone surrogates are not UTF-8, and APFS rejects unassigned code points.
names = st.text(
    st.characters(blacklist_categories=("Cs", "Cn"), blacklist_characters="/\0"), min_size=1, max_size=40
).filter(lambda n: n not in (".", "..") and len(n.encode()) <= 255)

_contents = st.one_of(st.text(max_size=64), st.binary(max_size=256))
_targets = st.one_of(
    st.sampled_from([".", "..", "../..", "/", "missing"]), names, st.lists(names, min_size=1, max_size=4).map("/".join)
)
# No sockets: they bind by name and sun_path is ~104 bytes, far below NAME_MAX.
_leaves = st.one_of(_contents, _targets.map(Symlink), st.just(Special.FIFO))


def _unique_dict(values: st.SearchStrategy) -> st.SearchStrategy[fstree.Spec]:
    return st.lists(st.tuples(names, values), max_size=6, unique_by=lambda kv: fs_key(kv[0])).map(dict)


trees = st.recursive(_unique_dict(_leaves), lambda children: _unique_dict(st.one_of(_leaves, children)), max_leaves=60)


def dir_paths(spec: fstree.Spec, prefix: str = ""):
    """Every directory in ``spec``, parents before children."""
    for name, node in spec.items():
        if isinstance(node, dict):
            yield prefix + name
            yield from dir_paths(node, f"{prefix}{name}/")


def bucket(n: int) -> str:
    """Coarse size label for ``hypothesis.event``."""
    return next((label for hi, label in ((0, "0"), (1, "1"), (4, "2-4"), (16, "5-16")) if n <= hi), "17+")


def tree_stats(spec: fstree.Spec) -> tuple[int, int]:
    """``(depth, directories)`` of a spec."""
    depth = dirs = 0
    for node in spec.values():
        if isinstance(node, dict):
            d, n = tree_stats(node)
            depth, dirs = max(depth, d + 1), dirs + n + 1
    return depth, dirs


def describe_tree(spec: fstree.Spec) -> None:
    """Report shape stats and steer generation toward deeper nesting and more directories (more scheduled tasks)."""
    depth, dirs = tree_stats(spec)
    event(f"tree depth: {bucket(depth)}")
    event(f"tree directories: {bucket(dirs)}")
    target(float(depth), label="tree depth")
    target(float(dirs), label="tree directories")
