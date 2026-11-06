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

from .analysis import AnalysisManager
from .analyses.reg_band import RegBandAnalysis
from .emit_plan import plan_program
from .frame_contract import build_contract
from .kinds import _freeze
from .nodes import Move
from .render import gir_dump, inst_line
from .storage import storage_accesses
from .verify_dataflow import check_plan

#: Bumped whenever a published field changes meaning.
SCHEDULE_VERSION = 1


def steady_chain(prog) -> list:
    """Return finalized steady GIR blocks in execution order."""
    if "steady" not in prog.blocks:
        return []
    chain = ["steady"]
    while f"steady{len(chain)}" in prog.blocks:
        chain.append(f"steady{len(chain)}")
    return chain


def _register_depth(prog, band):
    """`{operand: W}` -- the register ring width the leaves use for their `m = u % W` FALLBACK, a
    GIR fact (`Ref.reg_ring`, validated by RegBand, B3).

    The fallback only runs when GIR did not supply a per-act slot, which for a GIR-planned wmma or
    read it always does; W is therefore a legacy path, not the authority.
    """
    per_op = {}
    items = band.items()
    if not items:
        items = {}
        for blk in prog.blocks.values():
            for inst in blk.body:
                if isinstance(inst, Move):
                    for ref in inst.dsts:
                        if ref.tile.space == "register" and ref.reg_ring:
                            items[(ref.tile.operand, ref.group)] = ref.reg_ring
    for (op, _group), w in items.items():
        per_op[op] = max(per_op.get(op, 1), int(w))
    return per_op


@dataclass(frozen=True)
class TargetFacts:
    """What the rocisa backend and the kernel scaffold read besides the actions.

    ``drain_steps`` is None when lowering did not state it; readers then use
    ``peel_depth``.
    """

    peel_depth: int
    drain_steps: object
    steady_chain: tuple
    prefetch_guard: object
    unit_member_regions: object
    register_layout: object
    read_coverage: object
    read_fold: object
    coverage_axes: object
    register_depth: object


def _target_facts(prog):
    meta = prog.meta
    band = AnalysisManager().get(RegBandAnalysis(), prog)
    return TargetFacts(
        peel_depth=int(meta.get("peel_depth") or 0),
        drain_steps=meta.get("drain_steps"),
        steady_chain=tuple(steady_chain(prog)),
        prefetch_guard=_freeze(meta.get("prefetch_guard")) or None,
        unit_member_regions=_freeze(dict(meta.get("unit_member_regions", {}) or {})),
        register_layout=_freeze(dict(meta.get("register_layout", {}) or {})),
        read_coverage=_freeze(dict(meta.get("read_coverage", {}) or {})),
        read_fold=_freeze(dict(meta.get("read_fold", {}) or {})),
        coverage_axes=_freeze(dict(meta.get("coverage_axes", {}) or {})),
        register_depth=MappingProxyType(_register_depth(prog, band)),
    )


def _dependency_kinds(prog):
    kinds = {}
    for block in prog.blocks.values():
        for operation in block.body:
            for _endpoint, _counter, kind, _scope in getattr(operation, "deps", ()) or ():
                kinds[kind] = kinds.get(kind, 0) + 1
    return MappingProxyType(kinds)


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
    target: TargetFacts = None
    #: LoopIR dependency kinds carried into the GIR, by count.
    dependency_kinds: object = MappingProxyType({})
    #: `check_plan` findings, when the caller asked for the semantic check.
    violations: tuple = ()
    #: The `.gir` dump, when the caller asked for it (OutputLoopIR, tests).
    gir_text: object = None
    schema: int = SCHEDULE_VERSION

    def accesses_of(self, action_id):
        return self.accesses.get(action_id, ())


def finalize(prog, with_text=False, check=False) -> FinalizedSchedule:
    """Plan a frozen ``prog`` once and return its immutable snapshot."""
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
    return FinalizedSchedule(
        version=prog.version,
        plans=published,
        contract=contract,
        action_ids=tuple(action.action_id for actions in published.values()
                         for action in actions),
        accesses=accesses,
        lines=lines,
        target=_target_facts(prog),
        dependency_kinds=_dependency_kinds(prog),
        violations=tuple(check_plan(prog)) if check else (),
        gir_text=gir_dump(prog) if with_text else None,
    )
