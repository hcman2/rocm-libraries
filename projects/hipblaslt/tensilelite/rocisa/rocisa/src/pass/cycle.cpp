/* ************************************************************************
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
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
#include "instruction/branch.hpp"
#include "instruction/instruction.hpp"
#include "instruction/mem.hpp"
#include "instruction/mfma.hpp"
#include "instruction/common.hpp"
#include "pass.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <queue>

#include "formocast.hpp"

namespace rocisa
{
    // Helper function to populate instructions from a module
    void _popInst(std::shared_ptr<Module> mod, std::vector<std::shared_ptr<Item>>& moduleInst)
    {
        for(auto& item : mod->items())
        {
            if(auto subModule = std::dynamic_pointer_cast<Module>(item))
            {
                _popInst(subModule, moduleInst);
            }
            else if(auto instruction = std::dynamic_pointer_cast<Instruction>(item))
            {
                moduleInst.push_back(instruction);
            }
        }
    }

    // Helper function to count cycles
    int _countCycles(std::shared_ptr<Module> item, int numWaves)
    {
        Tensilelite::Formocast formocast;
        std::vector<std::shared_ptr<Item>> moduleInst;
        _popInst(item, moduleInst);

        int cycles = 0;
        int hwMFMA = -99;
        int jumpOverhead = 6;
        int previousLW = 0;
        std::queue<int> hwLRFIFO;
        std::queue<int> mfmaLRFIFO;
        std::queue<int> hwGRFIFO;
        bool isEndOfLoop  = false;
        bool isPreviousLR = false;
        auto isaVersion   = rocIsa::getInstance().getKernel().isaVersion;
        for(auto& item : moduleInst)
        {
            if(auto subModule = std::dynamic_pointer_cast<Module>(item))
            {
                throw std::runtime_error("Module should be instructions here.");
            }
            else if(auto mfmaInst = std::dynamic_pointer_cast<MFMAInstruction>(item))
            {
                auto mfmaLatency = mfmaInst->getIssueLatency();
                //FIXME: hack here for gfx950 bug in mfmaInst->getIssueLatency();
                mfmaLatency /= 2;
                if(cycles - hwMFMA >= (mfmaLatency - 1))
                {
                    cycles += 1;
                }
                else
                {
                    cycles = hwMFMA + mfmaLatency;
                }
                hwMFMA = cycles;
            }
            else if(auto dsReadInst = std::dynamic_pointer_cast<DSLoadInstruction>(item))
            {
                //heck LR fifo
                auto currCycles = cycles + dsReadInst->issueLatency();
                if(isPreviousLR){
                    currCycles += dsReadInst->issueLatency();
                    isPreviousLR = false;
                }
                else{
                    isPreviousLR = true;
                }
                int bpr = 4;
                if (auto lr128 = std::dynamic_pointer_cast<DSLoadB128>(dsReadInst)) {
                    bpr = 16;
                } else if (auto lr64 = std::dynamic_pointer_cast<DSLoadB64>(dsReadInst)) {
                    bpr = 8;
                }
                cycles = formocast.checkLocalReadFIFOFull(currCycles, hwLRFIFO, bpr, numWaves, isaVersion != std::array<int, 3>{9, 5, 0});

                formocast.pushLocalRead(currCycles, mfmaLRFIFO, bpr, isaVersion == std::array<int, 3>{9, 5, 0});
            }
            else if(auto rwInst = std::dynamic_pointer_cast<ReadWriteInstruction>(item))
            {
                if(auto grInst = std::dynamic_pointer_cast<MUBUFReadInstruction>(item))
                {
                    auto currCycles = cycles + grInst->issueLatency();
                    int bpr = 4;
                     if (auto gr128 = std::dynamic_pointer_cast<BufferLoadB128>(grInst)) {
                        bpr = 16;
                    } else if (auto gr64 = std::dynamic_pointer_cast<BufferLoadB64>(grInst)) {
                        bpr = 8;
                    }
                    cycles = formocast.checkGlobalReadFIFOFull(currCycles, hwGRFIFO, bpr, numWaves, false);
                }
                if(auto wInst = std::dynamic_pointer_cast<DSStoreB128>(item))
                {
                    if(previousLW + wInst->issueLatency() >= cycles && numWaves == 4)
                        cycles += wInst->issueLatency() * 2;
                    else
                        cycles += wInst->issueLatency();
                    previousLW = cycles;
                }
                else if(auto wInst = std::dynamic_pointer_cast<DSStoreB64>(item))
                {
                    if(previousLW + wInst->issueLatency() >= cycles && numWaves == 4)
                        cycles += wInst->issueLatency() * 2;
                    else
                        cycles += wInst->issueLatency();
                    previousLW = cycles;
                }
                else if(auto wInst = std::dynamic_pointer_cast<DSStoreB32>(item))
                {
                    if(previousLW + wInst->issueLatency() >= cycles && numWaves == 4)
                        cycles += wInst->issueLatency() * 2;
                    else
                        cycles += wInst->issueLatency();
                    previousLW = cycles;
                }
                else
                {
                    cycles += rwInst->issueLatency();
                }
            }
            else if(auto waitInst = std::dynamic_pointer_cast<_SWaitCnt>(item))
            {
                auto numLR = waitInst->getParams();
                cycles = formocast.checkLocalReadFinished(cycles + 1, mfmaLRFIFO, std::stoi(InstructionInputToString(numLR[0])));
            }
            else if(auto branchInst = std::dynamic_pointer_cast<BranchInstruction>(item))
            {
                cycles = std::max(cycles + jumpOverhead, hwMFMA + 4);
                // End of loop
                if(branchInst->labelName.find("label_LoopBeginL") != std::string::npos ) //branchInst->labelName == "label_LoopBeginL")
                {
                    isEndOfLoop = true;
                    break;
                }
            }
            else if(auto instruction = std::dynamic_pointer_cast<Instruction>(item))
            {
                cycles += 1;
            }
            if(auto instruction = std::dynamic_pointer_cast<Instruction>(item))
            {
                instruction->comment = "This is " + std::to_string(cycles) + "-cycle"; // for debug
            }
        }
        if(!isEndOfLoop)
        {
            // Loop end without label_LoopBeginL label.
            // Add jump overhead here
            cycles += jumpOverhead + 1;
        }
        return cycles * 4; // 4 for gfx9
    }

    // Function to calculate math clocks in an unrolled loop
    int _calculateMathClocksInUnrolledLoop(std::shared_ptr<Module> module, int numWaves)
    {
        // Kernel: openLoop->loopBody->noLoadLoop
        int  cycles     = -1;
        bool isOpenLoop = false;

        for(auto& item : module->items())
        {
            // Find loopBody
            if(auto subModule = std::dynamic_pointer_cast<Module>(item))
            {
                if(subModule->name == "loopBody")
                {
                    cycles = _countCycles(subModule, numWaves);
                    return cycles;
                }
            }
        }
        return -1;
    }

    // Main function to get cycles
    int getCycles(std::shared_ptr<Module> module, int numWaves)
    {
        return _calculateMathClocksInUnrolledLoop(module, numWaves);
    }
} // namespace rocisa
