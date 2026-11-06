# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The mark_placement stage: collect, place and apply the semantic marks.

The region analyses state each mark's legal window as a PendingMark on
`prog.pending`; placement resolves the window to one anchor (policy travels
with the Region, because it differs by what the Mark mutates); apply inserts
the Marks into the bodies. Apply is the only body mutator.
"""

from __future__ import annotations

from ..analyses import (SwapRegions, GrIncrementRegions, RegionIncrementRegions,
                        Gl2PrefetchRegions)
from ..nodes import BLOCK_ENTRY, BLOCK_EXIT, MIDPOINT, region_slot


def _region_analyses():
    #: Gl2PrefetchRegions is inert unless meta['prefetch_gl2'] > 0.
    return (SwapRegions(), GrIncrementRegions(), RegionIncrementRegions(),
            Gl2PrefetchRegions())


def collect_pending(prog, am):
    """Run the region analyses and stash every PendingMark they produce. Mutates no IR."""
    pending = []
    for analysis in _region_analyses():
        pending.extend(am.get(analysis, prog))
    prog.pending = pending


def _resolve(prog, region):
    body = prog.block(region.block).body
    lo = region_slot(body, region.after, True)    # first legal slot
    hi = region_slot(body, region.before, False)  # one past the last legal slot
    if hi < lo:
        return (region.block, region.before)             # malformed window: stated bound
    idx = lo + (hi - lo) // 2 if region.policy == MIDPOINT else lo
    return (region.block, body[idx] if idx < len(body) else BLOCK_EXIT)


def place_marks(prog, am):
    """Resolve each PendingMark's legal window to one concrete anchor. Mutates no IR."""
    for pm in prog.pending:
        pm.anchor = _resolve(prog, pm.region)


def _anchor_index(body, anchor):
    if anchor is BLOCK_EXIT or anchor == BLOCK_EXIT:
        return len(body)
    if anchor is BLOCK_ENTRY or anchor == BLOCK_ENTRY:
        return 0
    for i, node in enumerate(body):
        if node is anchor:
            return i
    # anchor node not found (e.g. its block differs) -> append at end, conservative
    return len(body)


def apply_marks(prog, am):
    """Insert each placed Mark into its block body, back to front so anchors stay valid."""
    by_block = {}
    for pm in prog.pending:
        if pm.anchor is None:
            raise RuntimeError("apply_marks: PendingMark not placed (run place_marks first)")
        block_label, anchor = pm.anchor
        by_block.setdefault(block_label, []).append((anchor, pm.mark))

    for block_label, items in by_block.items():
        body = prog.block(block_label).body
        insertions = [(_anchor_index(body, anchor), mark) for anchor, mark in items]
        for idx, mark in sorted(insertions, key=lambda t: t[0], reverse=True):
            body.insert(idx, mark)

    prog.pending = []
    prog.bump()


STEPS = (collect_pending, place_marks, apply_marks)


def mark_placement(prog, am):
    for step in STEPS:
        before = prog.version
        step(prog, am)
        if prog.version != before:
            am.invalidate()
