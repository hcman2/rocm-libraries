# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""LoopIR to a FinalizedSchedule.

The mutable Program and its passes are the builder. This module runs that
builder and publishes one schedule. The rocisa adapter and the frame-contract
installer read the schedule; they no longer plan the program a second time.

The old ``run_pipeline`` entry stays as the compatibility wrapper the builder
uses. It is not a second lowering.
"""

from __future__ import annotations

from .passes.pipeline import pipeline, run_pipeline
from .passes.stages import stage_groups
from .schedule import finalize


def finalize_program(prog, passes=None, verify=True):
    """Run the builder, then publish its schedule. Returns the schedule."""
    run_pipeline(prog, passes=passes if passes is not None else pipeline(), verify=verify)
    return finalize(prog)


def compare_stage_path(make_program):
    """Check each stage prefix against the flat pipeline.

    ``make_program`` returns a fresh mutable program. Both sides run the same
    pass classes; a mismatch means the stage list and ``pipeline()`` diverged
    or the builder is not deterministic.
    """
    from .render import render_gir

    flat = pipeline()
    staged = [item for _name, group in stage_groups() for item in group]
    if [type(item) for item in flat] != [type(item) for item in staged]:
        raise RuntimeError("pipeline() does not follow the stage list")
    count = 0
    for name, group in stage_groups():
        count += len(group)
        left = make_program()
        right = make_program()
        run_pipeline(left, passes=flat[:count], verify=False)
        run_pipeline(right, passes=staged[:count], verify=False)
        if render_gir(left) != render_gir(right):
            raise RuntimeError(
                "GIR stage %s diverges from the flat pipeline" % name)
    return True
