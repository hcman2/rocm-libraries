/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (C) 2022-2025 Advanced Micro Devices, Inc. All rights reserved.
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

// This file contains the calculateFinalPerformance function and its dependencies
// extracted from formocast_simulator.cpp

#include <origami/formocast_predict.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace Origami
{
    namespace Utils
    {
        /**
         * @brief Ceiling division for unsigned integers
         * @param numerator The dividend
         * @param denominator The divisor
         * @return The result of ceiling(numerator / denominator)
         * @throws std::invalid_argument if denominator is zero
         */
        inline uint32_t ceilDivide(uint32_t numerator, uint32_t denominator)
        {
            if (denominator == 0) {
                throw std::invalid_argument("Denominator cannot be zero");
            }
            return (numerator + denominator - 1) / denominator;
        }
    }

    using Utils::ceilDivide;

    // ============================================================================
    // Helper function: getLoopOverall
    // Calculates overall loop performance considering memory and math costs
    // ============================================================================
    double getLoopOverall(const MemoryAccessCosts& mem, double math, uint32_t loopCnt, double pgr)
    {
#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
        double loop_overall;

        if(pgr > 1 && loopCnt > 0)
            loop_overall = std::max(math, mem.mem_overall) * (loopCnt - 1) + (math);
            // loop_overall = mem.mem_overall * (loopCnt - 1) + (math);
        else
            loop_overall = std::max(math, mem.mem_overall) * loopCnt;
        return loop_overall;
#else
        double path1 = std::max(math, mem.mem_l1);
        double path2 = std::max(math, mem.mem_l2);
        double path3 = std::max(math, mem.mem_l3);
        double path4 = std::max(math, mem.mem_hbm);

        double ratio1 = mem.l1_hit;
        double ratio2 = (1 - ratio1) * mem.l2_hit;
        double ratio3 = (1 - ratio1 - ratio2) * mem.l3_hit;
        double ratio4 = (1 - ratio1 - ratio2 - ratio3);

        if(pgr > 1 && loopCnt > 0)
            return (path1 * ratio1 + path2 * ratio2 + path3 * ratio3 + path4 * ratio4) * (loopCnt - 1) + (math);
        else
            return (path1 * ratio1 + path2 * ratio2 + path3 * ratio3 + path4 * ratio4) * loopCnt;
#endif
    }

    // ============================================================================
    // Helper function: calculateMemoryAccessCosts
    // Calculates memory access costs across different cache levels
    // ============================================================================
    MemoryAccessCosts
    calculateMemoryAccessCosts(double MT0, double MT1,
                               const HardwareConstants& hw,
                               const CacheHitRates& hr,
                               double L2BandWidthPerCU, double L3BandWidthPerCU, double HBMBandWidthPerCU,
                               bool isSwizzleA, bool isSwizzleB,
                               double A_L1_req, double B_L1_req,
                               double A_L2_req, double A_L3_req, double A_hbm_req,
                               double B_L2_req, double B_L3_req, double B_hbm_req)
    {
        MemoryAccessCosts mem;

        double A_L1_clk = A_L1_req * 64 / hw.L1BusWidthPerCU;
        double A_L2_clk;
        if(isSwizzleA)
            A_L2_clk = A_L2_req * 128 / std::min(L2BandWidthPerCU, hw.L2BusWidthPerCU);
        else
            A_L2_clk = A_L2_req * 128 / std::min(L2BandWidthPerCU, hw.L2BusWidthPerCU);
        double A_L3_clk = A_L3_req * 128 / L3BandWidthPerCU;
        double A_hbm_clk = A_hbm_req * 128 / HBMBandWidthPerCU;

        double B_L1_clk = B_L1_req * 64 / hw.L1BusWidthPerCU;
        double B_L2_clk;
        if(isSwizzleB)
            B_L2_clk = B_L2_req * 128 / std::min(L2BandWidthPerCU, hw.L2BusWidthPerCU);
        else
            B_L2_clk = B_L2_req * 128 / std::min(L2BandWidthPerCU, hw.L2BusWidthPerCU);
        double B_L3_clk = B_L3_req * 128 / L3BandWidthPerCU;
        double B_hbm_clk = B_hbm_req * 128 / HBMBandWidthPerCU;

#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
        A_L1_clk = A_L1_req * hr.A_L1_hit * 64 / hw.L1BusWidthPerCU;
        A_L3_clk = A_L3_req * 64 / L3BandWidthPerCU;
        A_hbm_clk = A_hbm_req * 8 / HBMBandWidthPerCU;
        B_L1_clk = B_L1_req * hr.B_L1_hit * 64 / hw.L1BusWidthPerCU;
        B_L3_clk = B_L3_req * 64 / L3BandWidthPerCU;
        B_hbm_clk = B_hbm_req * 8 / HBMBandWidthPerCU;

        double L1_overall   = (A_L1_clk + B_L1_clk) / hw.math_frequency;
        double L2_overall   = (A_L2_clk + B_L2_clk) / hw.math_frequency;
        double L3_overall   = (A_L3_clk + B_L3_clk) / hw.mem_frequency;
        double hbm_overall  = (A_hbm_clk + B_hbm_clk) / hw.mem_frequency;
        mem.mem_overall     = L1_overall + L2_overall + L3_overall + hbm_overall;

        mem.mem_l1 = L1_overall;
        mem.mem_l2 = L2_overall;//std::max(mem.mem_l1, L2_overall);
        mem.mem_l3 = L3_overall;//std::max(mem.mem_l2, L3_overall);
        mem.mem_hbm = hbm_overall;//std::max(mem.mem_l3, hbm_overall);
        mem.l1_hit = (hr.A_L1_hit * MT0 + hr.B_L1_hit * MT1) / (MT0 + MT1);
        mem.l2_hit = hr.totalL2HitRate;
        mem.l3_hit = hr.totalL3HitRate;
#else
        double L1_overall   = (A_L1_clk + B_L1_clk) / hw.math_frequency;
        double L2_overall   = (A_L2_clk + B_L2_clk) / hw.math_frequency;
        double L3_overall   = (A_L3_clk + B_L3_clk) / hw.mem_frequency;
        double hbm_overall  = (A_hbm_clk + B_hbm_clk) / hw.mem_frequency;
        mem.mem_overall     = L1_overall + L2_overall + L3_overall + hbm_overall;

        mem.mem_l1 = L1_overall;
        mem.mem_l2 = std::max(mem.mem_l1, L2_overall);
        mem.mem_l3 = std::max(mem.mem_l2, L3_overall);
        mem.mem_hbm = std::max(mem.mem_l3, hbm_overall);
        mem.l1_hit = (hr.A_L1_hit * MT0 + hr.B_L1_hit * MT1) / (MT0 + MT1);
        mem.l2_hit = hr.totalL2HitRate;
        mem.l3_hit = hr.totalL3HitRate;

#endif
        //for debug
        mem.A_L1_req = A_L1_req;
        mem.B_L1_req = B_L1_req;
        mem.A_L2_req = A_L2_req;
        mem.B_L2_req = B_L2_req;

        return mem;
    }

    // ============================================================================
    // Helper function: resolveOccupancy
    // Adjusts performance based on CU occupancy and number of tiles
    // ============================================================================
    double resolveOccupancy(const HardwareConstants& hw, double perf, double prefetch, double mathCost, double storeCost, uint32_t num_tiles, uint32_t CUOccupancy)
    {
#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
        if ((num_tiles > 1)  && CUOccupancy >= 2)
        {
#define USE_OLD_OCCUPANCY_TWO 0 //Old Occupancy 2 doesn't make sense but has better perf.
#if USE_OLD_OCCUPANCY_TWO
            auto preLoopCost   = hw.initialCost + prefetch;
            perf = (preLoopCost + mathCost
                    + std::max(mathCost, storeCost))
                        * (num_tiles - 1)
                   + storeCost;
#else
            perf = (prefetch + mathCost)
                    + (mathCost + storeCost)
                       * (num_tiles - 1);
#endif
#else
        if ((num_tiles > 1)  && CUOccupancy >= 2 && num_tiles == CUOccupancy)
        {
#define USE_OLD_OCCUPANCY_TWO 1 //Old Occupancy 2 doesn't make sense but has better perf.
#if USE_OLD_OCCUPANCY_TWO
            auto preLoopCost   = hw.initialCost + prefetch;
            perf = (preLoopCost + mathCost
                    + std::max(mathCost, storeCost))
                        * (num_tiles - 1)
                   + storeCost;
#else
            perf = (hw.initialCost + prefetch + mathCost)
                    + std::max(mathCost, storeCost)
                       * (num_tiles - 1)
                   + storeCost;

#endif
#endif
        }
        else
        {
#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
            perf *= num_tiles;
            perf += 1.7*(num_tiles-1);
#else
            perf = perf + (perf - hw.initialCost + hw.initialCostHit) * (num_tiles - 1);
#endif
        }
        return perf;
    }

    // ============================================================================
    // Main function: calculateFinalPerformance
    // Calculates final predicted performance from intermediate metrics
    // ============================================================================
    PredictedPerformance
    calculateFinalPerformance(
        const IntermediatePerformanceMetrics& metrics,
        const ProblemInfo& problem,
        const SizeMapping& sizeMapping,
        const HardwareConstants& hw_consts,
        TieBreakerInfo& perfInfo)
    {
        PredictedPerformance pp;

        // Re-calculate all necessary values from problem, sizeMapping, and hw_consts
        double M = problem.M;
        double N = problem.N;
        double NumBatches = problem.NumBatches;
        double K = problem.K;
        bool isSwizzleA = problem.swizzleTensorA;
        bool isSwizzleB = problem.swizzleTensorB;

        double MT0 = sizeMapping.macroTile.x;
        double MT1 = sizeMapping.macroTile.y;
        int WGM = sizeMapping.workGroupMapping != 0 ? sizeMapping.workGroupMapping : 1;
        int CUOccupancy = sizeMapping.CUOccupancy;
        uint32_t depthU = sizeMapping.depthU;
        uint32_t GlobalSplitU = sizeMapping.globalSplitU;
        uint32_t GWVWD = sizeMapping.gwvwD;

        double K_AfterGSU = ceilDivide((uint32_t)K, GlobalSplitU);
        uint32_t M_WGs_total = ceilDivide(M, MT0);
        uint32_t N_WGs_total = ceilDivide(N, MT1);
        int N_WGs_per_tile_XCD = std::min((uint32_t)WGM, N_WGs_total);
        uint32_t numberWGs = M_WGs_total * N_WGs_total * NumBatches * GlobalSplitU;
        uint32_t WGs_per_tile = std::min(uint32_t(hw_consts.NumCUs), numberWGs);
        uint32_t WGs_per_tile_XCD = WGs_per_tile / hw_consts.NumXCDs;
        uint32_t num_tiles = ceilDivide(numberWGs, uint32_t(hw_consts.NumCUs));
        uint32_t loopCnt = K_AfterGSU / depthU;
        uint32_t K_tail = K_AfterGSU - (loopCnt * depthU);

        int PGR = sizeMapping.PrefetchGlobalRead;
#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
        PGR = (std::floor(K_AfterGSU/depthU > 1)) ? sizeMapping.PrefetchGlobalRead : int(K_AfterGSU/depthU);
#endif

        double L2BandWidthPerCU = hw_consts.L2ReadArbEff * 128 * 16 / WGs_per_tile_XCD;
#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
        if (L2BandWidthPerCU > hw_consts.L2ReadArbEff * 128 * 16 / (hw_consts.NumCUs/hw_consts.NumXCDs))
            L2BandWidthPerCU = hw_consts.L2ReadArbEff * 128 * 16 / (hw_consts.NumCUs/hw_consts.NumXCDs);
#endif
        double L3BandWidthPerCU = hw_consts.L3BandWidth / WGs_per_tile;
        double HBMBandWidthPerCU = hw_consts.hbmBandWidth / WGs_per_tile;

        double store = metrics.output_write_cost;
        double store_edge = metrics.output_write_cost_edge;

        // 1. Calculate Memory Access Costs using intermediate metrics
        MemoryAccessCosts mem_costs = calculateMemoryAccessCosts(
            std::min(MT0, M), std::min(MT1, N),
            hw_consts,
            metrics.cache_hits,
            L2BandWidthPerCU, L3BandWidthPerCU, HBMBandWidthPerCU,
            isSwizzleA, isSwizzleB,
            metrics.tile0_l1_request, metrics.tile1_l1_request,
            metrics.tile0_l2_request, metrics.tile0_l3_request, metrics.tile0_mem_request,
            metrics.tile1_l2_request, metrics.tile1_l3_request, metrics.tile1_mem_request);

        // 2. Calculate loop Performance
        double math_overall = metrics.compute_cycles / hw_consts.math_frequency;
        double loop_overall = getLoopOverall(mem_costs, math_overall, loopCnt, PGR);

#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
        loop_overall += loopCnt * 0.2;
#endif

        // 3. Handle Tail Loop
        double tail_overall = 0.0;
        if (K_tail > 0)
        {
#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
            tail_overall = (mem_costs.mem_overall * K_tail / depthU + math_overall) + metrics.prefetch_cost * 2;
#else
            tail_overall = (mem_costs.mem_hbm + math_overall);
#endif
        }

        // 4. Calculate preLoopCost
        double preLoopCost = metrics.startup_cost + metrics.prefetch_cost;

        // 5. Aggregate Performance: pre-loop + unrolled-loop + post-loop
        double perf = preLoopCost + loop_overall + store;
        if (num_tiles > 1)
        {
            // consider edge percentage
            double edge_percentage = 0.0;
            if (M_WGs_total * MT0 > M)
            {
                edge_percentage = 1 / (double)M_WGs_total;
            }
            store = edge_percentage * store_edge + (1 - edge_percentage) * store;
            perf = preLoopCost + loop_overall + store;
        }
#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
        else { store = std::max(store_edge, store); perf = metrics.prefetch_cost + loop_overall + store;}
#endif

        // 6. Add tail loop cost
        perf += tail_overall;

        // 7. Add LSU Reduction Part
        perf += metrics.local_split_overhead;

        // 8. Apply CU Occupancy
        perf = resolveOccupancy(hw_consts, perf, metrics.prefetch_cost, loop_overall + tail_overall, store, num_tiles, CUOccupancy);

        // 9. Add GSU Reduction Part
        perf += metrics.split_accumulation_overhead;

#undef EXPERIMENTAL
#define EXPERIMENTAL 1
#if EXPERIMENTAL
        if (int(M) % int(MT0) != 0)
            perf = perf + std::max(store_edge, store);
#endif

        pp.microSeconds = perf;
        pp.hitRate = metrics.cache_hits.totalL2HitRate * 100;

        // Update perfInfo (mutable member)
        perfInfo.memory = mem_costs;
        perfInfo.math = math_overall;
        perfInfo.svw = GWVWD;
        perfInfo.perf = perf;
        perfInfo.preloop = preLoopCost;
        perfInfo.loop = loop_overall;
        perfInfo.tail = tail_overall;
        perfInfo.store = store;
        perfInfo.gsu = metrics.split_accumulation_overhead;
        perfInfo.lsu = metrics.local_split_overhead;
        perfInfo.mt0 = MT0;
        perfInfo.mt1 = MT1;
        perfInfo.du = depthU;

        return pp;
    }

} // namespace Origami

