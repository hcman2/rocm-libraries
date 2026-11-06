# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""
The diagnostics stage: count what the model-only blocks are carrying away, LAST.
"""

from __future__ import annotations

from ..kinds import MarkKind
from ..nodes import Mark


def record_unemitted(prog, am):
    """Record the discarded-Mark count for model-only blocks."""
    model_only = [b for b in prog.blocks.values() if b.model_only]
    if not model_only:
        return
    dropped = sum(1 for b in model_only for n in b.body
                  if isinstance(n, Mark) and n.kind != MarkKind.PHASE_BOUNDARY)
    rec = (prog.meta.get("short_loop") or {}).get("model_only")
    if rec is None:
        raise RuntimeError(
            f"blocks {sorted(b.label for b in model_only)} are flagged model_only but no pass "
            f"recorded why -- an unemitted block must be a stated decision, not a flag")
    rec["marks_dropped"] = dropped
