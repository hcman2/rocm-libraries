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

#include <set>
#include <vector>

#include <formocast.hpp>

namespace TensileLite
{

    /**
     * \ingroup SolutionLibrary
     *
     * Uses a distance function to select solutions based on benchmarks.
     * Benchmarks are performed to determine the optimal solution at a number of
     * specific sizes. At runtime, we find the benchmarked size that is closest
     * to the size asked for.
     */
    template <typename MyProblem, typename MySolution = typename MyProblem::Solution>
    struct ProblemFormoCastLibrary : public SolutionLibrary<MyProblem, MySolution>
    {
        std::unordered_map<int, std::shared_ptr<MySolution>> solutionmap;

        static std::string Type()
        {
            return "FormoCast";
        }
        virtual std::string type() const override
        {
            return Type();
        }
        virtual std::string description() const override
        {
            if(solutionmap.empty())
                return concatenate(type(), ", solutionmap: empty");
            return concatenate(type(), solutionmap.size());
        }

        static void setupFormoCast(Tensilelite::Formocast& formocast, Task& task)
        {
            auto& problem  = task.problem;
            auto& solution = task.solution;

            // GetProblemInfo
            Tensilelite::Formocast::ProblemInfo problemInfo;
            problemInfo.M          = solution.calculateDimensionM(problem);
            problemInfo.N          = solution.calculateDimensionN(problem);
            problemInfo.NumBatches = solution.calculateNumBatches(problem);

            problemInfo.K          = problem.boundSize(0);
            problemInfo.transA     = problem.transA();
            problemInfo.transB     = problem.transB();
            problemInfo.bpeA       = problem.a().elementBytes();
            problemInfo.bpeB       = problem.b().elementBytes();
            problemInfo.bpeD       = problem.d().elementBytes();
            problemInfo.bpeCompute = problem.computeTypeElementSize();

            // GetSizeMapping
            auto                                sizeMapping = solution.getSizeMapping();
            Tensilelite::Formocast::SizeMapping sm;

            sm.waveNum = sizeMapping.waveNum;

            sm.macroTile[0]      = sizeMapping.macroTile.x;
            sm.macroTile[1]      = sizeMapping.macroTile.y;
            sm.matrixInstruction = sizeMapping.matrixInstruction;

            sm.grvwA = sizeMapping.grvwA;
            sm.grvwB = sizeMapping.grvwB;
            sm.gwvwC = sizeMapping.gwvwC;
            sm.gwvwD = sizeMapping.gwvwD;

            sm.depthU             = sizeMapping.depthU;
            sm.globalSplitU       = solution.calculateAutoGSU(problem, &task.hardware);
            sm.workGroupMapping   = sizeMapping.workGroupMapping;
            sm.globalAccumulation = sizeMapping.globalAccumulation;

            sm.workGroupMappingXCC      = sizeMapping.workGroupMappingXCC;
            sm.workGroupMappingXCCGroup = sizeMapping.workGroupMappingXCCGroup;
            sm.globalSplitUCoalesced    = sizeMapping.globalSplitUCoalesced;
            sm.globalSplitUWorkGroupMappingRoundRobin
                = sizeMapping.globalSplitUWorkGroupMappingRoundRobin;

            sm.CUOccupancy            = sizeMapping.CUOccupancy;
            sm.PrefetchGlobalRead     = sizeMapping.PrefetchGlobalRead;
            sm.MathClocksUnrolledLoop = sizeMapping.MathClocksUnrolledLoop;

            sm.DirectToVgprA      = sizeMapping.DirectToVgprA;
            sm.DirectToVgprB      = sizeMapping.DirectToVgprB;
            sm.NumLoadsCoalescedA = sizeMapping.NumLoadsCoalescedA;
            sm.NumLoadsCoalescedB = sizeMapping.NumLoadsCoalescedB;
            sm.VectorWidthA       = sizeMapping.VectorWidthA;
            sm.VectorWidthB       = sizeMapping.VectorWidthB;
            sm.LocalSplitU        = sizeMapping.LocalSplitU;

            sm.waveGroup = sizeMapping.waveGroup;
        }

        virtual std::shared_ptr<MySolution> getSolutionByIndex(MyProblem const& problem,
                                                               Hardware const&  hardware,
                                                               const int index) const override
        {
            auto indexMatch = solutionmap.find(index);
            if(indexMatch != solutionmap.end())
                return indexMatch->second;
            return nullptr;
        }

        virtual std::shared_ptr<MySolution> findBestSolution(MyProblem const& problem,
                                                             Hardware const&  hardware,
                                                             double*          fitness
                                                             = nullptr) const override
        {
            auto                        topSolutions = findTopSolutions(problem, hardware, 1);
            std::shared_ptr<MySolution> solution;
            if(!topSolutions.empty())
            {
                solution = topSolutions[0];
            }
            return solution;
        }

        virtual SolutionSet<MySolution>
            findAllSolutions(MyProblem const&          problem,
                             Hardware const&           hardware,
                             SolutionLibrarySearchType searchType
                             = SolutionLibrarySearchType::DEFAULT) const override
        {
            // TODO- Temp
            std::cout << "Entering FormoCastLibrary::findAllSolutions()" << std::endl;

            bool                    debug = Debug::Instance().printPropertyEvaluation();
            SolutionSet<MySolution> rv;
            if(searchType == SolutionLibrarySearchType::DEFAULT)
                return rv;

            for(auto const& row : this->solutionmap)
            {
                if(debug)
                    std::cout << row.second->description() << std::endl;
                rv.insert(row.second);
            }

            return rv;
        }

        virtual SolutionSet<MySolution>
            findAllSolutionsGroupedGemm(std::vector<MyProblem> const& problems,
                                        Hardware const&               hardware,
                                        SolutionLibrarySearchType     searchType
                                        = SolutionLibrarySearchType::DEFAULT) const override
        {
            bool                    debug = Debug::Instance().printPropertyEvaluation();
            SolutionSet<MySolution> rv;
            if(searchType == SolutionLibrarySearchType::DEFAULT)
                return rv;

            for(auto const& row : this->solutionmap)
            {
                if(debug)
                    std::cout << row.second->description() << std::endl;
                rv.insert(row.second);
            }

            return rv;
        }

        virtual SolutionVector<MySolution> findTopSolutions(MyProblem const& problem,
                                                            Hardware const&  hardware,
                                                            int numSolutions) const override
        {
            // TODO- Temp
            std::cout << "Entering FormoCastLibrary::findTopSolutions()" << std::endl;

            bool                                debug = Debug::Instance().printPropertyEvaluation();
            SolutionVector<MySolution>          rv;
            Tensilelite::Formocast              formocast;
            std::vector<std::pair<int, double>> performance;
            // TODO- tie breaker
            // std::vector<Tensilelite::Formocast::TieBreakerInfo> tbInfo;
            for(auto const& row : solutionmap)
            {
                int  sol_idx  = row.first;
                auto solution = row.second;

                if(debug)
                {
                    std::cout << solution->description() << ": ";
                }

                if((*solution->hardwarePredicate)(hardware)
                   && (*solution->problemPredicate)(problem))
                {
                    Task task(hardware, problem, *solution);
                    setupFormoCast(formocast, task);
                    Tensilelite::Formocast::PredictedPerformance predPerf
                        = formocast.predictedPerformance();
                    performance.push_back(std::pair(sol_idx, predPerf.microSeconds));
                }
            }

            auto comp = [](const std::pair<int, double>& e1, const std::pair<int, double>& e2) {
                return e1.second < e2.second;
            };
            std::sort(performance.begin(), performance.end(), comp);

            for(int i = 0; i < performance.size(); i++)
            {
                auto solution = solutionmap.at(performance[i].first);
                rv.emplace_back(solution);
                if(rv.size() == numSolutions)
                {
                    break;
                }
            }

            return rv;
        }

        virtual SolutionVector<MySolution>
            findTopSolutionsGroupedGemm(std::vector<MyProblem> const& problems,
                                        Hardware const&               hardware,
                                        int                           numSolutions) const override
        {
            SolutionVector<MySolution> solutions;
            return solutions;
        }
    };
} // namespace TensileLite
