# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""LoopIR to a FinalizedSchedule.

``finalize_loop_schedule`` lowers LoopIR into a working Program, runs the fixed
stage sequence on it, verifies, freezes and publishes the schedule. The
working Program never leaves this module mutable.

Stages, in order:

* ``cfg``: fold the short arm, add early exits, split the prefetch guard and
  label the terminators. The only stage that changes the CFG.
* ``phi``: make every loop-entry frame an explicit phi input.
* ``copy_hoist``: move copies and reads to their hazard-legal slots.
* ``manual``: optional GIRScheduleDumpPath/LoadPath hook.
* ``mark_placement``: collect, place and apply the semantic marks.
* ``diagnostics``: record what model-only blocks discard.
"""

from __future__ import annotations

from .analysis import AnalysisManager
from .passes.collect_pending import CollectPendingMarksPass
from .passes.entrance_frames import EntranceFramePhisPass
from .passes.hoist_copies import HoistCopiesPass
from .passes.placement import PlacementPass
from .passes.apply_marks import ApplyMarksPass
from .passes.record_unemitted import RecordUnemittedPass
from .passes.scaffold_shape import ScaffoldShapePass
from .schedule import finalize
from .verify import verify_gir


def _cfg(prog, am):
    ScaffoldShapePass().run(prog, am)


def _phi(prog, am):
    EntranceFramePhisPass().run(prog, am)


def _copy_hoist(prog, am):
    HoistCopiesPass(reads=True).run(prog, am)


def _mark_placement(prog, am):
    for step in (CollectPendingMarksPass(), PlacementPass(), ApplyMarksPass()):
        _run(step.run, prog, am)


def _diagnostics(prog, am):
    RecordUnemittedPass().run(prog, am)


STAGES = (
    ("cfg", _cfg),
    ("phi", _phi),
    ("copy_hoist", _copy_hoist),
    ("manual", None),
    ("mark_placement", _mark_placement),
    ("diagnostics", _diagnostics),
)


def _run(stage, prog, am):
    before = prog.version
    stage(prog, am)
    # Only a mutation drops the analysis cache, and `bump()` is what declares one.
    if prog.version != before:
        am.invalidate()


def stages(manual=None):
    """The `(name, stage)` sequence; ``manual`` fills the optional schedule hook."""
    return tuple((name, manual if name == "manual" else stage)
                 for name, stage in STAGES
                 if name != "manual" or manual is not None)


def build_program(prog, *, manual=None, verify=True, until=None):
    """Run the stages on a lowered working ``prog`` and freeze it. Returns ``prog``.

    ``until`` stops after the named stage, for tests that inspect a prefix.
    """
    if prog.frozen:
        raise RuntimeError("GIR finalizer: the program is already finalized")
    am = AnalysisManager()
    for name, stage in stages(manual):
        _run(stage, prog, am)
        if name == until:
            break
    if verify:
        verify_gir(prog)
    return prog.freeze()


def finalize_program(prog, *, manual=None, verify=True, with_text=False):
    """Run the stages on a lowered working ``prog`` and publish its schedule."""
    return finalize(build_program(prog, manual=manual, verify=verify), with_text=with_text)


def finalize_loop_schedule(loopir, profile=None, *, include_dependencies=True, params=None,
                           meta=None, manual=None, verify=True, with_text=False):
    """LoopIR -> FinalizedSchedule.

    ``params`` and ``meta`` are target facts the kernel adds to the lowered
    program before the stages run (for example PrefetchGL2 and the VALU
    allocation).
    """
    from ..loopir_to_gir import lower_to_gir

    prog = lower_to_gir(loopir, profile, include_dependencies=include_dependencies,
                        verify=False)
    prog.params.update(params or {})
    prog.meta.update(meta or {})
    return finalize_program(prog, manual=manual, verify=verify, with_text=with_text)
