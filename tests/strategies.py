"""Hypothesis strategies for file names and directory trees.

Strategies are built for a filesystem's capabilities (``FsCaps``); the
module-level ``names`` and ``trees`` target the default: APFS.
"""

from __future__ import annotations

import unicodedata
from collections.abc import Iterator
from dataclasses import dataclass

import fstree
from fstree import Special, Symlink
from hypothesis import event, target
from hypothesis import strategies as st

NAME_MAX = 255

# FAT and exFAT reject these in names, on top of control characters.
_DOS_FORBIDDEN = '"*:<>?\\|'


@dataclass(frozen=True)
class FsCaps:
    """What a filesystem can store, as far as the generators are concerned."""

    symlinks: bool = True
    fifos: bool = True
    dos_names: bool = False
    # Only names already in NFD. macOS's exFAT driver lists NFC names as NFD and
    # then cannot unlink the listed name (ENOENT), which breaks every tree walker.
    nfd_names: bool = False


APFS = FsCaps()


def fs_key(name: str) -> str:
    """A key equal for names any supported filesystem may treat as the same.

    Folds case and Unicode normalization, as APFS, HFS+ and FAT do. On
    case-sensitive volumes this only makes generated names more distinct.
    """
    return unicodedata.normalize("NFD", unicodedata.normalize("NFD", name).casefold())


def _valid_name(caps: FsCaps, name: str) -> bool:
    if name in (".", "..") or len(name.encode()) > NAME_MAX:
        return False
    if caps.dos_names:
        # Trailing dots and spaces are stripped, so the name would change.
        return name[-1] not in ". " and not any(c in _DOS_FORBIDDEN or c < " " for c in name)
    return True


def names_for(caps: FsCaps) -> st.SearchStrategy[str]:
    """Any single path component the filesystem accepts."""
    text = st.text(
        alphabet=st.characters(
            # Lone surrogates are not UTF-8; APFS rejects unassigned code points.
            blacklist_categories=("Cs", "Cn"),
            blacklist_characters="/\0",
        ),
        min_size=1,
        max_size=40,
    )
    if caps.nfd_names:
        # Convert rather than filter: most generated text is not already NFD.
        text = text.map(lambda n: unicodedata.normalize("NFD", n))
    return text.filter(lambda n: _valid_name(caps, n))


def trees_for(caps: FsCaps) -> st.SearchStrategy[fstree.Spec]:
    """A directory spec: nested dicts of files, symlinks, FIFOs, and subdirectories."""
    names = names_for(caps)
    contents = st.one_of(st.text(max_size=64), st.binary(max_size=256))
    kinds: list[st.SearchStrategy[object]] = [contents]
    if caps.symlinks:
        targets = st.one_of(
            st.sampled_from([".", "..", "../..", "/", "missing"]),
            names,
            st.lists(names, min_size=1, max_size=4).map("/".join),
        )
        kinds.append(targets.map(Symlink))
    if caps.fifos:
        kinds.append(st.just(Special.FIFO))
    # No sockets: they bind by name and sun_path is ~104 bytes, far below NAME_MAX.
    leaves = st.one_of(kinds)

    def unique_dict(values: st.SearchStrategy[object]) -> st.SearchStrategy[fstree.Spec]:
        return st.lists(st.tuples(names, values), max_size=6, unique_by=lambda kv: fs_key(kv[0])).map(dict)

    return st.recursive(
        unique_dict(leaves),
        lambda children: unique_dict(st.one_of(leaves, children)),
        max_leaves=60,
    )


names = names_for(APFS)
trees = trees_for(APFS)


def dir_paths(spec: fstree.Spec, prefix: str = "") -> Iterator[str]:
    """Every directory in ``spec``, parents before children."""
    for name, node in spec.items():
        if isinstance(node, dict):
            path = f"{prefix}{name}"
            yield path
            yield from dir_paths(node, f"{path}/")


def bucket(n: int) -> str:
    """Coarse size label for ``hypothesis.event``: 0, 1, 2-4, 5-16, 17+."""
    for hi, label in ((0, "0"), (1, "1"), (4, "2-4"), (16, "5-16")):
        if n <= hi:
            return label
    return "17+"


def describe_tree(spec: fstree.Spec) -> None:
    """Report a tree's shape to Hypothesis: statistics plus search targets.

    Targets push generation toward what stresses remmy: deeper nesting (longer
    parent chains, deferred rmdir) and more directories (more scheduled tasks).
    """
    depth, dirs, _ = tree_stats(spec)
    event(f"tree depth: {bucket(depth)}")
    event(f"tree directories: {bucket(dirs)}")
    target(float(depth), label="tree depth")
    target(float(dirs), label="tree directories")


def tree_stats(spec: fstree.Spec) -> tuple[int, int, int]:
    """``(depth, directories, entries)`` of a spec, for Hypothesis targeting."""
    depth = dirs = entries = 0
    for node in spec.values():
        entries += 1
        if isinstance(node, dict):
            d, n, e = tree_stats(node)
            depth = max(depth, d + 1)
            dirs += n + 1
            entries += e
    return depth, dirs, entries
