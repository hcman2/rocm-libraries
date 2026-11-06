# Copyright Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
"""Emit the LoopModel prologue, steady body, and drain stages."""

from rocisa.code import Module

from ...Lowering.gir.kinds import EmitKind
from ...Lowering.gir.registry import FILL_OPS
from ...Lowering.gir_to_rocisa import RocisaEmitter

from .Program import loopModelFinalizedSchedule


def _girEmitter(writer, kernel, tPA, tPB, schedule):
  """The ONE emitter for this schedule.

  Construction builds the leaf contexts, which are loop-order invariant, so a
  per-stage rebuild would redo them for every stage.
  """
  cache = getattr(writer, "_loopModelEmitterCache", None)
  if not (cache and cache[0] is schedule and cache[1] is tPA and cache[2] is tPB):
    cache = (schedule, tPA, tPB, RocisaEmitter(writer, kernel, tPA, tPB, schedule))
    writer._loopModelEmitterCache = cache
  return cache[3]


def girEmitStage(writer, kernel, tPA, tPB, phase, *, internalPointerSwap=False, kinds=None):
  """Emit one stage of the kernel's FinalizedSchedule."""
  schedule = loopModelFinalizedSchedule(writer, kernel)
  return _girEmitter(writer, kernel, tPA, tPB, schedule).emit_block(
      phase, internalPointerSwap=internalPointerSwap, kinds=kinds)


def loopModelSteadyBlocks(writer, kernel) -> list:
  """The schedule's steady chain for this kernel, in program order."""
  return list(loopModelFinalizedSchedule(writer, kernel).target.steady_chain)


def loopModelSteadyPhase(writer, kernel, loopCopy) -> str:
  """The chain block this unrolled copy emits.

  ONE block serves every scaffold copy: the copies then differ only in scaffold-owned state,
  which is what ExpandPointerSwap's pair has always done.  A longer chain is GIR's own unroll --
  the copies hold different buffers -- so there the copy index selects the block.
  """
  chain = loopModelSteadyBlocks(writer, kernel)
  if len(chain) <= 1:
    return chain[0] if chain else "steady"
  if not 0 <= int(loopCopy) < len(chain):
    raise RuntimeError(
        "UseLoopModel: unrolled loop copy %s has no GIR steady block -- the chain is %r.  The "
        "scaffold copy count must match LoopIR's derived steady ring period."
        % (loopCopy, chain))
  return chain[int(loopCopy)]


#: The prologue's GLOBAL->SHARED movement, which legacy issues from `setupNewTile`, and the
#: REGISTER fill, which legacy issues at the prefetch-local point.  One GIR block, two sites.
PROLOGUE_MOVEMENT = (EmitKind.TENSOR_LOAD, EmitKind.GR_INC, EmitKind.DESC_ENABLE,
                     EmitKind.REGION_INC, EmitKind.GL2_PREFETCH)
PROLOGUE_FILL = FILL_OPS + (EmitKind.WMMA, EmitKind.SWAP)


def _prologueGuardLadder(writer, kernel, tensorParametersA, tensorParametersB, module):
  """The single-trip skipPGR ladder; it brackets the COPIES, so it follows them to their site."""
  guard = loopModelFinalizedSchedule(writer, kernel).target.prefetch_guard
  if not guard:
    return
  module.add(writer.openPrefetchGlobalRead2orMore(kernel, guard["gen"]))
  module.add(girEmitStage(
      writer, kernel, tensorParametersA, tensorParametersB, guard["block"]))
  for idxPgr in range(0, kernel["PrefetchGlobalRead"] + 1):
    module.add(writer.closePrefetchGlobalRead2orMore(
        kernel, tensorParametersA, tensorParametersB, idxPgr))
    if idxPgr == 1 and guard.get("skip"):
      module.add(girEmitStage(
          writer, kernel, tensorParametersA, tensorParametersB, guard["skip"]))
  module.add(girEmitStage(
      writer, kernel, tensorParametersA, tensorParametersB, guard["join"]))


def loopModelPrologueCopies(writer, kernel, tensorParametersA, tensorParametersB, module):
  """HalfPLR's global prefetch, emitted where the legacy scaffold emits its own.

  HalfPLR is diffed instruction-for-instruction against its legacy twin, and legacy issues this
  from `setupNewTile` -- ~230 lines before the prefetch-local point the rest of the prologue comes
  from, which is a different basic block.  Every other UseLoopModel shape keeps the single site,
  so nothing but HalfPLR moves.
  """
  if kernel.get("UseLoopModel", False) and kernel["HalfPLR"]:
    module.add(girEmitStage(writer, kernel, tensorParametersA, tensorParametersB, "prologue",
                            kinds=PROLOGUE_MOVEMENT))
    _prologueGuardLadder(writer, kernel, tensorParametersA, tensorParametersB, module)


def loopModelPrologue(writer, kernel, tensorParametersA, tensorParametersB, module, pack, packPre):
  """The prologue fork: the PLR fill, plus the copies unless HalfPLR already took them early."""
  # Keep empty pack modules for the steady loop; the drain no longer consumes
  # localReadDo's offset state.
  for plrIdx in range(0, writer.states.numItersPLR):
    packPre[plrIdx] = Module()
    pack[plrIdx] = Module()
  early = bool(kernel["HalfPLR"])
  module.add(girEmitStage(writer, kernel, tensorParametersA, tensorParametersB, "prologue",
                          kinds=PROLOGUE_FILL if early else None))
  if not early:
    _prologueGuardLadder(writer, kernel, tensorParametersA, tensorParametersB, module)
  writer.states.SubTileIdx = (writer.states.SubTileIdx + 1) % kernel["numSubTiles"]


def loopModelSteadyIter(writer, kernel, tensorParametersA, tensorParametersB, module,
                        LoopModelScaffold, u, waitLWCode, syncCode, loopCopy=0):
  """One steady substep.

  The walker unrolls the whole inner loop, so the order-independent scaffold accumulates across
  u and is emitted once, after the unroll, at the last substep.  `loopCopy` selects this copy's
  block in GIR's steady chain: with one copy that is the only block, and nothing changes.
  """
  LoopModelScaffold.add(waitLWCode)
  LoopModelScaffold.add(syncCode)
  _perLW = writer.codes.perIterLocalWrite[u][1] if u < len(writer.codes.perIterLocalWrite) else None
  if _perLW is not None and _perLW.count():
    LoopModelScaffold.add(_perLW)
  if u == kernel["LoopIters"] - 1:
    # Copies, swaps, and GR increments are GIR leaves, just like reads and
    # WMMA operations.
    module.add(girEmitStage(
        writer, kernel, tensorParametersA, tensorParametersB,
        loopModelSteadyPhase(writer, kernel, loopCopy),
        internalPointerSwap=kernel["ExpandPointerSwap"]))
    module.add(LoopModelScaffold)


def loopModelDrainIter(writer, kernel, tensorParametersA, tensorParametersB, module,
                       LoopModelDrainScaffold, u, waitLWCode, syncCode, remainPgr):
  """Emit one drain substep."""
  _lmDrainStep = (kernel["PrefetchGlobalRead"] - 1) - remainPgr
  LoopModelDrainScaffold.add(waitLWCode)
  LoopModelDrainScaffold.add(syncCode)
  # The drain block already carries the copies issued by the peel.
  _perLW = writer.codes.perIterLocalWrite[u][1] \
      if (writer.codes.perIterLocalWrite and u < len(writer.codes.perIterLocalWrite)) else None
  if _perLW is not None and _perLW.count():
    LoopModelDrainScaffold.add(_perLW)
  if u == kernel["LoopIters"] - 1:
    # GIR places the swap and global-read increment for the drain.
    module.add(girEmitStage(
        writer, kernel, tensorParametersA, tensorParametersB, "drain%d" % _lmDrainStep,
        internalPointerSwap=False))
    module.add(LoopModelDrainScaffold)
