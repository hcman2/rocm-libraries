# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The storage accesses of every planned action, derived once.

The frame contract and the GirActionData stamped on each instruction both read
this projection, so the two cannot disagree about which operand, region,
generation or direction an action touches.
"""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType

from .analysis import AnalysisManager
from .analyses.lds_buffers import LdsBufferIds
from .nodes import Move


@dataclass(frozen=True)
class SharedAccess:
    """One shared-memory touch an action makes."""
    action: int
    is_write: bool
    operand: int               # id of the (operand, region) pair this touch names
    ring: int
    gen: int = -1              # -1 = not bound to a generation
    gdelta: int = 0
    absolute: int = -1         # -1 = the generation is relative, not pinned
    cross_agent: bool = False


def _shared_refs(action):
    """The shared-memory refs one action touches: a destination writes, a source reads."""
    source = action.source
    if not isinstance(source, Move):
        return ()
    return (tuple((ref, True) for ref in source.dsts if ref.tile.space == "shared")
            or tuple((ref, False) for ref in source.srcs if ref.tile.space == "shared"))


def _region_of(operand, regions):
    """The one region a frame instance selects, or -1 for the whole operand.

    `frame_region_instances_of` already enumerates one instance per physical agent, so an instance
    names a single region. A MAY-set would have to widen every consumer, so refuse instead."""
    axes = [tuple(sorted(axis)) for axis in (regions or ())]
    if not axes:
        return -1
    if len(axes) > 1 or len(axes[0]) != 1:
        raise RuntimeError(
            "frame contract: operand %r selects %r, but one instance must name one region. "
            "Widen the contract to a region set rather than dropping the extra values."
            % (operand, axes))
    return int(axes[0][0])


def _gens_by_region(prog):
    """`(operand, region) -> gen id`, the inverse of `generation_regions`."""
    return {(str(fact.get("operand")), int(fact.get("region", 0))): int(gen_id)
            for gen_id, fact in ((prog.meta or {}).get("generation_regions", {}) or {}).items()}


def _storage_id(operand, region, storage_ids):
    """The id of one `(operand, region)` pair, allocated on first sight.

    Whether an operand is region-split is a property of `region_axes`, so a region of -1 and a
    concrete region never name the same operand and an id compared for equality says everything
    the pair did."""
    return storage_ids.setdefault((operand, region), len(storage_ids))


def steady_frame_basis(prog):
    """Chunk-coordinate basis carried by each unrolled steady-copy block."""
    return {
        label: (int(block.gen_rel) if block.gen_rel is not None
                and (label == "steady" or label.startswith("steady")) else 0)
        for label, block in prog.blocks.items()
    }


def _accesses_of(action, storage, distributed, by_region, storage_ids, frame_basis=0):
    """Every shared touch of one action, one `SharedAccess` per frame-region instance.

    An instance names ITS OWN region's generation.  A `Ref` carries one `gen`, so a read spanning
    every region stamped them all with its own; a region whose reader and writer then sat on
    different generations had no rotation between them at all, which reads as a same-trip WAR
    against the write that recycles the very buffer being read."""
    refs = _shared_refs(action)
    if not action.spec.touches_shared:
        if refs:
            raise RuntimeError(
                "GIR storage: %s is declared access-less but its source touches shared memory"
                % action.kind)
        return ()
    out = []
    for ref, is_write in refs:
        operand = str(ref.tile.operand)
        gen = getattr(ref, "gen", None)
        absolute = getattr(ref, "abs_gen", None)
        for regions in storage.geometry.frame_region_instances_of(ref, is_write):
            region = _region_of(operand, regions)
            gen_id = -1 if gen is None else int(gen.id)
            if gen is not None and region >= 0:
                gen_id = by_region.get((operand, region), gen_id)
            out.append(SharedAccess(
                action=action.action_id,
                is_write=is_write,
                operand=_storage_id(operand, region, storage_ids),
                ring=max(1, int(storage.depth_of(ref.tile.operand))),
                gen=gen_id,
                # The serialized frame advances between unrolled steady copies. GIR itself keeps
                # one frame and states that same advance in each Ref's gdelta, so remove the
                # block basis here: concrete generation = (frame+basis) + (gdelta-basis).
                gdelta=int(getattr(ref, "gdelta", 0) or 0) - int(frame_basis),
                absolute=-1 if absolute is None else int(absolute),
                cross_agent=bool(distributed.get(ref.tile.operand))))
    return tuple(out)


def storage_accesses(prog, plans, analyses=None):
    """`{action_id: (SharedAccess, ...)}` for every planned action, in plan order."""
    analyses = analyses or AnalysisManager()
    storage = analyses.get(LdsBufferIds(), prog)
    if storage.unresolved:
        raise RuntimeError("frame contract: unresolved shared references %r"
                           % (storage.unresolved[:4],))
    distributed = prog.meta.get("agent_distributed", {}) or {}
    by_region = _gens_by_region(prog)
    basis = steady_frame_basis(prog)
    storage_ids = {}
    out = {}
    for label, block_actions in plans.items():
        for action in block_actions:
            out[action.action_id] = _accesses_of(
                action, storage, distributed, by_region, storage_ids, basis.get(label, 0))
    return MappingProxyType(out)
