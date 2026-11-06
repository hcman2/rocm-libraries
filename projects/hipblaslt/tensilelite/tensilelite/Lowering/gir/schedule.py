# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The facts published once a GIR builder has finished.

The mutable Program is the builder. Action ids, the emit plan, storage accesses
and the frame contract are produced together here, and both the rocisa adapter
and StinkyTofu read this object.
"""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType

from .emit_plan import plan_program
from .frame_contract import build_contract


def _snapshot_plans(plans):
    """A mapping of phases to action tuples.

    The tuples do not share the lists ``plan_program`` returns, so appending
    to those lists cannot change the published order or the ids.
    """
    return MappingProxyType({
        phase: tuple(actions) for phase, actions in plans.items()
    })


@dataclass(frozen=True)
class FinalizedSchedule:
    """Immutable projection of one builder run.

    This does not hold the mutable Program. ``plans`` is a mapping proxy of
    action tuples, ``contract`` is frozen, and each action payload is frozen.
    """

    version: int
    plans: object
    contract: object
    action_ids: tuple
    accesses: tuple


def finalize(prog) -> FinalizedSchedule:
    """Plan ``prog`` once and attach the immutable snapshot to it."""
    plans = plan_program(prog)
    contract = build_contract(prog, plans=plans)
    snapshot = _snapshot_plans(plans)
    schedule = FinalizedSchedule(
        version=prog.version,
        plans=snapshot,
        contract=contract,
        action_ids=tuple(
            action.action_id for actions in snapshot.values() for action in actions),
        accesses=contract.accesses,
    )
    prog.finalized = schedule
    return schedule


def published_schedule(prog) -> FinalizedSchedule:
    """The schedule for this program version, built on the first request."""
    schedule = getattr(prog, "finalized", None)
    if schedule is not None and schedule.version == prog.version:
        return schedule
    return finalize(prog)
