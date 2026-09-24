"""Open remmy bugs the suite has found, as reusable xfail markers.

Strict markers turn into failures (XPASS) once the bug is fixed, so the marker
cannot outlive the bug. Delete the marker, not the test.
"""

from __future__ import annotations

import pytest

PATH_MAX_EXCEEDED = pytest.mark.xfail(
    strict=True,
    reason="directories are removed by full path (rmdir), which fails with "
    "ENAMETOOLONG once the tree is deeper than PATH_MAX",
)

GIVES_UP_ON_EMFILE_UNDER_CONTENTION = pytest.mark.xfail(
    strict=False,
    reason="with more workers than free descriptors, a worker can see Pool::Live() == 0 "
    "and give up on EMFILE instead of parking the directory (racy)",
)
