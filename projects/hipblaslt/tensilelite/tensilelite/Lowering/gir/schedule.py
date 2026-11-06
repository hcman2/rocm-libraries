# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The facts published once a GIR builder has finished.

The mutable Program is the builder. Action ids, the emit plan, storage accesses
and the frame contract are produced together here, and both the rocisa adapter
and StinkyTofu read this object.
"""

from __future__ import annotations

from dataclasses import dataclass

from .emit_plan import plan_program
from .frame_contract import build_contract


@dataclass(frozen=True)
class FinalizedSchedule:
    """One derivation of the emit plan and the frame contract."""

    program: object
    version: int
    plans: dict
    contract: object
    action_ids: tuple
    accesses: tuple


def finalize(prog) -> FinalizedSchedule:
    """Plan ``prog`` once and attach the schedule to it."""
    plans = plan_program(prog)
    contract = build_contract(prog, plans=plans)
    schedule = FinalizedSchedule(
        program=prog,
        version=prog.version,
        plans=plans,
        contract=contract,
        action_ids=tuple(
            action.action_id for actions in plans.values() for action in actions),
        accesses=tuple(contract.accesses),
    )
    prog.finalized = schedule
    return schedule


def published_schedule(prog) -> FinalizedSchedule:
    """The schedule for this program version, built on the first request."""
    schedule = getattr(prog, "finalized", None)
    if schedule is not None and schedule.version == prog.version:
        return schedule
    return finalize(prog)
