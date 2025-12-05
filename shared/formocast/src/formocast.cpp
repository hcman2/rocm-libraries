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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <cassert>
#include <iostream>
#include <cstring>

namespace Tensilelite
{
    static uint32_t ceilDivide(uint32_t numerator, uint32_t denominator) {
        if (denominator == 0) {
            throw std::invalid_argument("Denominator cannot be zero");
        }
        return (numerator + denominator - 1) / denominator;
    }

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

    static double ceiling_math(double value, double significance = 1)
    {
        return std::ceil(value / significance) * significance;
    }

    static double calculateStoreL3Request(
        double M, double N, double MT0, double MT1, double& non_edge_req, double& edge_req)
    {
        double result = 0.0;

        double edge_size     = std::fmod(M, MT0);
        double numWGsNonEdge = std::floor(M / MT0);
        result = N * ((numWGsNonEdge * ceiling_math(MT0 / 32)) + ceiling_math(edge_size / 32));

        double maxMT1              = std::min(N, MT1);
        double nonEdgeRequestPerMT = maxMT1 * (ceiling_math(MT0 / 32));
        double edgeRequestPerMT    = maxMT1 * ceiling_math(edge_size / 32);
        if(numWGsNonEdge > 0.0)
            non_edge_req = nonEdgeRequestPerMT;
        else
            non_edge_req = 0;
        edge_req = edgeRequestPerMT;

        return result;
    }

    static double calculateStoreL2Request(double  M,
                                          double  N,
                                          double  MT0,
                                          double  MT1,
                                          double  SVW,
                                          double& non_edge_req,
                                          double& edge_req)
    {
        double result = 0.0;

        double edge_size     = std::fmod(M, MT0);
        double numWGsNonEdge = std::floor(M / MT0);
        double M_MOD_16SVW   = std::fmod(M, 16 * SVW);

        double non_edge_0 = MT0 * 2 / 64 * ceiling_math(64 / (16 * 2 * SVW));
        double edge_0     = (ceiling_math(std::floor(edge_size / (16 * SVW)) * (16 * SVW) * 2 / 64
                                      * ceiling_math(64 / (16 * 2 * SVW)))
                         * SVW);
        double edge_1     = (std::floor(M_MOD_16SVW * 2 / 64 * ceiling_math(64 / (16 * 2 * SVW)))
                         * std::min(M_MOD_16SVW, SVW));
        double edge_2     = (std::min(std::fmod(M, std::min(16 * SVW, 32.0)), SVW));

        result = N * ((numWGsNonEdge * non_edge_0) + (edge_0) + (edge_1) + (edge_2));

        double maxMT1              = std::min(N, MT1);
        double nonEdgeRequestPerMT = maxMT1 * (non_edge_0);
        double edgeRequestPerMT    = maxMT1 * (edge_0 + edge_1 + edge_2);
        if(numWGsNonEdge > 0.0)
            non_edge_req = nonEdgeRequestPerMT;
        else
            non_edge_req = 0;
        edge_req = edgeRequestPerMT;

        return result;
    }

    static double calculateStoreL1Request(double  M,
                                          double  N,
                                          double  MT0,
                                          double  MT1,
                                          double  SVW,
                                          double& non_edge_req,
                                          double& edge_req)
    {
        double result = 0.0;

        double edge_size     = std::fmod(M, MT0);
        double numWGsNonEdge = std::floor(M / MT0);

        double non_edge_0
            = MT0 / 16 * (-1) * (SVW == 1 ? 1 : 0) * (std::fmod(M, 4) == 2 ? 1 : 0);
        double non_edge_1
            = MT0 / 16 * (-4) * (SVW == 1 ? 1 : 0) * (std::fmod(M, 16) == 8 ? 1 : 0);
        double non_edge_2
            = MT0 / 16 * (-3) * (SVW == 1 ? 1 : 0) * (std::fmod(M, 4) == 0 ? 1 : 0);
        double non_edge_3
            = MT0 / 16 * (-12) * (SVW == 1 ? 1 : 0) * (std::fmod(M, 16) == 0 ? 1 : 0);
        double edge_0 = (std::floor(edge_size / (16 * SVW)) * 3 * (SVW == 1 ? 1 : 0)
                         * (std::fmod(M, 2) == 1 ? 1 : 0));
        double edge_1 = (std::floor(edge_size / (16 * SVW)) * 2 * (SVW == 1 ? 1 : 0)
                         * (std::fmod(M, 4) == 2 ? 1 : 0));
        double edge_2 = (std::floor(edge_size / (16 * SVW)) * (-4) * (SVW == 1 ? 1 : 0)
                         * (std::fmod(M, 8) == 0 ? 1 : 0));
        double edge_3 = (std::floor(edge_size / (16 * SVW)) * 4 * (SVW == 1 ? 1 : 0)
                         * (std::fmod(M, 16) == 0 ? 1 : 0));
        double edge_4 = (std::floor(edge_size / (16 * SVW)) * (-12 * SVW * SVW)
                         * (SVW == 1 ? 1 : 0) * (std::fmod(M, 16) == 0 ? 1 : 0));

        result += N / 64
                  * ((numWGsNonEdge * non_edge_0) + (numWGsNonEdge * non_edge_1)
                     + (numWGsNonEdge * non_edge_2) + (numWGsNonEdge * non_edge_3) + (edge_0)
                     + (edge_1) + (edge_2) + (edge_3) + (edge_4));

        double non_edge_4 = MT0 / 32 * (SVW == 2 || SVW == 8 ? 139 : (SVW == 4 ? 82 : 0))
                            * (SVW == 1 ? 0 : 1) * (std::fmod(M, 2) == 1 ? 1 : 0);
        double non_edge_5 = MT0 / 16 * (SVW == 2 || SVW == 8 ? 3 : (SVW == 4 ? 2 : 0))
                            * (SVW == 1 ? 0 : 1) * (std::fmod(M, 4) == 2 ? 1 : 0);
        double non_edge_6 = MT0 / 16 * (SVW == 2 || SVW == 8 ? 2 : (SVW == 4 ? 0 : 0))
                            * (SVW == 1 ? 0 : 1) * (std::fmod(M, 8) == 4 ? 1 : 0);
        double non_edge_7 = MT0 / 16 * (SVW == 2 || SVW == 8 ? 4 : (SVW == 4 ? 0 : 0))
                            * (SVW == 1 ? 0 : 1) * (std::fmod(M, 16) == 8 ? 1 : 0);
        double non_edge_8
            = MT0 / 16 * (-4) * (SVW == 1 ? 0 : 1) * (std::fmod(M, 8) == 0 ? 1 : 0);
        double non_edge_9 = MT0 / 16 * (SVW == 4 || SVW == 8 ? 0 : (SVW == 2 ? -8 : 0))
                            * (SVW == 1 ? 0 : 1) * (std::fmod(M, 16 * SVW) == 0 ? 1 : 0);
        double non_edge_10 = MT0 / 16 * (SVW == 4 || SVW == 8 ? -8 : (SVW == 2 ? 0 : 0))
                             * (SVW == 1 ? 0 : 1) * (std::fmod(M, 4 * SVW) == 0 ? 1 : 0);
        double edge_5 = (std::floor(edge_size / (16 * SVW)) * (-16 * SVW * SVW)
                         * (SVW == 1 ? 0 : 1) * (std::fmod(M, 16 * SVW) == 8 * SVW ? 1 : 0));
        double edge_6 = (std::floor(edge_size / (16 * SVW)) * (-48 * SVW * SVW)
                         * (SVW == 1 ? 0 : 1) * (std::fmod(M, 16 * SVW) == 0 ? 1 : 0));

        result += N / 64
                  * ((numWGsNonEdge * non_edge_4) + (numWGsNonEdge * non_edge_5)
                     + (numWGsNonEdge * non_edge_6) + (numWGsNonEdge * non_edge_7)
                     + (numWGsNonEdge * non_edge_8) + (numWGsNonEdge * non_edge_9)
                     + (numWGsNonEdge * non_edge_10) + (edge_5) + (edge_6));

        double M_MOD_16SVW = std::fmod(M, 16 * SVW);
        double M_MOD_8VW   = std::fmod(M, 8 * SVW);
        double M_MOD_4SVW  = std::fmod(M, 4 * SVW);
        double M_MOD_4     = std::fmod(M, 4);

        double non_edge_11 = MT0 / 16 * (16 - (SVW == 1 ? 1 : 4));
        double edge_7
            = (std::floor(edge_size / (16 * SVW)) * (12 * SVW * SVW) * (SVW == 1 ? 1 : 4));
        double edge_8 = ((M_MOD_16SVW >= 4 * SVW
                              ? (M_MOD_16SVW - 4 * SVW) * SVW
                                    * (SVW == 1 && M_MOD_4 == 0 ? 1 : 4) * (M_MOD_8VW == 0 ? 0 : 1)
                              : 0));

        result += N / 64 * ((numWGsNonEdge * non_edge_11) + (edge_7) + (edge_8));

        double non_edge_12 = ((MT0 * 2) / 64) * (SVW == 1 || SVW == 4 ? 2 : 1);
        double edge_9      = (std::floor(edge_size / (16 * SVW)) * (SVW == 1 ? 1 : 4 * SVW));
        double edge_10     = (M_MOD_16SVW < 4 * SVW ? M_MOD_4SVW : 0);
        double edge_11 = (M_MOD_16SVW >= 4 * SVW ? (SVW == 1 && M_MOD_4 == 0 ? 1 : 4 * SVW) : 0);

        result += N * ((numWGsNonEdge * non_edge_12) + (edge_9) + (edge_10) + (edge_11));

        double maxMT1              = std::min(N, MT1);
        double nonEdgeRequestPerMT = maxMT1 / 64
                                         * (non_edge_0 + non_edge_1 + non_edge_2 + non_edge_3
                                            + non_edge_4 + non_edge_5 + non_edge_6 + non_edge_7
                                            + non_edge_8 + non_edge_9 + non_edge_10 + non_edge_11)
                                     + (maxMT1 * non_edge_12);
        double edgeRequestPerMT
            = maxMT1 / 64
                  * (edge_0 + edge_1 + edge_2 + edge_3 + edge_4 + edge_5 + edge_6 + edge_7 + edge_8)
              + maxMT1 * (edge_9 + edge_10 + edge_11);

        if(numWGsNonEdge > 0.0)
            non_edge_req = nonEdgeRequestPerMT;
        else
            non_edge_req = 0;
        edge_req = edgeRequestPerMT;

        return result;
    }

    static double getLoadRequest(double   MTX,
                                 double   DU,
                                 double   L1CacheLineSize,
                                 uint32_t grvw,
                                 uint32_t bpe,
                                 int      dtv,
                                 bool     isSwizzledA,
                                 uint32_t VW,
                                 double&  tcc_ea0_coalscedA)
    {
        double L1_req     = 0.0;
        tcc_ea0_coalscedA = 1;
        if(isSwizzledA)
        {
            L1_req = MTX * DU * bpe / 64;
            L1_req *= ceilDivide(VW, uint32_t(2));
        }
        else
        {
            L1_req = MTX * DU * bpe / 64;
            tcc_ea0_coalscedA = ceilDivide((uint32_t)L1CacheLineSize, (uint32_t)DU * bpe);
            if(grvw * bpe == 8 || grvw * bpe <= 2)
                L1_req *= 2;
            if(dtv)
                L1_req *= 4;
        }
        return L1_req;
    }

    static double getLoadRequestAisNOrBisT(double   MTX,
                                 double   DU,
                                 double   L1CacheLineSize,
                                 double   L1BusWidthPerCU,
                                 uint32_t grvw,
                                 uint32_t bpe,
                                 int      dtv,
                                 int      NumLoadsCoalesced,
                                 uint32_t numWaveX,
                                 double&  tcc_ea0_coalsced)
    {
        double L1_req     = 0.0;
        tcc_ea0_coalsced = 1.0;
        L1_req = std::ceil(MTX / NumLoadsCoalesced * bpe / L1BusWidthPerCU) * NumLoadsCoalesced * DU;
        if(L1CacheLineSize > MTX / NumLoadsCoalesced * bpe)
            tcc_ea0_coalsced = 2;
        if((grvw * bpe == 8 || grvw * bpe <= 2) && (MTX / NumLoadsCoalesced * bpe >= L1BusWidthPerCU))
            L1_req *= 2;
        if(dtv)
            L1_req *= numWaveX;

        return L1_req;
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
            = calculateStoreL1Request(M, N, MT0, MT1, GWVWD, D_L1_req, D_L1_edge_req);
        double total_store_req2
            = calculateStoreL2Request(M, N, MT0, MT1, GWVWD, D_L2_req, D_L2_edge_req);
        double total_store_req3 = calculateStoreL3Request(M, N, MT0, MT1, D_L3_req, D_L3_edge_req);

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
        L1CacheHitRate hr;
        double A_L1_hit = 1.0;
        double B_L1_hit = 1.0;

        // Calculate L1 hit rate, assume bpeA==bpeB, TN only
        bool isL1BypassA = (NTA >= 2);
        bool isL1BypassB = (NTB >= 2);

        if(transA)
        {
            //A is T
            if(GRVWA * bpeA == 8 || GRVWA * bpeA <= 2)
                A_L1_hit *= 2;
            if(DTVA)
                A_L1_hit *= 4;
            A_L1_hit = isL1BypassA ? 0: 1 - 1 / A_L1_hit;
            A_L1_hit = isSwizzleA ? 1 - 1 / ceilDivide(VWA, uint32_t(2)) : A_L1_hit;
        }
        else
        {
            //A is N
            uint32_t L1Limit = hw.L1CacheCapacity;
            if((uint32_t)lda % 512 == 0)
                L1Limit /= 4;
            else if((uint32_t)lda % 256 == 0)
                L1Limit /= 2;

            if(MT0 / NLCA * bpeA < hw.L1CacheLineSize)
            {
                bool isL1CacheFull = (hw.L1CacheLineSize * threadnum / (MT0 / NLCA / GRVWA)) > L1Limit;
                if(isL1CacheFull)
                {
                    A_L1_hit = 1;
                }
                else
                {
                    A_L1_hit = std::ceil(NLCA / (hw.L1CacheLineSize / (MT0 / NLCA * bpeA))) / NLCA;
                }
            }
            else
            {
                A_L1_hit = 0.5; //Why not 1.0?
            }
            if((GRVWA * bpeA == 8 || GRVWA * bpeA <= 2) && (MT0 / NLCA * bpeA) >= hw.L1BusWidthPerCU)
                A_L1_hit /= 2;
            if(DTVA)
                A_L1_hit /= NumWave0;
            A_L1_hit = isL1BypassA ? 0: 1 - A_L1_hit;
        }

        if(transB)
        {
            //B is T
            uint32_t L1Limit = hw.L1CacheCapacity;
            if((uint32_t)ldb % 512 == 0)
                L1Limit /= 4;
            else if((uint32_t)ldb % 256 == 0)
                L1Limit /= 2;

            if(MT1 / NLCB * bpeB < hw.L1CacheLineSize)
            {
                bool isL1CacheFull = (hw.L1CacheLineSize * threadnum / (MT1 / NLCB / GRVWB)) > L1Limit;
                if(isL1CacheFull)
                {
                    B_L1_hit = 1;
                }
                else
                {
                    B_L1_hit = std::ceil(NLCB / (hw.L1CacheLineSize / (MT1 / NLCB * bpeB))) / NLCB;
                }
            }
            else
            {
                B_L1_hit = 0.5; //Why not 1.0?
            }
            if((GRVWB * bpeB == 8 || GRVWB * bpeB <= 2) && (MT1 / NLCB * bpeB) >= hw.L1BusWidthPerCU)
                B_L1_hit /= 2;
            if(DTVB)
                B_L1_hit /= NumWave1;
            B_L1_hit = isL1BypassB ? 0: 1 - B_L1_hit;
        }
        else
        {
            //B is N
            if(GRVWB * bpeB == 8 || GRVWB * bpeB <= 2)
                B_L1_hit *= 2;
            if(DTVB)
                B_L1_hit *= 4;
            B_L1_hit = isL1BypassB ? 0: 1 - 1 / B_L1_hit;
            B_L1_hit = isSwizzleB ? 1 - 1 / ceilDivide(VWB, uint32_t(2)) : B_L1_hit;
        }

        hr.tile0HitRate = A_L1_hit;
        hr.tile1HitRate = B_L1_hit;
        return hr;
    }

    Formocast::L3CacheHitRate
    Formocast::computeL3CacheHitRate(double M, double N, double K, const HardwareConstants& hw,
                                          uint32_t bpeA, uint32_t bpeB, int NTA, int NTB,
                                          int N_WGs_total, int M_WGs_total, int N_WGs_per_tile, int M_WGs_per_tile) const
    {
        L3CacheHitRate hr;
        double A_L3_hit = 0.0;
        double B_L3_hit = 0.0;

        bool isL3BypassA = (NTA > 3) || (NTA == 1);
        if(!isL3BypassA)
        {
            if((M * K * bpeA) + (N * K * bpeB) < hw.L3CacheCapacity)
            {
                A_L3_hit = 1 - double(1.0 / N_WGs_total);
            }
            else
            {
                A_L3_hit = 1 - double(M_WGs_per_tile / hw.NumCUs);
            }
        }
        bool isL3BypassB = (NTB > 3) || (NTB == 1);
        if(!isL3BypassB)
        {
            if((M * K * bpeA) + (N * K * bpeB) < hw.L3CacheCapacity)
            {
                B_L3_hit = 1 - double(1.0 / M_WGs_total);
            }
            else
            {
                B_L3_hit = 1 - double(N_WGs_per_tile / hw.NumCUs);
            }
        }
        hr.tile0HitRate = A_L3_hit;
        hr.tile1HitRate = B_L3_hit;
        hr.totalHitRate = A_L3_hit * M/(M+N) + B_L3_hit * N/(M+N);
        return hr;
    }

    double Formocast::calculateGSUOverhead(double M, double N, double K,
                                                        double NumBatches, double GlobalSplitU,
                                                        uint32_t gsuMethod, ProblemInfo problem,
                                                        const HardwareConstants& hw_consts,
                                                        uint32_t WGs_per_tile, uint32_t WGs_per_tile_XCD,
                                                        double MT0, double MT1, uint32_t numWGs, double vgprCheck,
                                                        double storeGSU) const
    {
        double   gsu_overall = 0.0;
        if(gsuMethod == 2 && GlobalSplitU > 1) //MB
        {
            // double GSU_load = (GlobalSplitU * M * N * 4 * NumBatches) / hbmBandWidth / NumCUs;
            // double GSU_store = (M * N * bpeD * NumBatches) / hbmBandWidth / NumCUs;
            // gsu_overall = initialCost + GSU_load + GSU_store;
            double read_l1_req, write_l1_req;

            auto   bpeIn    = problem.bpeCompute;
            auto   bpeOut   = problem.bpeD;
            // Read write requests for VW=4 dwordx2 Half output
            if (bpeIn == 4 && bpeOut == 2 && ((int)M % 4) == 0)
            {
                read_l1_req = M * N * (bpeIn * 4 + (GlobalSplitU - 2) * 4 + 2) / 64;
                write_l1_req = M * N * bpeOut / 64 * 8;
            }
            else
            {
                // Just to make it work
                read_l1_req = M * N * (bpeIn * 4 + (GlobalSplitU - 2) * 4 + 2) / 64;
                write_l1_req = M * N * bpeOut / 64 * 8;
                std::cerr << "Currently not support yet" << std::endl;
            }

            double read_l2_req = M * N * bpeIn * GlobalSplitU / hw_consts.L1CacheLineSize;
            double read_l3_req = read_l2_req;
            double write_l2_req = M * N * bpeOut / 64;
            double write_l3_req = write_l2_req;

            double WGs = M * N / (1024);
            WGs = std::min(double(hw_consts.NumCUs), WGs);

            //FIXME: should get this from hw_consts
            double cu_freq  = hw_consts.boost_frequency;
            double hbm_freq = hw_consts.mem_frequency;

            // Not sure if this works using the same formula as gemm
            double L2WriteBandWidthPerCU = hw_consts.L2WriteArbEff * 128 * 16 / WGs; //58% eff
            double L2BandWidthPerCU      = hw_consts.L2ReadArbEff * 128 * 16 / WGs; //90% eff
            double L3BandWidthPerCU      = hw_consts.L3BandWidth / WGs;
            double HBMBandWidthPerCU     = hw_consts.hbmBandWidth / WGs;

            double GSU_L1_clk  = read_l1_req/WGs * 64 / hw_consts.L1BusWidthPerCU;
            double GSU_L2_clk  = read_l2_req/WGs / 2 * 128 / std::min(L2BandWidthPerCU, hw_consts.L2BusWidthPerCU);
            double GSU_L3_clk  = read_l3_req/WGs / 2 * 128 / L3BandWidthPerCU;
            double GSU_hbm_clk = M * N * bpeIn * GlobalSplitU / hw_consts.hbmBandWidth; //read_req / 2 * 128 / HBMBandWidthPerCU;

            double GSU_L1_overall  = GSU_L1_clk / cu_freq;
            double GSU_L2_overall  = GSU_L2_clk / cu_freq;
            double GSU_L3_overall  = GSU_L3_clk / hbm_freq;
            double GSU_hbm_overall = GSU_hbm_clk / hbm_freq;
            double GSU_mem_overall = std::max(GSU_hbm_overall, std::max(GSU_L3_overall, std::max(GSU_L1_overall, GSU_L2_overall)));

            double D_L1_clk = write_l1_req/WGs * 64 / hw_consts.L1WriteBusWidthPerCU;
            double D_L2_clk = write_l2_req/WGs * 64 / std::min(hw_consts.L2WriteBusWidthPerCU, L2WriteBandWidthPerCU);
            double D_L3_clk = write_l3_req/WGs * 64 / L3BandWidthPerCU;
            // TODO: D_hbm_clk use D_L3_req.
            double D_hbm_clk     = 0 * 64 / HBMBandWidthPerCU;
            double store    = std::max(D_L1_clk/cu_freq, std::max(D_L2_clk/cu_freq, std::max(D_L3_clk/hbm_freq, D_hbm_clk/hbm_freq)));

            gsu_overall = GSU_mem_overall + store;
        }
        else if(gsuMethod == 3 && GlobalSplitU > 1) //MBSK
        {
            // FIXME: Modify with the MBSK changes.
            // FIXME: add sync overhead.
            double cu_freq  = hw_consts.boost_frequency;
            auto   bpeIn    = problem.bpeCompute;
            double WGs = std::min(double(hw_consts.NumCUs), double(numWGs)) / GlobalSplitU;
            double L2BandWidthPerCU      = hw_consts.L2ReadArbEff * 128 * 16 / WGs; //90% eff
            double atomic_overhead = GlobalSplitU * 0.1;
            double GSU_L1_req      = ((GlobalSplitU - 1) * MT0 * MT1 * bpeIn) / 64;
            if (GlobalSplitU > 2)
            {
                GSU_L1_req += (MT0 * MT1 * bpeIn) / 64;
            }
            double GSU_L1_clk      = GSU_L1_req * 64 / hw_consts.L1BusWidthPerCU;
            double GSU_L2_clk = GSU_L1_req / 2 * 128 / std::min(L2BandWidthPerCU, hw_consts.L2BusWidthPerCU);

            gsu_overall       = atomic_overhead + (std::max(GSU_L1_clk/cu_freq, GSU_L2_clk/cu_freq)) + storeGSU;
        }

        return gsu_overall;
    }

    double Formocast::calculateLSUOverhead(double MT0, double MT1, double lsu,
                                                     uint32_t svw, uint32_t numThreads,
                                                     ProblemInfo problem,
                                                     const HardwareConstants& hw_consts) const
    {
        if (lsu == 1) return 0.0;
        double lsu_overall = 0.0;
        auto   bpeIn       = problem.bpeCompute;

        // local write
        double lw_cycle    = 8;
        double local_write = MT0 * MT1 / numThreads; // total elements to store per thread.
        double local_write_cycle;
        switch(svw * bpeIn)
        {
        case 32:
            lw_cycle  = 20;
            local_write_cycle = local_write * lw_cycle * 2;
            break;
        case 16:
            lw_cycle  = 20;
            local_write_cycle = local_write * lw_cycle;
            break;
        case 8:
            lw_cycle  = 12;
            local_write_cycle = local_write * lw_cycle;
            break;
        case 4:
            lw_cycle  = 8;
            local_write_cycle = local_write * lw_cycle;
            break;
        default:
            lw_cycle  = 8;
            local_write_cycle = local_write * lw_cycle;
            break;
        }
        local_write_cycle *= 2;

        // local read
        double local_read_cycle = MT0 * MT1 / numThreads / svw * 4;
        local_read_cycle *= 2;

        // reduction
        double reduction_cycle = MT0 * MT1 / numThreads * (lsu - 1) * 4;

        lsu_overall = (local_write_cycle + local_read_cycle + reduction_cycle) / hw_consts.math_frequency;

        return lsu_overall;
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
        double A_L1_req;
        double B_L1_req;
        if(trA)
        {
            A_L1_req
                = getLoadRequest(MT0, depthU, hw.L1CacheLineSize, GRVWA, bpeA, DTVA, isSwizzleA, VWA, tcc_ea0_coalscedA);
        }
        else
        {
            A_L1_req
                = getLoadRequestAisNOrBisT(MT0, depthU, hw.L1CacheLineSize, hw.L1BusWidthPerCU, GRVWA, bpeA, DTVA, NLCA, numWave1, tcc_ea0_coalscedA);
        }
        if(trB)
        {
            B_L1_req
                = getLoadRequestAisNOrBisT(MT1, depthU, hw.L1CacheLineSize, hw.L1BusWidthPerCU, GRVWB, bpeB, DTVB, NLCB, numWave0, tcc_ea0_coalscedB);
        }
        else
        {
            B_L1_req
                = getLoadRequest(MT1, depthU, hw.L1CacheLineSize, GRVWB, bpeB, DTVB, isSwizzleB, VWB, tcc_ea0_coalscedB);
        }

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
        Formocast::L2CacheHitRate hitRate;

        uint32_t MT0 = sizeMapping.macroTile[0];
        uint32_t MT1 = sizeMapping.macroTile[1];

        uint32_t wg0 = ceilDivide(M, MT0);
        uint32_t wg1 = ceilDivide(N, MT1);

        uint32_t MT0_Edge = MT0 - ((wg0 * MT0) - M);
        uint32_t MT1_Edge = MT1 - ((wg1 * MT1) - N);
        if(MT0_Edge == 0)
            MT0_Edge = MT0;
        if(MT1_Edge == 0)
            MT1_Edge = MT1;

        //std::cout<<"wgm="<<wgm<<",wg0="<<wg0<<", wg1 = "<<wg1<<", MT0 = "<<MT0<<", MT1 = "<<MT1<<", MT0_edge = "<<MT0_Edge<<", MT1_edge = "<<MT1_Edge<<std::endl;

        // other info
        uint32_t L2CacheLineSize = 128; //Bytes
        uint32_t L2Capacity      = hw.L2CacheCapacity; //MBs
        uint32_t depthU          = sizeMapping.depthU;
        uint32_t gsuMulBatch     = gsu * batches;

        std::vector<uint32_t> arrA(gsuMulBatch * wg0, 0);
        std::vector<uint32_t> arrB(gsuMulBatch * wg1, 0);
        std::vector<uint32_t> arrA_2(gsuMulBatch * wg0, 0);
        std::vector<uint32_t> arrB_2(gsuMulBatch * wg1, 0);

        uint32_t WGMXCC  = hw.NumXCDs;
        uint32_t WGMXCCG = hw.NumCUs;
        assert((WGMXCCG % WGMXCC) == 0);

        uint32_t xccIdx      = 0;
        uint32_t score       = 0;
        uint32_t totalWGNum  = gsuMulBatch * wg0 * wg1;
        uint32_t totalWG0WG1 = wg0 * wg1;
        uint32_t xccgdiv     = totalWGNum / WGMXCCG;
        uint32_t xccgres     = totalWGNum % WGMXCCG;

        double  hitRateA     = 0;
        double  hitRateB     = 0;
        double  totalHitRate = 0;
        int32_t finalwgm     = 0;

        double aRatio = float(MT0) / float(MT0 + MT1);
        double bRatio = float(MT1) / float(MT0 + MT1);

        uint64_t aHitElements  = 0;
        uint64_t aMissElements = 0;
        uint64_t bHitElements  = 0;
        uint64_t bMissElements = 0;

        bool isL2BypassA = (NTA & 0x6) > 0;
        bool isL2BypassB = (NTB & 0x6) > 0;

        //std::cout<<"GSU="<<gsu<<", batch="<<batches<<", isL2BypassA = "<<isL2BypassA<<", isL2BypassB = "<<isL2BypassB<<std::endl;

        uint32_t hitA  = 0;
        uint32_t hitB  = 0;
        uint32_t missA = 0;
        uint32_t missB = 0;

        for(uint32_t wg = 0; wg < std::min(totalWGNum, 10 * (uint32_t)hw.NumCUs); wg++)
        {
            //clean cache
            if((wg % WGMXCCG) == 0)
            {
                //loop every XCDs
                for(uint32_t xcd = 0; xcd < hw.NumXCDs && wg > 0; xcd++)
                {
                    uint32_t MT0_A = 0;
                    for(uint32_t g = 0; g < gsuMulBatch; g++)
                    {
                        for(uint32_t i = 0; i < wg0; i++)
                        {
                            if(arrA[g * wg0 + i] & (1 << xcd))
                            {
                                if(i == (wg0 - 1)) //Edge
                                    MT0_A += (MT0_Edge * (K / gsu)) * bpeA;
                                else
                                    MT0_A += (MT0 * (K / gsu)) * bpeA;
                            }
                        }
                    }

                    uint32_t MT1_B = 0;
                    for(uint32_t g = 0; g < gsuMulBatch; g++)
                    {
                        for(uint32_t i = 0; i < wg1; i++)
                        {
                            if(arrB[g * wg1 + i] & (1 << xcd))
                            {
                                if(i == (wg1 - 1)) //Edge
                                    MT1_B += (MT1_Edge * (K / gsu)) * bpeB;
                                else
                                    MT1_B += (MT1 * (K / gsu)) * bpeB;
                                //std::cout<<"i,g,MT1_B:"<<i<<","<<g<<","<<MT1_B<<std::endl;
                            }
                        }
                    }
                    //std::cout<<"XCD:"<<xcd<<", next round("<<wg<<"): L2Capacity="<<L2Capacity<<", A:"<<MT0_A<<", B:"<<MT1_B<<std::endl;
                    if(MT0_A + MT1_B <= L2Capacity)
                    {
                        //keep in cache
                        //std::cout<<"keep in cache"<<std::endl;
                        for(uint32_t g = 0; g < gsuMulBatch; g++)
                            for(uint32_t i = 0; i < wg0; i++)
                                arrA_2[g * wg0 + i] |= arrA[g * wg0 + i] & (1 << xcd);
                        for(uint32_t g = 0; g < gsuMulBatch; g++)
                            for(uint32_t i = 0; i < wg1; i++)
                                arrB_2[g * wg1 + i] |= arrB[g * wg1 + i] & (1 << xcd);
                    }
                    else
                    {
                        //clean cache
                        //std::cout<<"clean cache"<<std::endl;
                        arrA_2.assign(wg0 * gsuMulBatch, 0);
                        arrB_2.assign(wg1 * gsuMulBatch, 0);
                    }
                }

                arrA.assign(wg0 * gsuMulBatch, 0);
                arrB.assign(wg1 * gsuMulBatch, 0);
            }

            // go xccgroup
            //std::cout<<"go xccgroup";
            uint32_t xccgIdx  = wg / WGMXCCG;
            uint32_t realWGId = xccgIdx * WGMXCCG;

            // get xccgroup wgNum
            //std::cout<<"get xccgroup wgNum";
            uint32_t xccgWgNum = std::min(WGMXCCG, totalWGNum - realWGId);
            // how many wg per xcc in this xccgroup
            uint32_t xccunit = xccgWgNum / WGMXCC;
            uint32_t xccres  = xccgWgNum % WGMXCC;
            // starting wgId
            uint32_t resWGId = (wg - realWGId) % xccgWgNum;

            // go xcc
            //std::cout<<"go xcc";
            uint32_t xccIdx = resWGId % WGMXCC;
            // skip previous xcc
            uint32_t skip = 0;
            for(int i = 0; i < xccIdx; i++)
            {
                // skip i
                skip += xccunit;
                if(i < xccres)
                {
                    // this xcc has extra 1 wg
                    skip += 1;
                }
            }
            realWGId += skip;

            // go inner xccid
            // in XCCN, we get the idx of the wg in XCCN.
            uint32_t innerXccId = resWGId / WGMXCC;
            realWGId += innerXccId;

            int32_t  sgprWGM            = wgm;
            uint32_t sgprNumWorkGroups0 = wg0;
            uint32_t sgprNumWorkGroups1 = wg1;
            uint32_t wg2     = realWGId / (sgprNumWorkGroups0 * sgprNumWorkGroups1 * gsu); //batch
            uint32_t idxWG01 = realWGId - (wg2 * sgprNumWorkGroups0 * sgprNumWorkGroups1 * gsu);
            uint32_t sgprWorkGroup1 = idxWG01 / wg0;
            uint32_t sgprWorkGroup0 = idxWG01 - (sgprWorkGroup1 * wg0);

            //go GSUWGMRR
            //std::cout<<"realWGId = "<<realWGId<<" , sgprWorkGroup0 = "<<sgprWorkGroup0<<" , sgprWorkGroup1 = "<<sgprWorkGroup1<<std::endl;
            uint32_t gsuSumIdx = 0;
            if(isGSUWGMRR)
            {
                gsuSumIdx      = sgprWorkGroup1 / sgprNumWorkGroups1;
                sgprWorkGroup1 = sgprWorkGroup1 % sgprNumWorkGroups1;
            }
            else
            {
                gsuSumIdx      = sgprWorkGroup1 % gsu;
                sgprWorkGroup1 = sgprWorkGroup1 / gsu;
            }
            //std::cout<<"gsuSumIdx = "<<gsuSumIdx<<" , sgprWorkGroup0 = "<<sgprWorkGroup0<<" , sgprWorkGroup1 = "<<sgprWorkGroup1<<std::endl;
            uint32_t finalwg1, finalwg0;
            if(wgm > 0)
            {
                uint32_t v6  = sgprWorkGroup1 / sgprWGM;
                uint32_t s84 = v6 * sgprWGM;
                s84          = sgprWorkGroup1 - s84;
                s84 *= sgprNumWorkGroups0;
                s84 += sgprWorkGroup0;
                uint32_t s81 = v6;

                v6           = sgprNumWorkGroups1 / sgprWGM;
                uint32_t s82 = v6;
                uint32_t s83 = sgprWGM * s82;
                s83          = sgprNumWorkGroups1 - s83;
                if(s83 == 0)
                    s83 = sgprWGM;
                if(s81 >= s82)
                    s82 = s83;
                else
                    s82 = sgprWGM;

                v6             = s84 / s82;
                uint32_t v7    = v6 * s82;
                v7             = s84 - v7;
                sgprWorkGroup0 = v6;
                sgprWorkGroup1 = v7;
                sgprWorkGroup1 = sgprWorkGroup0 * s82;
                sgprWorkGroup1 = s84 - sgprWorkGroup1;
                s81 *= sgprWGM;
                sgprWorkGroup1 += s81;

                finalwg1 = sgprWorkGroup1;
                finalwg0 = sgprWorkGroup0;
            }
            else
            {
                sgprWGM = 0 - sgprWGM;

                uint32_t v12 = sgprWorkGroup0 / sgprWGM;
                uint32_t s85 = v12;

                uint32_t s88 = s85 * sgprWGM;
                s88          = sgprWorkGroup0 - s88;
                s88 *= sgprNumWorkGroups1;
                s88 += sgprWorkGroup1;

                v12          = sgprNumWorkGroups0 / sgprWGM;
                uint32_t s86 = v12;
                uint32_t s87 = sgprWGM * s86;
                s87          = sgprNumWorkGroups0 - s87;
                if(s87 == 0)
                    s87 = sgprWGM;
                if(s85 >= s86)
                    s86 = s87;
                else
                    s86 = sgprWGM;

                v12          = s88 / s86;
                uint32_t v13 = v12 * s86;
                v13          = s88 - v13;

                sgprWorkGroup1 = v12;
                sgprWorkGroup0 = v13;
                sgprWorkGroup0 = sgprWorkGroup1 * s86;
                sgprWorkGroup0 = s88 - sgprWorkGroup0;
                s85 *= sgprWGM;
                sgprWorkGroup0 += s85;

                finalwg0 = sgprWorkGroup0;
                finalwg1 = sgprWorkGroup1;
            }
            //std::cout<<"xccIdx = "<<xccIdx<<" ,batch, gsuSumIdx, finalwg0, finalwg1 = "<<wg2<<","<<gsuSumIdx<<","<<finalwg0<<","<<finalwg1<<std::endl;
            uint32_t idxA = (wg2 * gsu + gsuSumIdx) * wg0 + finalwg0;
            if(isL2BypassA)
            {
                missA++;
                if(finalwg0 == wg0 - 1) //Edge
                    aMissElements += (MT0_Edge * depthU);
                else
                    aMissElements += (MT0 * depthU);
            }
            else if((arrA[idxA] & (1 << xccIdx)) || (arrA_2[idxA] & (1 << xccIdx)))
            {
                hitA++;
                if(finalwg0 == (wg0 - 1)) //Edge
                    aHitElements += (MT0_Edge * depthU);
                else
                    aHitElements += (MT0 * depthU);
                //std::cout<<"hitA "<<aHitElements<<std::endl;
                arrA[idxA] |= (1 << xccIdx);
            }
            else
            {
                missA++;
                if(finalwg0 == wg0 - 1) //Edge
                    aMissElements += (MT0_Edge * depthU);
                else
                    aMissElements += (MT0 * depthU);
                //std::cout<<"missA "<<aMissElements<<std::endl;
                arrA[idxA] |= (1 << xccIdx);
            }
            uint32_t idxB = (wg2 * gsu + gsuSumIdx) * wg1 + finalwg1;
            if(isL2BypassB)
            {
                missB++;
                if(finalwg1 == (wg1 - 1)) //Edge
                    bMissElements += (MT1_Edge * depthU);
                else
                    bMissElements += (MT1 * depthU);
            }
            else if((arrB[idxB] & (1 << xccIdx)) || (arrB_2[idxB] & (1 << xccIdx)))
            {
                hitB++;
                if(finalwg1 == (wg1 - 1)) //Edge
                    bHitElements += (MT1_Edge * depthU);
                else
                    bHitElements += (MT1 * depthU);
                //std::cout<<"hitB"<<std::endl;
                arrB[idxB] |= (1 << xccIdx);
            }
            else
            {
                missB++;
                if(finalwg1 == (wg1 - 1)) //Edge
                    bMissElements += (MT1_Edge * depthU);
                else
                    bMissElements += (MT1 * depthU);
                //std::cout<<"missB"<<std::endl;
                arrB[idxB] |= (1 << xccIdx);
            }
        }

        double hitRateA_old     = float(hitA) / float(hitA + missA);
        double hitRateB_old     = float(hitB) / float(hitB + missB);
        double totalHitRate_old = double(aRatio * hitRateA_old) + double(bRatio * hitRateB_old);

        if(aHitElements > 0)
            hitRateA = double(aHitElements) / double(aHitElements + aMissElements);
        if(bHitElements > 0)
            hitRateB = double(bHitElements) / double(bHitElements + bMissElements);
        if(aHitElements + bHitElements > 0)
            totalHitRate = double(aHitElements + bHitElements)
                           / double(aHitElements + aMissElements + bHitElements + bMissElements);

        //std::cout<<"Old HR is "<<hitRateA_old<<","<<hitRateB_old<<","<<totalHitRate_old<<std::endl;
        //std::cout<<"New HR is "<<hitRateA<<","<<hitRateB<<","<<totalHitRate<<std::endl;
        //std::cout<<"A Hit is "<<aHitElements<<", miss is"<<aMissElements<<std::endl;
        //std::cout<<"B Hit is "<<bHitElements<<", miss is"<<bMissElements<<std::endl;
        hitRate.totalHitRate = totalHitRate;
        hitRate.tile0HitRate = hitRateA;
        hitRate.tile1HitRate = hitRateB;

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
        int finalCycle = currentCycle;
        int grStallLatencyBuffer;

        if (!isStall) {
            grStallLatencyBuffer = 1; //no stall
        } else if (bpRead == 16) {
            grStallLatencyBuffer = 160;
        } else if (bpRead == 8) {
            grStallLatencyBuffer = 80;
        } else {
            grStallLatencyBuffer = 40;
        }

        if (fifo.size() < (16 / numWaves)) {
            fifo.push(currentCycle);
        } else {
            int oldCycle = fifo.front();
            if ((currentCycle - oldCycle) >= grStallLatencyBuffer) {
                fifo.pop();
                fifo.push(currentCycle);
            } else {
                finalCycle = oldCycle + grStallLatencyBuffer;
                fifo.pop();
                fifo.push(finalCycle);
            }
        }
        return finalCycle;
    }

    int Formocast::checkLocalReadFinished(int currentCycle, std::queue<int>& fifo, int numLR) const
    {
        if(fifo.size() <= numLR)
            return currentCycle;
        int finalCycle = currentCycle;
        //pop finisned LR
        while(fifo.size() > numLR)
        {
            int oldCycle = fifo.front();
            if (oldCycle < currentCycle)
                fifo.pop();
            else
                break;
        }
        //check non-finished LR
        while(fifo.size() > numLR)
        {
            int oldCycle = fifo.front();
            finalCycle = std::max(finalCycle, oldCycle);
        }
        return finalCycle;
    }

    int Formocast::checkLocalReadFIFOFull(int currentCycle, std::queue<int>& fifo, int bpRead, int numWaves, bool isStall) const
    {
        int finalCycle = currentCycle;
        int lrStallLatencyBuffer;
        if (!isStall){
            lrStallLatencyBuffer = 1;
        }
        else if (bpRead == 16) {
            lrStallLatencyBuffer = 10;
        } else if (bpRead == 8) {
            lrStallLatencyBuffer = 5;
        } else {
            lrStallLatencyBuffer = 2;
        }

        if (fifo.size() < (16 / numWaves)) {
            fifo.push(currentCycle);
        } else {
            int oldCycle = fifo.front();
            if ((currentCycle - oldCycle) >= lrStallLatencyBuffer) {
                fifo.pop();
                fifo.push(currentCycle);
            } else {
                finalCycle = oldCycle + lrStallLatencyBuffer;
                fifo.pop();
                fifo.push(finalCycle);
            }
        }
        return finalCycle;
    }

    void Formocast::pushLocalRead(int currentCycle, std::queue<int>& fifo, int bpr, bool isGfx950) {

        std::vector<int> latency(5);
        if (isGfx950)
        {
            latency = {11,11,11,11,21};
        }
        else
        {
            latency = {12,12,12,21,27};
        }
        int lrMemLatency;
        if (bpr == 16) {
            lrMemLatency = latency[4];
        } else if (bpr == 8) {
            lrMemLatency = latency[3];
        } else {
            lrMemLatency = latency[2];
        }
        fifo.push(currentCycle + lrMemLatency);
    }
} // namespace Tensilelite
