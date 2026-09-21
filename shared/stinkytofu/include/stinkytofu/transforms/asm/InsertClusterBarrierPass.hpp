/* ************************************************************************
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ************************************************************************ */
#pragma once

#include <memory>

#include "stinkytofu/Export.hpp"

namespace stinkytofu {

class Pass;

/// \p streamKMulticast and \p pgrValue only enable the Rule 3 producer-side
/// tensor drain for StreamK cluster multicast at PrefetchGlobalRead >= 2.
/// \p numLdsBuffers controls how many older cooperative tensor-load groups may
/// remain in flight; a group of M loads waits at `(numLdsBuffers - 1) * M`.
/// \p rule3CrossLoop allows Rule 3 signals to cross the loop back edge.
/// \p rule3SignalLeadCycles controls how far ahead of its wait the Rule 3
/// signal is targeted; 0 co-locates them.
STINKYTOFU_EXPORT std::unique_ptr<Pass> createInsertClusterBarrierPass(
    bool streamKMulticast = false, int pgrValue = 1, int rule3SignalLeadCycles = 100,
    int numLdsBuffers = 1, bool rule3CrossLoop = false);

}  // namespace stinkytofu
