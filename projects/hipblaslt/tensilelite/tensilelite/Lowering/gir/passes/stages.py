# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Named stages of the GIR builder.

The flat pipeline is this list in order. A direct finalizer walks the same
stages and can compare each prefix with the flat path.
"""

from __future__ import annotations

from ..stages.cfg import cfg
from ..stages.entrance_frames import entrance_frames
from ..stages.hoist_copies import hoist_copies
from .collect_pending import CollectPendingMarksPass
from .placement import PlacementPass
from .apply_marks import ApplyMarksPass
from .record_unemitted import RecordUnemittedPass


class _Stage:
    """A finalizer stage function in the pass list shape."""

    def __init__(self, fn):
        self.run = fn


def stage_groups():
    """`(name, passes)` in builder order.

    `cfg` is the only stage that changes the CFG. `phi` makes loop-entry frames
    explicit. `copy_hoist` places movements inside the hazard window.
    `mark_placement` collects, places and applies the semantic marks.
    """
    return (
        ("cfg", (_Stage(cfg),)),
        ("phi", (_Stage(entrance_frames),)),
        ("copy_hoist", (_Stage(hoist_copies),)),
        ("mark_placement", (
            CollectPendingMarksPass(),
            PlacementPass(),
            ApplyMarksPass(),
            RecordUnemittedPass(),
        )),
    )
