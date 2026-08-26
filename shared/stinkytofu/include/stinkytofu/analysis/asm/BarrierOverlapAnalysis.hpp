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

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "stinkytofu/core/AnalysisManager.hpp"

namespace stinkytofu {
class Function;
struct StinkyInstruction;

/// Layer-2 barrier-window overlaps discovered by the CDNA5 DAG scheduler.
///
/// Entries are symmetric. Every member of one signal/wait group is associated
/// with every member of the overlapping group, allowing a later transform to
/// query the result after scheduling has reordered the instructions.
struct BarrierOverlapInfo {
    using BarrierSet = std::unordered_set<const StinkyInstruction*>;
    std::unordered_map<const StinkyInstruction*, BarrierSet> overlaps;

    void recordGroupOverlap(const std::vector<StinkyInstruction*>& lhs,
                            const std::vector<StinkyInstruction*>& rhs) {
        for (const StinkyInstruction* left : lhs) {
            for (const StinkyInstruction* right : rhs) {
                overlaps[left].insert(right);
                overlaps[right].insert(left);
            }
        }
    }

    bool groupsOverlap(const std::vector<StinkyInstruction*>& lhs,
                       const std::vector<StinkyInstruction*>& rhs) const {
        for (const StinkyInstruction* left : lhs) {
            auto it = overlaps.find(left);
            if (it == overlaps.end()) continue;
            for (const StinkyInstruction* right : rhs)
                if (it->second.count(right)) return true;
        }
        return false;
    }
};

/// Scheduler-produced analysis. Its empty run result is useful when requested
/// independently; StinkyDAGSchedulerPass publishes the populated result.
struct BarrierOverlapAnalysis {
    STINKYTOFU_ANALYSIS_KEY("BarrierOverlapAnalysis")

    using Result = BarrierOverlapInfo;

    static Result run(Function&, AnalysisManager&) {
        return {};
    }
};

}  // namespace stinkytofu
