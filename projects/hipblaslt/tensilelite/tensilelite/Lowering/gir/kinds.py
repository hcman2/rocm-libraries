# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Typed GIR kinds and payloads.

The enum *values* are the strings the dump and the assembly comments already
print. ``str`` formatting and ``==`` against those strings stay valid, so a
typed kind does not change rendered text.
"""

from __future__ import annotations

from enum import StrEnum


class MoveOp(StrEnum):
    TENSOR_LOAD = "tensor_load"
    DS_LOAD = "ds_load"
    GLOBAL_LOAD = "global_load"
    STORE = "store"


class MarkKind(StrEnum):
    PHASE_BOUNDARY = "phase_boundary"
    SWAP = "swap"
    GR_INCREMENT = "gr_increment"
    REGION_INCREMENT = "region_increment"
    DESCRIPTOR_ENABLE = "descriptor_enable"
    GL2_PREFETCH = "gl2_prefetch"
    CHUNK_PIN = "chunk_pin"
    BUFFER_PIN = "buffer_pin"


class EmitKind(StrEnum):
    TENSOR_LOAD = "tensor_load"
    DS_LOAD = "ds_load"
    GLOBAL_LOAD = "global_load"
    STORE = "store"
    WMMA = "wmma"
    SWAP = "swap"
    GR_INC = "gr_inc"
    REGION_INC = "region_inc"
    DESC_ENABLE = "desc_enable"
    GL2_PREFETCH = "gl2_prefetch"


def _coerce(enum, value):
    """Return the enum member for a known value, or the original value if it is not one.

    Unknown spellings stay strings so an existing verifier can still name them.
    """
    if isinstance(value, enum) or not value:
        return value
    try:
        return enum(value)
    except ValueError:
        return value


def coerce_move(op):
    return _coerce(MoveOp, op)


def coerce_mark(kind):
    return _coerce(MarkKind, kind)


def coerce_emit(kind):
    return _coerce(EmitKind, kind)


class Payload(dict):
    """A kind's payload.

    It is a ``dict`` so existing ``isinstance(..., dict)`` checks, key order and
    ``repr`` stay exactly the dump's. Subclasses add typed attribute access for
    the fields that kind owns; they do not reorder or drop keys.
    """

    def __init__(self, data=None, **kwargs):
        super().__init__()
        if isinstance(data, Payload):
            data = dict(data)
        if data:
            self.update(data)
        if kwargs:
            self.update(kwargs)


def _view(name, fields):
    """A Payload subclass with one read-only attribute per known field."""
    attrs = {
        field: property(lambda self, field=field: self[field])
        for field in fields
    }
    attrs["__slots__"] = ()
    return type(name, (Payload,), attrs)


PhaseBoundaryPayload = _view("PhaseBoundaryPayload", ("phase",))
SwapPayload = _view("SwapPayload", ("hop", "gen_from", "gen_to", "steps"))
GrIncrementPayload = _view("GrIncrementPayload", ("unit", "chunks", "to_chunk"))
RegionIncrementPayload = _view(
    "RegionIncrementPayload", ("unit", "steps", "from_region", "to_region", "axis_steps"))
DescriptorEnablePayload = _view("DescriptorEnablePayload", ("unit", "member", "enabled"))
Gl2PrefetchPayload = _view("Gl2PrefetchPayload", ("depth", "block"))
ChunkPinPayload = _view("ChunkPinPayload", ("unit", "chunk"))
BufferPinPayload = _view("BufferPinPayload", ("hop", "unit", "gen"))
ReadPayload = _view(
    "ReadPayload",
    ("tc", "tile", "k", "k_flat", "reg_buf", "region", "address_region", "regions",
     "tile_flat", "fills", "group", "size_regs", "unit_index", "unit_indexes",
     "issue", "gen", "advance"))
CopyPayload = _view("CopyPayload", ("unit", "gen", "regions", "region"))
WmmaPayload = _view(
    "WmmaPayload",
    ("idx0", "idx1", "u", "bufA", "bufB", "sizeA", "sizeB", "groupA", "groupB",
     "unitA", "unitB", "bufMXA", "bufMXB", "groupMXA", "groupMXB", "unitMXA",
     "unitMXB", "srcs", "source_issues"))


_MARK_PAYLOAD = {
    MarkKind.PHASE_BOUNDARY: PhaseBoundaryPayload,
    MarkKind.SWAP: SwapPayload,
    MarkKind.GR_INCREMENT: GrIncrementPayload,
    MarkKind.REGION_INCREMENT: RegionIncrementPayload,
    MarkKind.DESCRIPTOR_ENABLE: DescriptorEnablePayload,
    MarkKind.GL2_PREFETCH: Gl2PrefetchPayload,
    MarkKind.CHUNK_PIN: ChunkPinPayload,
    MarkKind.BUFFER_PIN: BufferPinPayload,
}

_EMIT_PAYLOAD = {
    EmitKind.DS_LOAD: ReadPayload,
    EmitKind.GLOBAL_LOAD: ReadPayload,
    EmitKind.TENSOR_LOAD: CopyPayload,
    EmitKind.WMMA: WmmaPayload,
    EmitKind.SWAP: SwapPayload,
    EmitKind.GR_INC: GrIncrementPayload,
    EmitKind.REGION_INC: RegionIncrementPayload,
    EmitKind.DESC_ENABLE: DescriptorEnablePayload,
    EmitKind.GL2_PREFETCH: Gl2PrefetchPayload,
}


def payload_for(kind, data):
    """Wrap ``data`` in the payload type for ``kind``, preserving key order."""
    cls = _MARK_PAYLOAD.get(kind) or _EMIT_PAYLOAD.get(kind) or Payload
    if isinstance(data, cls):
        return data
    return cls(dict(data or {}))
