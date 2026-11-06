# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The one handler table for verify, plan and emit.

GIR owns the kind, the payload schema and which mark becomes which action.
The rocisa adapter owns the method that realizes an action. Neither side keeps
a second copy of the kind list.
"""

from __future__ import annotations

from .kinds import EmitKind, MarkKind


# Fields a Mark must carry. A swap additionally names its pointer; which key
# depends on the hop, and that check is the registered verifier below.
MARK_SCHEMA = {
    MarkKind.PHASE_BOUNDARY: ("phase",),
    MarkKind.SWAP: ("hop", "gen_from", "gen_to"),
    MarkKind.GR_INCREMENT: ("unit",),
    MarkKind.REGION_INCREMENT: ("unit", "steps", "from_region", "to_region"),
    MarkKind.DESCRIPTOR_ENABLE: ("unit", "member", "enabled"),
    MarkKind.GL2_PREFETCH: ("depth", "block"),
    MarkKind.CHUNK_PIN: ("unit", "chunk"),
    MarkKind.BUFFER_PIN: ("hop", "unit", "gen"),
}

# Mark kind -> emit kind. ``None`` is a placement fact with no instruction:
# phase boundaries, and the pins that only constrain where an increment goes.
MARK_EMIT = {
    MarkKind.PHASE_BOUNDARY: None,
    MarkKind.SWAP: EmitKind.SWAP,
    # One summation-chunk step of the global address. Distinct from region_inc,
    # which steps the TDM descriptor inside one chunk.
    MarkKind.GR_INCREMENT: EmitKind.GR_INC,
    MarkKind.REGION_INCREMENT: EmitKind.REGION_INC,
    MarkKind.DESCRIPTOR_ENABLE: EmitKind.DESC_ENABLE,
    # The L2 warm-up pair. Unlike every other action it names no operand; the
    # position is the content.
    MarkKind.GL2_PREFETCH: EmitKind.GL2_PREFETCH,
    MarkKind.CHUNK_PIN: None,
    MarkKind.BUFFER_PIN: None,
}

# Emit kind -> method on the rocisa adapter. Fills share one method. Kinds
# with no method (``store``) produce no instruction, matching the previous
# fall-through.
EMIT_METHOD = {
    EmitKind.WMMA: "_emit_planned_wmma",
    EmitKind.DS_LOAD: "_emit_planned_fill",
    EmitKind.GLOBAL_LOAD: "_emit_planned_fill",
    EmitKind.SWAP: "_emit_planned_swap",
    EmitKind.GL2_PREFETCH: "_emit_planned_gl2",
    EmitKind.GR_INC: "_emit_planned_gr_inc",
    EmitKind.TENSOR_LOAD: "_emit_planned_tensor_load",
    EmitKind.DESC_ENABLE: "_emit_planned_desc_enable",
    EmitKind.REGION_INC: "_emit_planned_region_inc",
}

_MARK_VERIFIERS = {}


def register_mark_verifier(kind, fn):
    """Attach an extra check for one mark kind. The schema check always runs."""
    _MARK_VERIFIERS[MarkKind(kind)] = fn


def mark_verifier(kind):
    return _MARK_VERIFIERS.get(kind)


def emit_method(kind):
    return EMIT_METHOD.get(kind)
