// Copyright Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT

#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <queue>

#include <origami/utils.hpp>

namespace Tensilelite
{
    class Formocast
    {
    public:
        struct SizeMapping
        {
            size_t waveNum;

            std::array<int, 3> macroTile;
            std::array<int, 4> matrixInstruction;
            size_t             grvwA = 1;
            size_t             grvwB = 1;
            size_t             gwvwC = 1;
            size_t             gwvwD = 1;

            size_t  depthU             = 0;
            int16_t globalSplitU       = 0;

            int     workGroupMapping   = 0;
            int     globalAccumulation = 0;

            int  workGroupMappingXCC                    = 0;
            int  workGroupMappingXCCGroup               = 0;
            bool globalSplitUCoalesced                  = false;
            bool globalSplitUWorkGroupMappingRoundRobin = false;

            int CUOccupancy            = 0;
            int PrefetchGlobalRead     = 2;
            int MathClocksUnrolledLoop = 0;

            bool DirectToVgprA = false;
            bool DirectToVgprB = false;
            int NumLoadsCoalescedA = 0;
            int NumLoadsCoalescedB = 0;
            int VectorWidthA = 1;
            int VectorWidthB = 1;
            int LocalSplitU = 1;

            std::array<int, 2> waveGroup;
        };
        struct PredictedPerformance
        {
            double   microSeconds = 0.0;
            double   hitRate      = 0.0;
        };

        struct L2CacheHitRate
        {
            double tile0HitRate          = 0.0;
            double tile1HitRate          = 0.0;
            double totalHitRate          = 0.0;
        };

        struct L1CacheHitRate : public L2CacheHitRate{};
        struct L3CacheHitRate : public L2CacheHitRate{};

        struct HardwareConstants
        {
            double L1CacheCapacity;
            double L2CacheCapacity;
            double L3CacheCapacity;
            double L1CacheLineSize;
            double L2CacheLineSize;
            double L1BusWidthPerCU;
            double L2BusWidthPerCU;
            double L1WriteBusWidthPerCU;
            double L2WriteBusWidthPerCU;
            double maxBandWidthHBM;
            double mem_frequency;
            double hbmBandWidth;
            double L3BandWidth;
            double math_frequency;
            double boost_frequency;
            double initialCost;
            double initialCostHit;
            double flopsPerClk;
            double NumCUs;
            double wavefrontSize;
            double L2ReadArbEff;
            double L2WriteArbEff;
            uint32_t NumXCDs;

            void print() const {
                std::cout << "HardwareConstants:" << std::endl;
                std::cout << "  L1CacheCapacity:      " << L1CacheCapacity << std::endl;
                std::cout << "  L2CacheCapacity:      " << L2CacheCapacity << std::endl;
                std::cout << "  L3CacheCapacity:      " << L3CacheCapacity << std::endl;
                std::cout << "  L1CacheLineSize:      " << L1CacheLineSize << std::endl;
                std::cout << "  L2CacheLineSize:      " << L2CacheLineSize << std::endl;
                std::cout << "  L1BusWidthPerCU:      " << L1BusWidthPerCU << std::endl;
                std::cout << "  L2BusWidthPerCU:      " << L2BusWidthPerCU << std::endl;
                std::cout << "  L1WriteBusWidthPerCU: " << L1WriteBusWidthPerCU << std::endl;
                std::cout << "  L2WriteBusWidthPerCU: " << L2WriteBusWidthPerCU << std::endl;
                std::cout << "  maxBandWidthHBM:      " << maxBandWidthHBM << std::endl;
                std::cout << "  mem_frequency:        " << mem_frequency << std::endl;
                std::cout << "  hbmBandWidth:         " << hbmBandWidth << std::endl;
                std::cout << "  L3BandWidth:          " << L3BandWidth << std::endl;
                std::cout << "  math_frequency:       " << math_frequency << std::endl;
                std::cout << "  boost_frequency:      " << boost_frequency << std::endl;
                std::cout << "  initialCost:          " << initialCost << std::endl;
                std::cout << "  initialCostHit:       " << initialCostHit << std::endl;
                std::cout << "  flopsPerClk:          " << flopsPerClk << std::endl;
                std::cout << "  NumCUs:               " << NumCUs << std::endl;
                std::cout << "  wavefrontSize:        " << wavefrontSize << std::endl;
                std::cout << "  L2ReadArbEff:         " << L2ReadArbEff << std::endl;
                std::cout << "  L2WriteArbEff:        " << L2WriteArbEff << std::endl;
                std::cout << "  NumXCDs:              " << NumXCDs << std::endl;
            };
        };

        struct CacheHitRates
        {
            double A_L1_hit;
            double B_L1_hit;
            double A_L2_hit;
            double B_L2_hit;
            double A_L3_hit;
            double B_L3_hit;
            double totalL2HitRate;
            double totalL3HitRate; // Added for clarity
        };

        struct MemoryAccessCosts
        {
            double mem_l1;
            double mem_l2;
            double mem_l3;
            double mem_hbm;
            double l1_hit;
            double l2_hit;
            double l3_hit;
            double mem_overall;
            //for debug
            double A_L1_req;
            double B_L1_req;
            double A_L2_req;
            double B_L2_req;
        };

        struct TieBreakerInfo
        {
            MemoryAccessCosts memory;
            double perf;
            double preloop;
            double loop;
            double tail;
            double store;
            double gsu;
            double lsu;
            double math;
            double mt0;
            double mt1;
            uint32_t du;
            int    svw;
        };

        struct ProblemInfo
        {
            double M;
            double N;
            double NumBatches;
            double K;
            uint32_t bpeA;
            uint32_t bpeB;
            uint32_t bpeD;
            uint32_t bpeCompute;
            bool transA;
            bool transB;
            bool swizzleTensorA;
            bool swizzleTensorB;
            origami::data_type_t dataType;
        };
        
        void setProblem(ProblemInfo p);
        void setSolution(SizeMapping sm);
        void setHardware(std::shared_ptr<origami::hardware_t> hw);
        HardwareConstants getHardwareConstants(void) const;
        void calculateStorePerformance(double M,
                                       double N,
                                       double NumBatches,
                                       double MT0,
                                       double MT1,
                                       uint32_t GWVWD,
                                       uint32_t bpeD,
                                       const HardwareConstants& hw_consts,
                                       uint32_t WGs_per_tile,
                                       uint32_t WGs_per_tile_XCD,
                                       double &store,
                                       double &store_edge) const;
        double calculateGSUOverhead(double M, double N, double K, 
                                    double NumBatches, double GlobalSplitU,
                                    uint32_t gsuMethod, ProblemInfo problem,
                                    const HardwareConstants& hw_consts,
                                    uint32_t WGs_per_tile, uint32_t WGs_per_tile_XCD,
                                    double MT0, double MT1, uint32_t numWGs, double vgprCheck,
                                    double storeGSU) const;
        double calculateLSUOverhead(double MT0, double MT1, double lsu,
                                    uint32_t svw, uint32_t numThreads,
                                    ProblemInfo problem,
                                    const HardwareConstants& hw_consts) const;
        MemoryAccessCosts
        calculateMemoryAccessCosts(double MT0, double MT1, uint32_t depthU,
                                   uint32_t bpeA, uint32_t bpeB,
                                   const HardwareConstants& hw,
                                   uint32_t GRVWA, uint32_t GRVWB,
                                   bool DTVA, bool DTVB,
                                   const CacheHitRates& hr,
                                   double L2BandWidthPerCU, double L3BandWidthPerCU, double HBMBandWidthPerCU,
                                   double TCP_EFF_ratio,
                                   uint32_t VWA, uint32_t VWB,
                                   bool isSwizzleA, bool isSwizzleB,
                                   bool trA, bool trB,
                                   uint32_t numWave0, uint32_t numWave1,
                                   int NLCA, int NLCB) const;
        double getLoopOverall(const MemoryAccessCosts& mem, double math, uint32_t loopCnt, double pgr) const;
        PredictedPerformance predictedPerformance() const;      
        L1CacheHitRate 
        computeL1CacheHitRate(const HardwareConstants& hw,
                            double MT0, double MT1, uint32_t bpeA, uint32_t bpeB,
                            int NTA, int NTB, uint32_t GRVWA, uint32_t GRVWB,
                            bool DTVA, bool DTVB, bool isSwizzleA, bool isSwizzleB,
                            uint32_t VWA, uint32_t VWB, bool transA, bool transB,
                            double lda, double ldb, int NLCA, int NLCB,
                            uint32_t threadnum, uint32_t NumWave0, uint32_t NumWave1) const;
        L2CacheHitRate computeL2CacheHitRate(uint32_t M,
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
                                             bool     isGSUWGMRR) const;
        L3CacheHitRate 
        computeL3CacheHitRate(double M, double N, double K, const HardwareConstants& hw,
                                          uint32_t bpeA, uint32_t bpeB, int NTA, int NTB,
                                          int N_WGs_total, int M_WGs_total, int N_WGs_per_tile, int M_WGs_per_tile) const;
        double resolveOccupancy(const HardwareConstants& hw, double perf, double prefetch, double mathCost, double storeCost, uint32_t num_tiles, uint32_t CUOccupancy) const;
        bool isBetter(ProblemInfo problem, TieBreakerInfo previousSolution) const;
        bool isBetter(TieBreakerInfo previousSolution, TieBreakerInfo currentSolution) const;
        TieBreakerInfo getTieBreakerInfo() const;
        int checkLocalReadFIFOFull(int currentCycle, std::queue<int>& fifo, int bpRead, int numWaves, bool isStall) const;
        int checkLocalReadFinished(int currentCycle, std::queue<int>& fifo, int numLR) const;
        int checkGlobalReadFIFOFull(int currentCycle, std::queue<int>& fifo, int bpRead, int numWaves, bool isStall) const;
        void pushLocalRead(int currentCycle, std::queue<int>& fifo, int bpr, bool isGfx950);
    public:
        SizeMapping sizeMapping;
        ProblemInfo problem;
        std::shared_ptr<origami::hardware_t> hardware;
        mutable TieBreakerInfo perfInfo;
    };
} // namespace Tensilelite
