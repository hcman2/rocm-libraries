# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The cfg stage: the only stage that changes the CFG.

Fold the short arm, add the early exits, split the prefetch guard, then label
the terminators. The split runs before the labelling so its new guard is named
from the same table as every other terminator.
"""

from __future__ import annotations

from .early_exit import early_exit
from .fold_short_path import fold_short_path
from .scaffold_map import scaffold_map
from .split_prefetch_guard import split_prefetch_guard

STEPS = (fold_short_path, early_exit, split_prefetch_guard, scaffold_map)


def cfg(prog, am):
    for step in STEPS:
        before = prog.version
        step(prog, am)
        if prog.version != before:
            am.invalidate()
