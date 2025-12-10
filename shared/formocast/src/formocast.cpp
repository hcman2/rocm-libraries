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

#include <formocast.hpp>
#include <formocast_utils.hpp>
#include <simulator.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <cassert>
#include <cstring>

namespace Tensilelite
{
    using Utils::ceilDivide;

    static double getPrefetchPerformance(int      pgr,
                                         int      grvwa,
                                         int      grvwb,
                                         int      bpeA,
                                         int      bpeB,
                                         uint32_t depthU,
                                         int      waveNum,
                                         double   MT0,
                                         double   MT1,
                                         double   math_frequency,
                                         double   mem_latency,
                                         int      numAccPerWave)
    {
        const double others = 220 + numAccPerWave * 4;
        int          stallA = 4;
        int          lwA    = 8;
        switch(grvwa * bpeA)
        {
        case 16:
            stallA = 25;
            lwA    = 20;
            break;
        case 8:
            stallA = 18;
            lwA    = 12;
            break;
        case 4:
            stallA = 8;
            lwA    = 8;
            break;
        default:
            stallA = 4;
            lwA    = 8;
        }
        int stallB = 4;
        int lwB    = 1;
        switch(grvwb * bpeB)
        {
        case 16:
            stallB = 25;
            lwB    = 20;
            break;
        case 8:
            stallB = 18;
            lwB    = 12;
            break;
        case 4:
            stallB = 8;
            lwB    = 8;
            break;
        default:
            stallB = 4;
            lwB    = 8;
        }

        int numGRA = MT0 * depthU * bpeA / (waveNum * 64) / grvwa;
        int numGRB = MT1 * depthU * bpeB / (waveNum * 64) / grvwb;

        //issue 2nd prefetch
        double grCycles2 = numGRA * 4 / waveNum;
        grCycles2 += numGRB * 4 / waveNum;

        if(pgr >= 2)
        {
            double grCycles = 0.0;
            if(numGRA + numGRB > 16)
            {
                grCycles     = 16 * 4;
                auto extraGR = numGRA + numGRB - 16;
                if(numGRA > 16)
                {
                    //issue GRA
                    grCycles += stallA * (numGRA - 16);
                }
                //issue GRB
                if(numGRB <= 16)
                {
                    grCycles += stallA * numGRB;
                }
                else
                {
                    grCycles += stallA * 16;
                    grCycles += stallB * (16 - numGRB);
                }
            }
            else
            {
                grCycles = (numGRA + numGRB) * 4 * (waveNum / 2);
            }

            //issue local write
            double lwCycles = numGRA * lwA / waveNum;
            lwCycles += numGRB * lwB / waveNum;

            double perf = std::max((grCycles + others) / math_frequency, mem_latency)
                          + (lwCycles + grCycles2) / math_frequency;
            //std::cout<<"grCycles, others, math_frequency="<<grCycles<<","<<others<<","<<math_frequency<<""<<std::endl;
        }
        return (grCycles2 + others) / math_frequency;
    }



    double Formocast::getLoopOverall(const MemoryAccessCosts& mem, double math, uint32_t loopCnt, double pgr) const
    {
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
    }

    Formocast::HardwareConstants archConstantMap(const unsigned char* magic, size_t magicSize) {
        Formocast::HardwareConstants hw;
        if (magicSize != sizeof(Formocast::HardwareConstants)) {
            std::cerr << "Error: magic number size does not match HardwareConstants size!" << std::endl;
        }
        std::memcpy(&hw, magic, std::min(magicSize, sizeof(Formocast::HardwareConstants)));
        return hw;
    }

    Formocast::HardwareConstants
    Formocast::getHardwareConstants() const
    {
        HardwareConstants hw;
        if(hardware->arch == origami::hardware_t::architecture_t::gfx950)
        {
            unsigned char magic[184] = {0, 0, 0, 0, 0, 0, 224, 64, 0, 0, 0, 0, 0, 0, 80, 65, 0, 0, 0, 0, 0, 0, 176, 65, 0, 0, 0, 0, 0, 0, 96, 64, 0, 0, 0, 0, 0, 0, 96, 64, 0, 0, 0, 0, 0, 0, 80, 64, 0, 0, 0, 0, 0, 0, 96, 64, 0, 0, 0, 0, 0, 0, 80, 64, 0, 0, 0, 0, 0, 0, 80, 64, 0, 0, 0, 0, 0, 0, 8, 64, 0, 0, 0, 0, 0, 176, 157, 64, 189, 134, 242, 26, 202, 171, 152, 64, 189, 134, 242, 26, 202, 171, 168, 64, 0, 0, 0, 0, 0, 32, 156, 64, 0, 0, 0, 0, 0, 92, 162, 64, 205, 204, 204, 204, 204, 204, 4, 64, 205, 204, 204, 204, 204, 204, 0, 64, 0, 0, 0, 0, 0, 0, 176, 64, 0, 0, 0, 0, 0, 0, 112, 64, 0, 0, 0, 0, 0, 0, 80, 64, 205, 204, 204, 204, 204, 204, 236, 63, 0, 0, 0, 0, 0, 0, 232, 63, 8, 0, 0, 0, 0, 0, 0, 0};
            hw = archConstantMap(magic, 184);
        }
        else if(hardware->arch == origami::hardware_t::architecture_t::gfx942)
        {
            unsigned char magic[184] = {0, 0, 0, 0, 0, 0, 224, 64, 0, 0, 0, 0, 0, 0, 80, 65, 0, 0, 0, 0, 0, 0, 176, 65, 0, 0, 0, 0, 0, 0, 96, 64, 0, 0, 0, 0, 0, 0, 96, 64, 0, 0, 0, 0, 0, 0, 80, 64, 0, 0, 0, 0, 0, 0, 96, 64, 0, 0, 0, 0, 0, 0, 80, 64, 0, 0, 0, 0, 0, 0, 80, 64, 0, 0, 0, 0, 0, 0, 8, 64, 0, 0, 0, 0, 0, 80, 148, 64, 118, 98, 39, 118, 98, 7, 162, 64, 118, 98, 39, 118, 98, 7, 178, 64, 0, 0, 0, 0, 0, 48, 145, 64, 1, 96, 132, 2, 0, 0, 0, 0, 154, 153, 153, 153, 153, 153, 5, 64, 64, 96, 132, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 160, 64, 0, 0, 0, 0, 0, 0, 115, 64, 0, 0, 0, 0, 0, 0, 80, 64, 205, 204, 204, 204, 204, 204, 236, 63, 143, 194, 245, 40, 92, 143, 226, 63, 8, 0, 0, 0, 0, 0, 0, 0};
            hw = archConstantMap(magic, 184);
        }
        else
        {
            throw std::runtime_error(
                    "Attempting to retrieve hardware constants for unsupported architecture");
        }

        hw.NumCUs               = hardware->N_CU;
        hw.NumXCDs              = hardware->NUM_XCD;
        hw.L2CacheCapacity      = hardware->L2_capacity;
        hw.boost_frequency      = hardware->compute_clock_ghz * 1000;
        return hw;
    }

    void Formocast::calculateStorePerformance(double M, double N, double NumBatches, double MT0, double MT1, uint32_t GWVWD, uint32_t bpeD, const HardwareConstants& hw_consts, uint32_t WGs_per_tile, uint32_t WGs_per_tile_XCD, double &store, double &store_edge) const
    {
        double D_L1_req = 0.0;
        double D_L2_req = 0.0;
        double D_L3_req = 0.0;
        double D_L1_edge_req, D_L2_edge_req, D_L3_edge_req;
        double total_store_req1
            = Simulator::calculateStoreL1Request(M, N, MT0, MT1, GWVWD, D_L1_req, D_L1_edge_req);
        double total_store_req2
            = Simulator::calculateStoreL2Request(M, N, MT0, MT1, GWVWD, D_L2_req, D_L2_edge_req);
        double total_store_req3 = Simulator::calculateStoreL3Request(M, N, MT0, MT1, D_L3_req, D_L3_edge_req);

        // std::cout<<"store L1 non-edge= "<<D_L1_req<<std::endl;
        // std::cout<<"store L1 edge    = "<<D_L1_edge_req<<std::endl;
        // std::cout<<"store L2 non-edge= "<<D_L2_req<<std::endl;
        // std::cout<<"store L2 edge    = "<<D_L2_edge_req<<std::endl;
        // std::cout<<"store L3 non-edge= "<<D_L3_req<<std::endl;
        // std::cout<<"store L3 edge    = "<<D_L3_edge_req<<std::endl;
        // std::cout<<"store L1 request = "<<total_store_req1<<std::endl;
        // std::cout<<"store L2 request = "<<total_store_req2<<std::endl;
        // std::cout<<"store L3 request = "<<total_store_req3<<std::endl;

        double L2WriteBandWidthPerCU = hw_consts.L2WriteArbEff * 128 * 16 / WGs_per_tile_XCD; //58% eff
        double L3BandWidthPerCU      = hw_consts.L3BandWidth / WGs_per_tile;
        double HBMBandWidthPerCU     = hw_consts.hbmBandWidth / WGs_per_tile;
        double D_L1_clk              = D_L1_req * 64 / hw_consts.L1WriteBusWidthPerCU;
        double D_L2_clk = D_L2_req * 64 / std::min(hw_consts.L2WriteBusWidthPerCU, L2WriteBandWidthPerCU);
        double D_L3_clk = D_L3_req * 64 / L3BandWidthPerCU;
        // TODO: D_hbm_clk use D_L3_req.
        double D_hbm_clk     = 0 * 64 / HBMBandWidthPerCU;
        double D_L1_clk_edge = D_L1_edge_req * 64 / hw_consts.L1WriteBusWidthPerCU;
        double D_L2_clk_edge
            = D_L2_edge_req * 64 / std::min(hw_consts.L2WriteBusWidthPerCU, L2WriteBandWidthPerCU);
        double D_L3_clk_edge  = D_L3_edge_req * 64 / L3BandWidthPerCU;
        double D_hbm_clk_edge = 0 * 64 / HBMBandWidthPerCU;
        double D_L1_clk_total = total_store_req1 * 64 / hw_consts.L1WriteBusWidthPerCU;
        double D_L2_clk_total
            = total_store_req2 * 64 / std::min(hw_consts.L2WriteBusWidthPerCU, L2WriteBandWidthPerCU);
        double D_L3_clk_total  = total_store_req3 * 64 / L3BandWidthPerCU;
        double D_hbm_clk_total = 0 * 64 / HBMBandWidthPerCU;

        double store_edge_overall = ((D_L1_clk_edge + D_L2_clk_edge) / hw_consts.math_frequency)
                                    + ((D_L3_clk_edge + D_hbm_clk_edge) / hw_consts.mem_frequency);
        double store_non_edge_overall
            = ((D_L1_clk + D_L2_clk) / hw_consts.math_frequency) + ((D_L3_clk + D_hbm_clk) / hw_consts.mem_frequency);
        double store_total = ((D_L1_clk_total + D_L2_clk_total) / hw_consts.math_frequency)
                             + ((D_L3_clk_total + D_hbm_clk_total) / hw_consts.mem_frequency);
        // Use the max of edge/non-edge store
        store      = store_non_edge_overall;
        store_edge = store_edge_overall;
    }

    Formocast::L1CacheHitRate
    Formocast::computeL1CacheHitRate(const HardwareConstants& hw,
                                          double MT0, double MT1, uint32_t bpeA, uint32_t bpeB,
                                          int NTA, int NTB, uint32_t GRVWA, uint32_t GRVWB,
                                          bool DTVA, bool DTVB, bool isSwizzleA, bool isSwizzleB,
                                          uint32_t VWA, uint32_t VWB, bool transA, bool transB,
                                          double lda, double ldb, int NLCA, int NLCB,
                                          uint32_t threadnum, uint32_t NumWave0, uint32_t NumWave1) const
    {
        auto hr = Simulator::computeL1CacheHitRate(
            hw.L1CacheCapacity, hw.L1CacheLineSize, hw.L1BusWidthPerCU,
            MT0, MT1, bpeA, bpeB, NTA, NTB, GRVWA, GRVWB, DTVA, DTVB,
            isSwizzleA, isSwizzleB, VWA, VWB, transA, transB, lda, ldb,
            NLCA, NLCB, threadnum, NumWave0, NumWave1);
        
        L1CacheHitRate result;
        result.tile0HitRate = hr.tile0HitRate;
        result.tile1HitRate = hr.tile1HitRate;
        return result;
    }

    Formocast::L3CacheHitRate
    Formocast::computeL3CacheHitRate(double M, double N, double K, const HardwareConstants& hw,
                                          uint32_t bpeA, uint32_t bpeB, int NTA, int NTB,
                                          int N_WGs_total, int M_WGs_total, int N_WGs_per_tile, int M_WGs_per_tile) const
    {
        auto hr = Simulator::computeL3CacheHitRate(
            M, N, K, hw.L3CacheCapacity, hw.NumCUs, bpeA, bpeB, NTA, NTB,
            N_WGs_total, M_WGs_total, N_WGs_per_tile, M_WGs_per_tile);
        
        L3CacheHitRate result;
        result.totalHitRate = hr.totalHitRate;
        result.tile0HitRate = hr.tile0HitRate;
        result.tile1HitRate = hr.tile1HitRate;
        return result;
    }

    double Formocast::calculateGSUOverhead(double M, double N, double K,
                                                        double NumBatches, double GlobalSplitU,
                                                        uint32_t gsuMethod, ProblemInfo problem,
                                                        const HardwareConstants& hw_consts,
                                                        uint32_t WGs_per_tile, uint32_t WGs_per_tile_XCD,
                                                        double MT0, double MT1, uint32_t numWGs, double vgprCheck,
                                                        double storeGSU) const
    {
        double gsu_overall = 0.0;
        
        if(gsuMethod == 2 && GlobalSplitU > 1) //MB
        {
            gsu_overall = Simulator::getMBOverhead(
                M, N, GlobalSplitU, NumBatches,
                problem.bpeCompute, problem.bpeD, hw_consts.hbmBandWidth,
                hw_consts.L1CacheLineSize, hw_consts.NumCUs, hw_consts.boost_frequency,
                hw_consts.mem_frequency, hw_consts.L2WriteArbEff, hw_consts.L2ReadArbEff,
                hw_consts.L3BandWidth, hw_consts.L1BusWidthPerCU, hw_consts.L2BusWidthPerCU,
                hw_consts.L1WriteBusWidthPerCU, hw_consts.L2WriteBusWidthPerCU
            );
        }
        else if(gsuMethod == 3 && GlobalSplitU > 1) //MBSK
        {
            gsu_overall = Simulator::getMBSKOverhead(
                GlobalSplitU, MT0, MT1, problem.bpeCompute,
                hw_consts.NumCUs, numWGs, hw_consts.boost_frequency,
                hw_consts.L2ReadArbEff, hw_consts.L1BusWidthPerCU, hw_consts.L2BusWidthPerCU,
                storeGSU
            );
        }

        return gsu_overall;
    }

    double Formocast::calculateLSUOverhead(double MT0, double MT1, double lsu,
                                                     uint32_t svw, uint32_t numThreads,
                                                     ProblemInfo problem,
                                                     const HardwareConstants& hw_consts) const
    {
        return Simulator::getLSUOverhead(MT0, MT1, lsu, svw, numThreads, 
                                         problem.bpeCompute, hw_consts.math_frequency);
    }

    Formocast::MemoryAccessCosts
    Formocast::calculateMemoryAccessCosts(double MT0, double MT1, uint32_t depthU,
                                   uint32_t bpeA, uint32_t bpeB,
                                   const HardwareConstants& hw,
                                   uint32_t GRVWA, uint32_t GRVWB,
                                   bool DTVA, bool DTVB,
                                   const CacheHitRates& hr,
                                   double L2BandWidthPerCU, double L3BandWidthPerCU, double HBMBandWidthPerCU,
                                   uint32_t VWA, uint32_t VWB,
                                   bool isSwizzleA, bool isSwizzleB,
                                   bool trA, bool trB,
                                   uint32_t numWave0, uint32_t numWave1,
                                   int NLCA, int NLCB) const
    {
        MemoryAccessCosts mem;
        double tcc_ea0_coalscedA;
        double tcc_ea0_coalscedB;
        double A_L1_req = Simulator::getLoadRequest(MT0, depthU, hw.L1CacheLineSize, 
                                         GRVWA, bpeA, DTVA, 
                                         trA,           // isTransposed
                                         isSwizzleA,    // isSwizzled (for transposed case)
                                         VWA,           // VW (for transposed case)
                                         hw.L1BusWidthPerCU,  // L1BusWidthPerCU (for non-transposed case)
                                         NLCA,          // NumLoadsCoalesced (for non-transposed case)
                                         numWave1,      // numWaveX (for non-transposed case)
                                         tcc_ea0_coalscedA);

        double B_L1_req = Simulator::getLoadRequest(MT1, depthU, hw.L1CacheLineSize, 
                                         GRVWB, bpeB, DTVB, 
                                         !trB,          // isTransposed (B is transposed when trB=false)
                                         isSwizzleB,    // isSwizzled (for transposed case)
                                         VWB,           // VW (for transposed case)
                                         hw.L1BusWidthPerCU,  // L1BusWidthPerCU (for non-transposed case)
                                         NLCB,          // NumLoadsCoalesced (for non-transposed case)
                                         numWave0,      // numWaveX (for non-transposed case)
                                         tcc_ea0_coalscedB);

        double A_L2_req = A_L1_req * (1 - hr.A_L1_hit) / 2 * tcc_ea0_coalscedA;
        double A_L3_req = A_L2_req * (1 - hr.A_L2_hit) / tcc_ea0_coalscedA;
        double A_hbm_req = A_L3_req * (1 - hr.A_L3_hit);
        double B_L2_req = B_L1_req * (1 - hr.B_L1_hit) / 2 * tcc_ea0_coalscedB;
        double B_L3_req = B_L2_req * (1 - hr.B_L2_hit) / tcc_ea0_coalscedB;
        double B_hbm_req = B_L3_req * (1 - hr.B_L3_hit);

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

        //for debug
        mem.A_L1_req = A_L1_req;
        mem.B_L1_req = B_L1_req;
        mem.A_L2_req = A_L2_req;
        mem.B_L2_req = B_L2_req;

        return mem;
    }

    double Formocast::resolveOccupancy(const HardwareConstants& hw, double perf, double prefetch, double mathCost, double storeCost, uint32_t num_tiles, uint32_t CUOccupancy) const
    {
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
        }
        else
        {
            perf = perf + (perf - hw.initialCost + hw.initialCostHit) * (num_tiles - 1);
        }
        return perf;
    }

    bool Formocast::isBetter(ProblemInfo problem, TieBreakerInfo previousSolution) const
    {
        double M = problem.M;
        double N = problem.N;
        double NumBatches = problem.NumBatches;
        double K = problem.K;

        assert(M != 0);

        if (N <= 32 && M >= 1024 && K >= 1024)
        {
            auto currSol = getTieBreakerInfo();
            auto currSkinny = currSol.mt0 * currSol.du / M;
            auto prevSkinny = previousSolution.mt0 * previousSolution.du / M;
            if (currSol.mt1 == N && currSkinny > prevSkinny)
            {
                return true;
            }
        }
        if (M <= 32 && N >= 1024 && K >= 1024)
        {
            auto currSol = getTieBreakerInfo();
            auto currSkinny = currSol.mt1 * currSol.du / M;
            auto prevSkinny = previousSolution.mt1 * previousSolution.du / M;
            if (currSol.mt0 == M && currSkinny > prevSkinny)
            {
                return true;
            }
        }
        if (NumBatches == 1 && K <= 512 && M >= 1024 && N >= 1024)
        {
            auto currSol = getTieBreakerInfo();
            if (currSol.svw > previousSolution.svw)
            {
                return true;
            }
        }
        return false;
    }

    // NB:
    //  isBetter return
    //   True means current is better than previous
    //   False doesn't means worse, means tie (equal) --> IMPORTANT note since this would be used in std::sort
    bool Formocast::isBetter(TieBreakerInfo previousSolution, TieBreakerInfo currentSolution) const
    {
        double M = problem.M;
        double N = problem.N;
        double NumBatches = problem.NumBatches;
        double K = problem.K;

        assert(M != 0);

        if (N <= 32 && M >= 1024 && K >= 1024)
        {
            auto currSkinny = currentSolution.mt0 * currentSolution.du / M;
            auto prevSkinny = previousSolution.mt0 * previousSolution.du / M;
            if (currentSolution.mt1 == N && currSkinny > prevSkinny)
            {
                return true;
            }
        }
        if (M <= 32 && N >= 1024 && K >= 1024)
        {
            auto currSkinny = currentSolution.mt1 * currentSolution.du / M;
            auto prevSkinny = previousSolution.mt1 * previousSolution.du / M;
            if (currentSolution.mt0 == M && currSkinny > prevSkinny)
            {
                return true;
            }
        }
        if (NumBatches == 1 && K <= 512 && M >= 1024 && N >= 1024)
        {
            if (currentSolution.svw > previousSolution.svw)
            {
                return true;
            }
        }
        return false;
    }

    Formocast::TieBreakerInfo Formocast::getTieBreakerInfo() const
    {
        return perfInfo;
    }

    // NB:
    //  isBetter return
    //   True means current is better than previous
    //   False doesn't means worse, means tie (EQUAL) --> IMPORTANT note since this would be used in std::sort
    bool Formocast::isBetter(MinTieBreakerInfo previousSolution, MinTieBreakerInfo currentSolution) const
    {
        // just early return, return false means equal
        if(previousSolution == currentSolution)
            return false;

        double M = problem.M;
        double N = problem.N;
        double NumBatches = problem.NumBatches;
        double K = problem.K;

        assert(M != 0);

        if (N <= 32 && M >= 1024 && K >= 1024)
        {
            auto currSkinny = currentSolution.mt0 * currentSolution.du / M;
            auto prevSkinny = previousSolution.mt0 * previousSolution.du / M;
            if (currentSolution.mt1 == N && currSkinny > prevSkinny)
            {
                return true;
            }
        }
        if (M <= 32 && N >= 1024 && K >= 1024)
        {
            auto currSkinny = currentSolution.mt1 * currentSolution.du / M;
            auto prevSkinny = previousSolution.mt1 * previousSolution.du / M;
            if (currentSolution.mt0 == M && currSkinny > prevSkinny)
            {
                return true;
            }
        }
        if (NumBatches == 1 && K <= 512 && M >= 1024 && N >= 1024)
        {
            if (currentSolution.svw > previousSolution.svw)
            {
                return true;
            }
        }
        return false;
    }

    Formocast::MinTieBreakerInfo Formocast::getMinTieBreakerInfo() const
    {
        MinTieBreakerInfo tbInfo;

        tbInfo.mt0 = perfInfo.mt0;
        tbInfo.mt1 = perfInfo.mt1;
        tbInfo.du  = perfInfo.du;
        tbInfo.svw = perfInfo.svw;

        return tbInfo;
    }

    Formocast::PredictedPerformance
    Formocast::predictedPerformance(void) const
    {
        PredictedPerformance pp;

        //std::cout<<"[Formocast] predictedPerformance"<<std::endl;

        // 1. Problem Dimension Calculation
        double M = problem.M;
        double N = problem.N;
        double NumBatches = problem.NumBatches;
        double K = problem.K;
        bool transA = problem.transA;
        bool transB = problem.transB;
        uint32_t bpeA    = problem.bpeA;
        uint32_t bpeB    = problem.bpeB;
        uint32_t bpeD    = problem.bpeD;
        // swizzle settings
        bool     isSwizzleA = problem.swizzleTensorA;
        bool     isSwizzleB = problem.swizzleTensorB;

        // 2. Hardware Parameter Extraction
        HardwareConstants hw_consts = getHardwareConstants();
        //hw_consts.print();

        // 3. Variables directly from sizeMapping

        // Basic tile and workgroup configuration
        double MT0 = sizeMapping.macroTile[0];
        double MT1 = sizeMapping.macroTile[1];
        int      WGM = sizeMapping.workGroupMapping != 0 ? sizeMapping.workGroupMapping : 1;
        int      CUOccupancy = sizeMapping.CUOccupancy;
        uint32_t depthU = sizeMapping.depthU;

        // Global split
        bool     isGSUWGMRR = sizeMapping.globalSplitUWorkGroupMappingRoundRobin;
        uint32_t gsuMethod = sizeMapping.globalAccumulation;

        // Prefetch and memory access configuration
        int      PGR = sizeMapping.PrefetchGlobalRead;

        // Wave and global read configuration
        uint32_t GRVWA = sizeMapping.grvwA;
        uint32_t GRVWB = sizeMapping.grvwB;
        uint32_t GWVWD = sizeMapping.gwvwD;
        uint32_t VWA   = sizeMapping.VectorWidthA;
        uint32_t VWB   = sizeMapping.VectorWidthB;
        uint32_t waveNum  = sizeMapping.waveNum;
        uint32_t NumWave0 = sizeMapping.waveGroup[0];
        uint32_t NumWave1 = sizeMapping.waveGroup[1];
        uint32_t NumThreads = hw_consts.wavefrontSize * waveNum;

        // Matrix instruction and VGPR configuration
        int miSize = sizeMapping.matrixInstruction[0];
        bool DTVA = sizeMapping.DirectToVgprA;
        bool DTVB = sizeMapping.DirectToVgprB;

        // NLCA/B is used for non-TN cases to calculate load requests.
        int NLCA = sizeMapping.NumLoadsCoalescedA;
        int NLCB = sizeMapping.NumLoadsCoalescedB;

        //GlobalSplitU
        uint32_t GlobalSplitU = sizeMapping.globalSplitU;
        //LocalSplitU
        int LSU = sizeMapping.LocalSplitU;

        // Clock calculation
        // TODO: No need to check minMathClock if we guarantee that MathClocksUnrolledLoop is correct.
        double math_clk = sizeMapping.MathClocksUnrolledLoop;
        //auto miLatency = hardware->get_mi_latency(sizeMapping.matrixInstruction[0], sizeMapping.matrixInstruction[1], sizeMapping.matrixInstruction[2], origami::data_type_t::BFloat16); //FIXME: Only for Bf16 now (TF32 is also BF16 in gfx950)
        //auto minMathClock = ((double)MT0 * MT1 * depthU / (sizeMapping.matrixInstruction[0] * sizeMapping.matrixInstruction[1] * sizeMapping.matrixInstruction[2]) * miLatency);
        //assert(math_clk >= minMathClock);
        //math_clk = std::max(math_clk, minMathClock); //FIXME: CMS kernel has incorrect MathClocksUnrolledLoop

        // Debug output (commented)
        //std::cout<<"DTVA         =          "<<DTVA<<std::endl;
        //std::cout<<"DTVB         =          "<<DTVB<<std::endl;
        //std::cout<<"MT0          =          "<<MT0<<std::endl;
        //std::cout<<"MT1          =          "<<MT1<<std::endl;
        //std::cout<<"GlobalSplitU =          "<<GlobalSplitU<<std::endl;
        //std::cout<<"math_clk     =          "<<math_clk<<std::endl;
        //std::cout<<"WGM          =          "<<WGM<<std::endl;
        //std::cout<<"CUOccupancy  =          "<<CUOccupancy<<std::endl;
        //std::cout<<"depthU       =          "<<depthU<<std::endl;
        //std::cout<<"PGR          =          "<<PGR<<std::endl;
        //std::cout<<"GWVWD        =          "<<GWVWD<<std::endl;
        //std::cout<<"miSize       =          "<<miSize<<std::endl;

        // 3.1 Early terminate. FIXME: Can filter most of the solutions with an outside function.
        // FIXME: add an extra function to reject the solutions first.
        if (GlobalSplitU == 0)
        {
            // FIXME: Need to support streamK kernels.
            GlobalSplitU = 1;
            pp.microSeconds = 9999999.9;
            pp.hitRate = 0;
            return pp;
        }
        if (MT0 - M >= 16 || MT1 - N >= 16)
        {
            //std::cout<<"M:"<<M<<",N:"<<N<<",MT0:"<<MT0<<",MT1:"<<MT1<<std::endl;
            pp.microSeconds = 9999999.9;
            pp.hitRate = 0;
            return pp;
        }
        if(problem.dataType == origami::data_type_t::BFloat16 || problem.dataType == origami::data_type_t::Half)
        {
            //TODO: handle TF32 problem so that check the BPE here.
            if(problem.bpeA == 2 && problem.bpeB == 2)
                if (((K >= 64 && depthU <=32) || (K <= 32 && depthU > 32) || (K > 32 && depthU > K)) && NumBatches < hw_consts.NumCUs && sizeMapping.matrixInstruction[2] >= 32)
                {
                    std::cout<<"K:"<<K<<",depthU:"<<depthU<<",NumBatches:"<<NumBatches<<",sizeMapping.matrixInstruction[2]:"<<sizeMapping.matrixInstruction[2]<<std::endl;
                    pp.microSeconds = 9999999.9;
                    pp.hitRate = 0;
                    return pp;
                }
        }

        // 4. Derived Problem/Workgroup Dimensions
        double K_AfterGSU = ceilDivide((uint32_t)K, GlobalSplitU);
        uint32_t M_WGs_total = ceilDivide(M, MT0);
        uint32_t N_WGs_total = ceilDivide(N, MT1);
        int N_WGs_per_tile_XCD = std::min((uint32_t)WGM, N_WGs_total);
        int M_WGs_per_tile_XCD
            = std::min(M_WGs_total, ceilDivide(int(hw_consts.NumCUs / 8), N_WGs_per_tile_XCD));
        int M_WGs_per_tile = std::min(M_WGs_total, ceilDivide(int(hw_consts.NumCUs), N_WGs_per_tile_XCD));
        int N_WGs_per_tile
            = std::min(N_WGs_total, N_WGs_per_tile_XCD * ceilDivide(M_WGs_per_tile, M_WGs_total));
        uint32_t numberWGs = M_WGs_total * N_WGs_total * NumBatches * GlobalSplitU;
        uint32_t WGs_per_tile = std::min(uint32_t(hw_consts.NumCUs), numberWGs);
        uint32_t WGs_per_tile_XCD = WGs_per_tile / hw_consts.NumXCDs;
        uint32_t num_tiles = ceilDivide(numberWGs, uint32_t(hw_consts.NumCUs));
        uint32_t loopCnt = K_AfterGSU / depthU;
        uint32_t K_tail = K_AfterGSU - (loopCnt * depthU);

        // 5. Cache Hit Rates and Bandwidths
        CacheHitRates cache_hits;
        L1CacheHitRate l1 = computeL1CacheHitRate(hw_consts,
                                                MT0, MT1, bpeA, bpeB,
                                                0, 0, GRVWA, GRVWB,
                                                DTVA, DTVB, isSwizzleA, isSwizzleB,
                                                VWA, VWB, transA, transB,
                                                M, N, NLCA, NLCB,
                                                NumThreads, NumWave0, NumWave1);
        L2CacheHitRate l2 = computeL2CacheHitRate(M,
                                                N,
                                                K_AfterGSU,
                                                hw_consts,
                                                GlobalSplitU,
                                                WGM,
                                                NumBatches,
                                                bpeA,
                                                bpeB,
                                                0,
                                                0,
                                                isGSUWGMRR);
        L3CacheHitRate l3 = computeL3CacheHitRate(M, N, K, hw_consts,
                                                bpeA, bpeB, 0, 0,
                                                N_WGs_total, M_WGs_total, N_WGs_per_tile, M_WGs_per_tile);

        cache_hits.A_L1_hit = l1.tile0HitRate;
        cache_hits.B_L1_hit = l1.tile1HitRate;
        cache_hits.A_L2_hit = l2.tile0HitRate;
        cache_hits.B_L2_hit = l2.tile1HitRate;
        cache_hits.A_L3_hit = l3.tile0HitRate;
        cache_hits.B_L3_hit = l3.tile1HitRate;
        cache_hits.totalL2HitRate = l2.totalHitRate;
        cache_hits.totalL3HitRate = l3.totalHitRate;

        // 6. Calculate Store Performance (D matrix writes)
        double store, store_edge;
        calculateStorePerformance(M, N, NumBatches, MT0, MT1, GWVWD, bpeD, hw_consts, WGs_per_tile, WGs_per_tile_XCD, store, store_edge);

        // 7. Calculate GSU Overhead
        double storeGSU = store * 2; //FIXME: incorrect
        auto vgprUsageCheck = MT0 * MT1 / miSize / miSize;
        double gsu_overall = calculateGSUOverhead(M, N, K, NumBatches, GlobalSplitU, gsuMethod,
                                                  problem, hw_consts, WGs_per_tile, WGs_per_tile_XCD,
                                                  MT0, MT1, numberWGs, vgprUsageCheck, storeGSU);

        // 8. Calcupate LSU Overhead
        double lsu_overall = calculateLSUOverhead(MT0, MT1, LSU, GWVWD, NumThreads, problem, hw_consts);

        // 9. Calculate Memory Access and Math Costs
        double L2BandWidthPerCU     = hw_consts.L2ReadArbEff * 128 * 16 / WGs_per_tile_XCD; //90% eff
        double L3BandWidthPerCU     = hw_consts.L3BandWidth / WGs_per_tile;
        double HBMBandWidthPerCU    = hw_consts.hbmBandWidth / WGs_per_tile;
        MemoryAccessCosts mem_costs = calculateMemoryAccessCosts(std::min(MT0, M), std::min(MT1, N), depthU,
                                                                bpeA, bpeB,
                                                                hw_consts,
                                                                GRVWA, GRVWB,
                                                                DTVA, DTVB,
                                                                cache_hits,
                                                                L2BandWidthPerCU, L3BandWidthPerCU, HBMBandWidthPerCU,
                                                                VWA, VWB,
                                                                isSwizzleA, isSwizzleB,
                                                                transA, transB,
                                                                NumWave0, NumWave1,
                                                                NLCA, NLCB);

        // 10. Calculate Prefetch Performance
        int numAccPerWave = MT0 * MT1 / waveNum / hw_consts.wavefrontSize;
#define USE_OLD_PREFETCH 1
#if USE_OLD_PREFETCH
        double prefetch_mem = mem_costs.mem_overall;
#else
        double prefetch_mem = mem_costs.mem_hbm;
#endif
        double prefetch      = getPrefetchPerformance(PGR,
                                                 GRVWA,
                                                 GRVWB,
                                                 bpeA,
                                                 bpeB,
                                                 depthU,
                                                 waveNum,
                                                 MT0,
                                                 MT1,
                                                 hw_consts.math_frequency,
                                                 prefetch_mem,
                                                 numAccPerWave);
        double preLoopCost = hw_consts.initialCost + prefetch;

        // 11. Calculate loop Performance
        double math_overall = math_clk / hw_consts.math_frequency;
        double loop_overall = getLoopOverall(mem_costs, math_overall, loopCnt, PGR);

        // 12. Aggregate Performance: pre-loop + unrolled-loop + post-loop
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

        // 13. Handle Tail Loop
        double tail_overall = 0.0;
        if (K_tail > 0)
        {
            // FIXME: need to add new opt.
            tail_overall = (mem_costs.mem_hbm + math_overall);
            perf += tail_overall;
        }

        // 14. Apply CU Occupancy
        perf = resolveOccupancy(hw_consts, perf, prefetch, loop_overall + tail_overall, store, num_tiles, CUOccupancy);

        // 15. Add LSU+GSU Reduction Part
        perf += (gsu_overall + lsu_overall);

        pp.microSeconds = perf;
        pp.hitRate = cache_hits.totalL2HitRate * 100;

        perfInfo.memory = mem_costs;
        perfInfo.math = math_overall;
        perfInfo.svw = GWVWD;
        perfInfo.perf = perf;
        perfInfo.preloop = preLoopCost;
        perfInfo.loop = loop_overall;
        perfInfo.tail = tail_overall;
        perfInfo.store = store;
        perfInfo.gsu = gsu_overall;
        perfInfo.lsu = lsu_overall;
        perfInfo.mt0 = MT0;
        perfInfo.mt1 = MT1;
        perfInfo.du = depthU;

#if 0
        std::cout<<"MT0               =          "<<MT0<<std::endl;
        std::cout<<"MT1               =          "<<MT1<<std::endl;
        std::cout<<"depthU            =          "<<depthU<<std::endl;
        std::cout<<"NumCUs            =          "<<hw_consts.NumCUs<<std::endl;
        std::cout<<"WorkGroupMapping  =          "<<WGM<<std::endl;
        std::cout<<"CUOccupancy       =          "<<CUOccupancy<<std::endl;
        std::cout<<"GlobalSplitU      =          "<<GlobalSplitU<<std::endl;
        std::cout<<"loopCnt           =          "<<loopCnt<<std::endl;
        std::cout<<"flopsPerClk       =          "<<hw_consts.flopsPerClk<<std::endl;
        std::cout<<"A_L1_req          =          "<<mem_costs.A_L1_req<<std::endl;
        std::cout<<"B_L1_req          =          "<<mem_costs.B_L1_req<<std::endl;
        std::cout<<"A_L2_req          =          "<<mem_costs.A_L2_req<<std::endl;
        std::cout<<"B_L2_req          =          "<<mem_costs.B_L2_req<<std::endl;
        std::cout<<"A_L1_hit          =          "<<cache_hits.A_L1_hit<<std::endl;
        std::cout<<"B_L1_hit          =          "<<cache_hits.B_L1_hit<<std::endl;
        std::cout<<"A_L2_hit          =          "<<cache_hits.A_L2_hit<<std::endl;
        std::cout<<"B_L2_hit          =          "<<cache_hits.B_L2_hit<<std::endl;
        std::cout<<"overall L2 Hit    =          "<<cache_hits.totalL2HitRate<<std::endl;
        std::cout<<"A_L3_hit          =          "<<cache_hits.A_L3_hit<<std::endl;
        std::cout<<"B_L3_hit          =          "<<cache_hits.B_L3_hit<<std::endl;
        std::cout<<"math_clk          =          "<<math_clk<<std::endl;
        std::cout<<"mem_l1            =          "<<mem_costs.mem_l1<<std::endl;
        std::cout<<"mem_l2            =          "<<mem_costs.mem_l2<<std::endl;
        std::cout<<"mem_l3            =          "<<mem_costs.mem_l3<<std::endl;
        std::cout<<"mem_hbm           =          "<<mem_costs.mem_hbm<<std::endl;
        std::cout<<"math_overall      =          "<<math_overall<<std::endl;
        std::cout<<"tail_overall      =          "<<tail_overall<<std::endl;
        std::cout<<"M_WGs_total       =          "<<M_WGs_total<<std::endl;
        std::cout<<"N_WGs_total       =          "<<N_WGs_total<<std::endl;
        std::cout<<"K_tail            =          "<<K_tail<<std::endl;
        std::cout<<"loop_overall      =          "<<loop_overall<<std::endl;
        std::cout<<"preLoopCost       =          "<<preLoopCost<<std::endl;
        std::cout<<"prefetch          =          "<<prefetch<<std::endl;
        std::cout<<"store             =          "<<store<<std::endl;
        std::cout<<"gsu_overall       =          "<<gsu_overall<<std::endl;
        std::cout<<"lsu_overall       =          "<<lsu_overall<<std::endl;
        std::cout<<"num_tiles         =          "<<num_tiles<<std::endl;
        std::cout<<"=================="<<perf<<" us"<<std::endl;
#endif
        return pp;
    }

    Formocast::L2CacheHitRate
        Formocast::computeL2CacheHitRate(uint32_t M,
                                                   uint32_t N,
                                                   uint32_t K,
                                                   const HardwareConstants& hw,
                                                   uint32_t gsu,
                                                   int32_t  wgm,
                                                   uint32_t batches,
                                                   uint32_t bpeA,
                                                   uint32_t bpeB,
                                                   int32_t  NTA,
                                                   int32_t  NTB,
                                                   bool     isGSUWGMRR) const
    {
        uint32_t MT0 = sizeMapping.macroTile[0];
        uint32_t MT1 = sizeMapping.macroTile[1];
        uint32_t depthU = sizeMapping.depthU;

        auto hr = Simulator::computeL2CacheHitRate(
            M, N, K, MT0, MT1, depthU, hw.L2CacheCapacity, hw.NumCUs, hw.NumXCDs,
            gsu, wgm, batches, bpeA, bpeB, NTA, NTB, isGSUWGMRR);
        
        L2CacheHitRate hitRate;
        hitRate.totalHitRate = hr.totalHitRate;
        hitRate.tile0HitRate = hr.tile0HitRate;
        hitRate.tile1HitRate = hr.tile1HitRate;

        return hitRate;
    }

    void Formocast::setProblem(ProblemInfo p)
    {
        problem = p;
    }

    void Formocast::setSolution(SizeMapping sm)
    {
        sizeMapping = sm;
    }

    void Formocast::setHardware(std::shared_ptr<origami::hardware_t> hw)
    {
        hardware = hw;
    }

    int Formocast::checkGlobalReadFIFOFull(int currentCycle, std::queue<int>& fifo, int bpRead, int numWaves, bool isStall) const
    {
        return Simulator::checkGlobalReadFIFOFull(currentCycle, fifo, bpRead, numWaves, isStall);
    }

    int Formocast::checkLocalReadFinished(int currentCycle, std::queue<int>& fifo, int numLR) const
    {
        return Simulator::checkLocalReadFinished(currentCycle, fifo, numLR);
    }

    int Formocast::checkLocalReadFIFOFull(int currentCycle, std::queue<int>& fifo, int bpRead, int numWaves, bool isStall) const
    {
        return Simulator::checkLocalReadFIFOFull(currentCycle, fifo, bpRead, numWaves, isStall);
    }

    void Formocast::pushLocalRead(int currentCycle, std::queue<int>& fifo, int bpr, bool isGfx950)
    {
        Simulator::pushLocalRead(currentCycle, fifo, bpr, isGfx950);
    }
} // namespace Tensilelite
