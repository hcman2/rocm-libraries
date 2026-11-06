# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""
The GIR backend: pass ORDER as data, and the driver that runs it.

Everything that only READS the program is an Analysis (under `gir/analyses/`) or a check in
`gir/verify.py`; only the passes listed here may change it.
"""

from __future__ import annotations

from ..analysis import AnalysisManager
from ..verify import verify_gir
from .stages import stage_groups


def pipeline():
    """The backend pass order.

    The cfg stage is the ONLY stage allowed to change the CFG: it merges the correct short-arm
    register order into the prologue/drain shape, proves equivalence, adds early-exit edges, and
    labels the terminators.

    Fences, memory tokens and wait counts are all StinkyTofu's, derived after scheduling from the
    frame contract this pipeline exports; GIR states the dataflow and stops there.
    """
    return [item for _name, group in stage_groups() for item in group]


def run_pipeline(prog, passes=None, verify=True):
    """Run `passes` (default: pipeline()) over `prog`, verify, then freeze it.  Returns `prog`.

    A frozen program is the builder's output: no pass may run on it again.
    """
    if prog.frozen:
        raise RuntimeError("GIR run_pipeline: the program is already finalized")
    passes = passes if passes is not None else pipeline()
    am = AnalysisManager()
    for p in passes:
        before = prog.version
        p.run(prog, am)
        # ONLY a mutation drops the cache, and `bump()` is what declares one.  Clearing
        # unconditionally discarded results a later pass in this same run still wanted.
        if prog.version != before:
            am.invalidate()
    if verify:
        verify_gir(prog)
    return prog.freeze()
