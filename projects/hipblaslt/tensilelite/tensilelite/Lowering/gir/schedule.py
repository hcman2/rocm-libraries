# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The facts published once a GIR builder has finished.

The Program is mutable only while the finalizer stages run; ``build_program`` freezes it.
Action ids, the emit plan, storage accesses, the frame contract and each
action's dump line are produced together here, and both the rocisa adapter and
StinkyTofu read this object.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from types import MappingProxyType

from .emit_plan import plan_program
from .frame_contract import build_contract
from .render import inst_line, render_gir
from .storage import storage_accesses


@dataclass(frozen=True)
class FinalizedSchedule:
    """Immutable projection of one builder run.

    No field refers to a GIR node. ``plans`` maps each phase to a tuple of
    actions whose ``source`` is cleared; ``lines`` keeps the dump line each
    action quotes in the assembly. ``accesses`` maps action ids to their
    storage touches, and ``contract.accesses`` is the same data flattened.
    """

    version: int
    plans: object
    contract: object
    action_ids: tuple
    accesses: object
    lines: object
    #: The GIR dump, when the caller asked for it (OutputLoopIR, tests).
    gir_text: object = None

    def accesses_of(self, action_id):
        return self.accesses.get(action_id, ())


def finalize(prog, with_text=False) -> FinalizedSchedule:
    """Plan a frozen ``prog`` once and attach the immutable snapshot to it."""
    if not prog.frozen:
        raise RuntimeError("GIR finalize: the finalizer has not finished this program")
    plans = plan_program(prog)
    accesses = storage_accesses(prog, plans)
    contract = build_contract(prog, plans=plans, accesses=accesses)
    lines = MappingProxyType({
        action.action_id: (inst_line(action.source) if action.source is not None
                           else str(action.kind))
        for actions in plans.values() for action in actions})
    published = MappingProxyType({
        phase: tuple(replace(action, source=None) for action in actions)
        for phase, actions in plans.items()})
    schedule = FinalizedSchedule(
        version=prog.version,
        plans=published,
        contract=contract,
        action_ids=tuple(action.action_id for actions in published.values()
                         for action in actions),
        accesses=accesses,
        lines=lines,
        gir_text=render_gir(prog) if with_text else None,
    )
    prog.finalized = schedule
    return schedule


def published_schedule(prog) -> FinalizedSchedule:
    """The schedule of a frozen program, built on the first request."""
    schedule = getattr(prog, "finalized", None)
    if schedule is not None and schedule.version == prog.version:
        return schedule
    return finalize(prog)
