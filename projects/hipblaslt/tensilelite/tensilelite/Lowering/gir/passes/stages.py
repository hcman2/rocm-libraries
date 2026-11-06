# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Named stages of the GIR builder.

The flat pipeline is this list in order. A direct finalizer walks the same
stages and can compare each prefix with the flat path.
"""

from __future__ import annotations

from .scaffold_shape import ScaffoldShapePass
from .entrance_frames import EntranceFramePhisPass
from .hoist_copies import HoistCopiesPass
from .collect_pending import CollectPendingMarksPass
from .placement import PlacementPass
from .apply_marks import ApplyMarksPass
from .record_unemitted import RecordUnemittedPass


def stage_groups():
    """`(name, passes)` in builder order.

    `cfg` is the only stage that changes the CFG. `phi` makes loop-entry frames
    explicit. `copy_hoist` places movements inside the hazard window.
    `mark_placement` collects, places and applies the semantic marks.
    """
    return (
        ("cfg", (ScaffoldShapePass(),)),
        ("phi", (EntranceFramePhisPass(),)),
        ("copy_hoist", (HoistCopiesPass(reads=True),)),
        ("mark_placement", (
            CollectPendingMarksPass(),
            PlacementPass(),
            ApplyMarksPass(),
            RecordUnemittedPass(),
        )),
    )
