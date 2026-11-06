# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Typed GIR kinds and payloads.

The enum *values* are the strings the dump and the assembly comments already
print. ``str`` formatting and ``==`` against those strings stay valid, so a
typed kind does not change rendered text.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from enum import Enum


class StrEnum(str, Enum):
    """``str`` enum for Python 3.10.

    ``enum.StrEnum`` arrived in 3.11. Members still compare, hash, and format
    as their value, which is the spelling the dump and the assembly comments use.
    """

    def __new__(cls, value):
        member = str.__new__(cls, value)
        member._value_ = value
        return member

    def __str__(self):
        return str.__str__(self)

    def __format__(self, format_spec):
        return str.__format__(self, format_spec)


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


def _freeze(value):
    """A value that cannot be edited after the payload is built.

    Dicts become frozen payloads whose ``repr`` is still the dict spelling.
    Lists become tuples. Tuples are copied so a later edit of the original
    does not show through.
    """
    if isinstance(value, Payload):
        return value
    if isinstance(value, dict):
        return Payload(tuple((key, _freeze(item)) for key, item in value.items()))
    if isinstance(value, list):
        return tuple(_freeze(item) for item in value)
    if isinstance(value, tuple):
        return tuple(_freeze(item) for item in value)
    return value


@dataclass(frozen=True)
class Payload(Mapping):
    """Read-only payload.

    Key order and ``repr`` match the dict the dump already prints. Item
    assignment raises, including for a nested dict that used to be shared.
    """

    _pairs: tuple = ()

    def __getitem__(self, key):
        for name, value in self._pairs:
            if name == key:
                return value
        raise KeyError(key)

    def __iter__(self):
        return (name for name, _value in self._pairs)

    def __len__(self):
        return len(self._pairs)

    def __repr__(self):
        return repr(dict(self._pairs))

    def __str__(self):
        return repr(self)


def _view(name, fields):
    """A Payload subclass with one read-only attribute per known field."""
    return type(name, (Payload,), {
        field: property(lambda self, field=field: self[field])
        for field in fields
    })


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
    """Wrap ``data`` in the frozen payload type for ``kind``, preserving key order."""
    cls = _MARK_PAYLOAD.get(kind) or _EMIT_PAYLOAD.get(kind) or Payload
    if type(data) is cls:
        return data
    pairs = data._pairs if isinstance(data, Payload) else tuple(
        (key, _freeze(value)) for key, value in dict(data or {}).items())
    return cls(pairs)
