/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#pragma once
#include <Tensile/MasterSolutionLibrary.hpp>
#include <Tensile/SingleSolutionLibrary.hpp>

#include <Tensile/PredictionLibrary.hpp>

namespace TensileLite
{
    namespace Serialization
    {

        template <typename MyProblem, typename MySolution, typename IO>
        struct MappingTraits<ProblemPredictionLibrary<MyProblem, MySolution>, IO>
        {
            using Library = ProblemPredictionLibrary<MyProblem, MySolution>;
            using iot     = IOTraits<IO>;

            static void mapping(IO& io, Library& lib)
            {
                // const bool use_origami   = Debug::Instance().usePredictionSelection() == 0;
                const bool use_formocast = Debug::Instance().usePredictionSelection() == 1;
                lib.predictAlgo = use_formocast ? 1 : 0;

                auto ctx = static_cast<LibraryIOContext<MySolution>*>(iot::getContext(io));
                if(ctx == nullptr)
                {
                    iot::setError(io,
                                  "ProblemPredictionLibrary requires that context be "
                                  "set to a SolutionMap.");
                }

                // Serialize table for Origami
                bool table_empty = false, table_fc_empty = false;
                bool is_out = iot::outputting(io);
                std::vector<int> mappingIndices;
                if(is_out)
                {
                    mappingIndices.reserve(lib.solutionmap.size());

                    for(auto const& pair : lib.solutionmap)
                        mappingIndices.push_back(pair.first);

                    iot::mapRequired(io, "table", mappingIndices);
                }
                else
                {
                    iot::mapRequired(io, "table", mappingIndices);
                    if(mappingIndices.empty())
                        table_empty = true;
                }
                // Serialize table_fc for FormoCast
                std::vector<int> mappingIndices_fc;
                if(is_out)
                {
                    mappingIndices_fc.reserve(lib.solutionmap_fc.size());

                    for(auto const& pair : lib.solutionmap_fc)
                        mappingIndices_fc.push_back(pair.first);

                    iot::mapRequired(io, "table_fc", mappingIndices_fc);
                }
                else
                {
                    iot::mapRequired(io, "table_fc", mappingIndices_fc);
                    if(mappingIndices_fc.empty())
                        table_fc_empty = true;
                }
                if(table_empty && table_fc_empty)
                {
                  iot::setError(io, "ProblemPredictionLibrary has no valid pool");
                }
                if(!is_out)
                {
                    for(int index : (table_empty ? mappingIndices_fc : mappingIndices))
                    {
                        auto slnIter = ctx->solutions->find(index);
                        if(slnIter == ctx->solutions->end())
                        {
                            iot::setError(
                                io,
                                concatenate("[ProblemPredictionLibrary] Invalid solution index: ",
                                            index));
                        }
                        else
                        {
                            auto solution = slnIter->second;
                            lib.solutionmap.insert(std::make_pair(index, solution));

                            origami::dim3_t origami_mi;
                            if(solution->sizeMapping.matrixInstruction[0] == 0
                               && solution->sizeMapping.matrixInstruction[1] == 0
                               && solution->sizeMapping.matrixInstruction[2] == 0)
                            {
                                // Override dot2 instruction with vector lane widths
                                origami_mi = {1, 1, 64};
                            }
                            else
                            {
                                origami_mi = {
                                    static_cast<size_t>(solution->sizeMapping.matrixInstruction[0]),
                                    static_cast<size_t>(solution->sizeMapping.matrixInstruction[1]),
                                    static_cast<size_t>(
                                        solution->sizeMapping.matrixInstruction[2])};
                            }

                            origami::config_t origami_config = {
                                .mt = {solution->sizeMapping.macroTile.x,
                                       solution->sizeMapping.macroTile.y,
                                       solution->sizeMapping.depthU},
                                .mi = origami_mi,
                                .occupancy
                                = std::max(solution->sizeMapping.CUOccupancy, static_cast<int>(1)),
                                .workgroup_mapping         = solution->sizeMapping.workGroupMapping,
                                .cache_hints_a             = solution->sizeMapping.nonTemporalA,
                                .cache_hints_b             = solution->sizeMapping.nonTemporalB,
                                .workspace_size            = std::numeric_limits<size_t>::max(),
                                .workspace_size_per_elem_c = std::numeric_limits<size_t>::max(),
                            };

                            lib.origami_config_list.emplace_back(origami_config);
                            lib.origami_config_map.insert(std::make_pair(origami_config, index));
                        }
                    }

                    for(int index : (table_fc_empty ? mappingIndices : mappingIndices_fc))
                    {
                        auto slnIter = ctx->solutions->find(index);
                        if(slnIter == ctx->solutions->end())
                        {
                            iot::setError(
                                io,
                                concatenate("[ProblemPredictionLibrary_FC] Invalid solution index: ",
                                            index));
                        }
                        else
                        {
                            auto solution = slnIter->second;
                            lib.solutionmap_fc.insert(std::make_pair(index, solution));
                        }
                    }
                }
            }
            const static bool flow = false;
        };
    } // namespace Serialization
} // namespace TensileLite
