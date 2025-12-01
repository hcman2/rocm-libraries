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
#include <tuple>
#include <regex>
#include <algorithm>

#include "formocast.hpp"

using MacroArguments = std::vector<std::tuple<std::string, std::string>>;
using MacroEntity = std::tuple<std::shared_ptr<rocisa::Macro>, std::shared_ptr<MacroArguments>>;
using MacroTable = std::unordered_map<std::string, MacroEntity>;

namespace rocisa
{
    // Helper function to handle macro and macro if/else, should be remove once macro is forbidden
    void _extractMacro(MacroTable& macros, std::shared_ptr<Macro> macro)
    {
        auto args = std::make_shared<MacroArguments>();
        auto entity = std::make_tuple(macro, args);
        macros[macro->macro->name] = entity;
        // parse argument and default values
        std::regex argPattern("([^=]+)=?(\\w+)?");
        std::smatch match;
        for(auto& arg : macro->macro->args)
        {
            std::regex_match(std::get<std::string>(arg), match, argPattern);
            args->push_back(std::make_tuple(match[1].str(), match.size() > 2 ? match[2].str() : std::string()));
        }
    }

    void _getMacros(std::shared_ptr<Module> module, MacroTable& macros)
    {
        for(auto& item : module->items())
        {
            if(auto subModule = std::dynamic_pointer_cast<Module>(item))
            {
                _getMacros(subModule, macros);
            }
            else if(auto macro = std::dynamic_pointer_cast<Macro>(item))
            {
                if(macros.find(macro->macro->name) == macros.end())
                {
                    _extractMacro(macros, macro);
                }
            }
        }
    }

    bool _evalMacroCondition(std::string value, std::shared_ptr<MacroArguments> args)
    {
        std::regex tokenPattern("\\\\([^=\\s]+)|\\w+|==|!=|&&");
        std::sregex_iterator it(value.begin(), value.end(), tokenPattern);
        std::sregex_iterator end;
        std::string lhs, rhs, op, val;
        int i = 0;
        bool result;
        std::vector<int> results;
        while(it != end)
        {
            std::smatch match = *it;
            if(!match.str(0).empty())
            {
                val = match.str(0);
                if(val[0] == '\\')
                {
                    auto var = match.str(1);
                    auto find_it = std::find_if(args->begin(), args->end(), [&var](auto& arg){ return std::get<0>(arg) == var; });
                    if(find_it != args->end())
                    {
                        val = std::get<1>(*find_it);
                    }
                    else
                    {
                        throw std::runtime_error("unknown macro argument");
                    }
                }
                if(i == 0)
                {
                    lhs = val;
                }
                else if(i == 1)
                {
                    op = val;
                }
                else if(i == 2)
                {
                    rhs = val;
                    if(op == "==")
                    {
                        result = lhs == rhs;
                    }
                    else if(op == "!=")
                    {
                        result = lhs != rhs;
                    }
                    else
                    {
                        throw std::runtime_error("unknown macro condition");
                    }
                    results.push_back(result ? 1 : 0);
                }
                else if(i == 4)
                {
                    if(val == "&&")
                    {
                        results.push_back(2);
                    }
                    else
                    {
                        throw std::runtime_error("unknown macro condition");
                    }
                }
                i = (i + 1) % 4;
            }
            ++it;
        }
        result = results.front();
        i = 1;
        while(i < results.size())
        {
            auto r = results[i];
            if(r == 2)
            {
                result = result & results[i+1];
                i += 2;
            }
            else
            {
                i++;
            }
        }
        return result;
    }

    void _expandMacroAndPopInst(std::vector<std::shared_ptr<Item>>& moduleInst, std::vector<std::shared_ptr<Item>>& macroItems, std::shared_ptr<MacroArguments> args, std::vector<bool>& branch)
    {
        for(auto& item : macroItems)
        {
            if(auto subModule = std::dynamic_pointer_cast<Module>(item))
            {
                _expandMacroAndPopInst(moduleInst, subModule->itemList, args, branch);
            }
            else if(auto valueIf = std::dynamic_pointer_cast<ValueIf>(item))
            {
                branch.push_back(branch.back() && _evalMacroCondition(valueIf->value, args));
            }
            else if(auto valueElseIf = std::dynamic_pointer_cast<ValueElseIf>(item))
            {
                bool ifTaken = branch.back();
                branch.pop_back();
                branch.push_back(!ifTaken && _evalMacroCondition(valueElseIf->value, args));
            }
            else if(auto valueEndif = std::dynamic_pointer_cast<ValueEndif>(item))
            {
              branch.pop_back();
            }
            else if(auto instruction = std::dynamic_pointer_cast<Instruction>(item))
            {
                if(branch.back())
                {
                    moduleInst.push_back(instruction);
                }
            }
        }
    }

    void _expandMacroAndPopInst(std::vector<std::shared_ptr<Item>>& moduleInst, std::vector<std::shared_ptr<Item>>& macroItems, std::shared_ptr<MacroArguments> args)
    {
        std::vector<bool> branch = {true};
        _expandMacroAndPopInst(moduleInst, macroItems, args, branch);
    }

    void _popInst(std::shared_ptr<Module> mod, std::vector<std::shared_ptr<Item>>& moduleInst, MacroTable& macros)
    {
        for(auto& item : mod->items())
        {
            if(auto subModule = std::dynamic_pointer_cast<Module>(item))
            {
                _popInst(subModule, moduleInst, macros);
            }
            else if(auto macro = std::dynamic_pointer_cast<Macro>(item))
            {
                _extractMacro(macros, macro);
            }
            else if(auto macroInst = std::dynamic_pointer_cast<MacroInstruction>(item))
            {
                auto entity = macros[macroInst->name];
                auto macro = std::get<0>(entity);
                auto defaultArgs = std::get<1>(entity);
                auto args = std::make_shared<MacroArguments>();
                for(auto& arg : *defaultArgs)
                {
                    args->push_back(arg);
                }
                for(int i = 0; i < macroInst->args.size(); i++)
                {
                    std::get<1>(args->at(i)) = InstructionInputToString(macroInst->args[i]);
                }
                _expandMacroAndPopInst(moduleInst, macro->itemList, args);
            }
            else if(auto label = std::dynamic_pointer_cast<Label>(item))
            {
                moduleInst.push_back(label);
            }
            else if(auto instruction = std::dynamic_pointer_cast<Instruction>(item))
            {
                moduleInst.push_back(instruction);
            }
        }
    }

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
    int _countCycles(std::shared_ptr<Module> item, int numWaves, MacroTable& macros)
    {
        Tensilelite::Formocast formocast;
        std::vector<std::shared_ptr<Item>> moduleInst;
        _popInst(item, moduleInst, macros);

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
        bool skip = false;
        for(auto& item : moduleInst)
        {
            if(auto subModule = std::dynamic_pointer_cast<Module>(item))
            {
                throw std::runtime_error("Module should be instructions here.");
            }
            else if(auto subModule = std::dynamic_pointer_cast<MacroInstruction>(item))
            {
                throw std::runtime_error("MacroInst should be instructions here.");
            }
            else if(auto subModule = std::dynamic_pointer_cast<Macro>(item))
            {
                throw std::runtime_error("Macro should be instructions here.");
            }
            else if(auto label = std::dynamic_pointer_cast<Label>(item))
            {
                auto labelStr = std::visit(
                    [](auto&& arg) -> std::string {
                        using T = std::decay_t<decltype(arg)>;
                        if constexpr(std::is_same_v<T, int>)
                        {
                            return std::to_string(arg);
                        }
                        else if constexpr(std::is_same_v<T, std::string>)
                        {
                            return arg;
                        }
                    },
                    label->label);
                std::regex simdBranchesPattern(".*Loop(Skip)?BeginL(_\\d+)?.*");
                std::smatch match;
                if(std::regex_match(labelStr, match, simdBranchesPattern))
                {
                    skip = match[1].matched || (match[2].matched && match.str(2) != "_0");
                }
                else
                {
                    skip = false;
                }
            }
            else if(skip)
            {
                continue;
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
                auto pos = branchInst->labelName.find("label_LoopBeginL");
                if(pos != std::string::npos && pos == 0) //branchInst->labelName == "label_LoopBeginL")
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
                instruction->comment = instruction->comment + " <This is " + std::to_string(cycles) + "-cycle>"; // for debug
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
    int _countCycles(std::shared_ptr<Module> item, int numWaves)
    {
        MacroTable macros;
        return _countCycles(item, numWaves, macros);
    }

    // Function to calculate math clocks in an unrolled loop
    int _calculateMathClocksInUnrolledLoop(std::shared_ptr<Module> module, int numWaves)
    {
        // Kernel: openLoop->loopBody->noLoadLoop
        int  cycles     = -1;
        bool isOpenLoop = false;
        MacroTable macros;
        _getMacros(module, macros);

        for(auto& item : module->items())
        {
            // Find loopBody
            if(auto subModule = std::dynamic_pointer_cast<Module>(item))
            {
                if(subModule->name == "loopBody")
                {
                    cycles = _countCycles(subModule, numWaves, macros);
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
