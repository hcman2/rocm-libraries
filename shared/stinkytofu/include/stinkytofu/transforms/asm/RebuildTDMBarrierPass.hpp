// Copyright Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

#include "stinkytofu/Export.hpp"

namespace stinkytofu {
class Pass;

/// Rebuild workgroup barriers from LDS memory-token read/write transitions.
///
/// The pass expects a CFG and runs before StinkyBuildImplicitDependencyPass.
/// Existing token-managed all-wave barrier pairs are canonicalized away, then
/// regenerated from dataflow across basic blocks. When \p modelLoopBackEdges is
/// false, loop latch-to-header edges do not contribute token state; PGR2+
/// kernels already establish the steady-state LDS phase in their pipelined
/// prologue and intentionally use this mode.
STINKYTOFU_EXPORT std::unique_ptr<Pass> createRebuildTDMBarrierPass(bool modelLoopBackEdges = true);

}  // namespace stinkytofu
