# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""The GIR builder stages, run in order by ``gir.finalizer``.

Each stage is ``stage(prog, am)`` on the working Program; only stages mutate
it, and a stage declares a mutation with ``prog.bump()``.
"""
