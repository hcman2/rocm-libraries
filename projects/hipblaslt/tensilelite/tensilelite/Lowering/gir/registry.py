# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The one kind table.

Each mark kind binds its payload, required fields, extra verifier and the
action it plans to. Each action kind binds its class, payload, adapter method,
whether it touches shared storage and the StinkyTofu action class. The tables
are checked for completeness at import, so adding a kind without a handler
fails before anything runs.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Optional

from . import actions as A
from . import kinds as K
from .kinds import EmitKind, MarkKind, MoveOp


def _verify_swap(inst, blk, have):
    # A read swap moves an operand's LocalReadAddr (`operand`); a copy swap
    # moves a Phi movement's descriptor (`unit`).
    key = "operand" if inst.at.get("hop") == MoveOp.DS_LOAD else "unit"
    if key not in have:
        raise RuntimeError(
            f"G-MARK: Mark('swap', hop={inst.at.get('hop')!r}) in {blk.label} must name "
            f"its pointer with '{key}' (a read swap names an operand, a copy swap names "
            f"the Phi movement's member tuple); got fields {sorted(have)}")


@dataclass(frozen=True)
class MarkSpec:
    payload: type
    fields: tuple
    # The action this mark plans to. ``None`` is a placement fact with no
    # instruction: phase boundaries, and pins that only constrain an increment.
    emits: Optional[EmitKind] = None
    verify: Optional[Callable] = None


@dataclass(frozen=True)
class ActionSpec:
    action: type
    payload: type
    # Method on the rocisa adapter. ``None`` emits no instruction.
    method: Optional[str]
    # True: the action's shared-memory accesses are its source Move's shared
    # refs. False: the action is intentionally access-less.
    touches_shared: bool
    # GirActionKind member name the adapter stamps on each instruction.
    stinky: str = "Other"


MARK_SPECS = {
    MarkKind.PHASE_BOUNDARY: MarkSpec(K.PhaseBoundaryPayload, ("phase",)),
    MarkKind.SWAP: MarkSpec(K.SwapPayload, ("hop", "gen_from", "gen_to"),
                            EmitKind.SWAP, _verify_swap),
    # One summation-chunk step of the global address. Distinct from region_inc,
    # which steps the TDM descriptor inside one chunk.
    MarkKind.GR_INCREMENT: MarkSpec(K.GrIncrementPayload, ("unit",), EmitKind.GR_INC),
    MarkKind.REGION_INCREMENT: MarkSpec(
        K.RegionIncrementPayload, ("unit", "steps", "from_region", "to_region"),
        EmitKind.REGION_INC),
    MarkKind.DESCRIPTOR_ENABLE: MarkSpec(
        K.DescriptorEnablePayload, ("unit", "member", "enabled"), EmitKind.DESC_ENABLE),
    # The L2 warm-up pair names no operand; the position is the content.
    MarkKind.GL2_PREFETCH: MarkSpec(K.Gl2PrefetchPayload, ("depth", "block"),
                                    EmitKind.GL2_PREFETCH),
    MarkKind.CHUNK_PIN: MarkSpec(K.ChunkPinPayload, ("unit", "chunk")),
    MarkKind.BUFFER_PIN: MarkSpec(K.BufferPinPayload, ("hop", "unit", "gen")),
}

ACTION_SPECS = {
    EmitKind.DS_LOAD: ActionSpec(A.ReadAction, K.ReadPayload, "_emit_planned_fill", True, "Ds"),
    # A DirectToVgpr read names no LDS instruction, so it is `Other`.
    EmitKind.GLOBAL_LOAD: ActionSpec(A.ReadAction, K.ReadPayload, "_emit_planned_fill", True),
    EmitKind.TENSOR_LOAD: ActionSpec(A.CopyAction, K.CopyPayload, "_emit_planned_tensor_load",
                                     True, "Tensor"),
    EmitKind.STORE: ActionSpec(A.StoreAction, K.Payload, None, True),
    EmitKind.WMMA: ActionSpec(A.WmmaAction, K.WmmaPayload, "_emit_planned_wmma", False, "Wmma"),
    EmitKind.SWAP: ActionSpec(A.SwapAction, K.SwapPayload, "_emit_planned_swap", False),
    EmitKind.GR_INC: ActionSpec(A.GrIncAction, K.GrIncrementPayload, "_emit_planned_gr_inc",
                                False),
    EmitKind.REGION_INC: ActionSpec(A.RegionIncAction, K.RegionIncrementPayload,
                                    "_emit_planned_region_inc", False),
    EmitKind.DESC_ENABLE: ActionSpec(A.DescEnableAction, K.DescriptorEnablePayload,
                                     "_emit_planned_desc_enable", False),
    EmitKind.GL2_PREFETCH: ActionSpec(A.Gl2PrefetchAction, K.Gl2PrefetchPayload,
                                      "_emit_planned_gl2", False),
}

#: Move ops whose action lands in registers (includes the DirectToVgpr hop).
FILL_OPS = tuple(kind for kind, spec in ACTION_SPECS.items() if spec.action is A.ReadAction)


def _check_complete():
    missing = set(MarkKind) - set(MARK_SPECS)
    if missing:
        raise RuntimeError("GIR registry: mark kinds without a spec: %s" % sorted(missing))
    missing = set(EmitKind) - set(ACTION_SPECS)
    if missing:
        raise RuntimeError("GIR registry: action kinds without a spec: %s" % sorted(missing))
    for kind, spec in MARK_SPECS.items():
        if spec.emits is not None and spec.emits not in ACTION_SPECS:
            raise RuntimeError("GIR registry: %s plans to unknown action %r" % (kind, spec.emits))
    for op in MoveOp:
        if EmitKind(op.value) not in ACTION_SPECS:
            raise RuntimeError("GIR registry: move op %s has no action" % op)


_check_complete()


def mark_spec(kind):
    """The spec of a known mark kind, or None for a spelling the verifier must reject."""
    return MARK_SPECS.get(kind)


def action_spec(kind):
    try:
        return ACTION_SPECS[kind]
    except KeyError:
        raise KeyError("GIR registry: no action spec for kind %r" % (kind,)) from None


def payload_for(kind, data):
    """The frozen payload of a mark or action ``kind``; unknown kinds get a plain payload."""
    # Mark and action kinds share spellings (``swap``), so pick the table by type.
    if isinstance(kind, MarkKind):
        spec = MARK_SPECS[kind]
    elif isinstance(kind, EmitKind):
        spec = ACTION_SPECS[kind]
    else:
        spec = None
    return K.freeze_payload(spec.payload if spec is not None else K.Payload, data)
