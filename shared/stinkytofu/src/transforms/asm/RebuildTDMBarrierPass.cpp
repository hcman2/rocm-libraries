// Copyright Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT

#include "stinkytofu/transforms/asm/RebuildTDMBarrierPass.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "stinkytofu/core/PassManager.hpp"
#include "stinkytofu/hardware/ArchHelper.hpp"
#include "stinkytofu/ir/asm/StinkyAsmIR.hpp"
#include "stinkytofu/ir/asm/StinkyModifiers.hpp"
#include "stinkytofu/support/Casting.hpp"
#include "stinkytofu/support/ErrorHandling.hpp"
#include "stinkytofu/support/LoopDetection.hpp"

#define DEBUG_TYPE "RebuildTDMBarrierPass"

namespace {
using namespace stinkytofu;

enum Phase : uint8_t {
    Standby = 0,
    Reading = 1,
    Writing = 2,
};

using TokenState = std::map<int, uint8_t>;
using BlockState = std::map<BasicBlock*, TokenState>;
using CFGEdge = std::pair<const BasicBlock*, const BasicBlock*>;
using CFGEdgeSet = std::set<CFGEdge>;

const std::vector<int>* tokensOf(const StinkyInstruction& inst) {
    const auto* modifier = inst.getModifier<MemTokenData>();
    return modifier == nullptr ? nullptr : &modifier->tokens;
}

std::vector<int> normalizedTokens(const StinkyInstruction& inst) {
    const auto* tokens = tokensOf(inst);
    if (tokens == nullptr) return {};
    std::vector<int> result = *tokens;
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

uint8_t classifyAccess(const StinkyInstruction& inst) {
    if (isTensorLoad(inst) || isDSWrite(inst)) return Writing;
    if (isDSRead(inst)) return Reading;
    return Standby;
}

bool isTokenMemoryCandidate(const StinkyInstruction& inst) {
    return isTensorLoad(inst) || isDSWrite(inst) || isDSRead(inst) || isDSAtomic(inst) ||
           isGlobalStoreAsyncFromLds(inst);
}

bool sameManagedTokens(const StinkyInstruction& signal, const StinkyInstruction& wait) {
    const std::vector<int> signalTokens = normalizedTokens(signal);
    return !signalTokens.empty() && signalTokens == normalizedTokens(wait);
}

size_t removeManagedBarrierPairs(Function& func) {
    size_t removedPairs = 0;
    for (BasicBlock& bb : func) {
        for (auto it = bb.begin(); it != bb.end();) {
            auto* signal = dyn_cast<StinkyInstruction>(it.getNodePtr());
            if (signal == nullptr || !isBarrierSignal(*signal) || !isSplitBarrierAllWave(*signal)) {
                ++it;
                continue;
            }

            auto waitIt = std::next(it);
            if (waitIt == bb.end()) {
                ++it;
                continue;
            }
            auto* wait = dyn_cast<StinkyInstruction>(waitIt.getNodePtr());
            if (wait == nullptr || !isBarrierWait(*wait) || !isSplitBarrierAllWave(*wait) ||
                !sameManagedTokens(*signal, *wait)) {
                ++it;
                continue;
            }

            it = bb.eraseIR(it);
            it = bb.eraseIR(it);
            ++removedPairs;
        }
    }
    return removedPairs;
}

void resetBarrierTokens(const StinkyInstruction& inst, TokenState& state) {
    if (!isBarrier(inst)) return;
    const auto* tokens = tokensOf(inst);
    if (tokens == nullptr) return;
    for (int token : *tokens) state.erase(token);
}

void transferBlock(const BasicBlock& bb, TokenState& state) {
    for (const IRBase& ir : bb) {
        const auto* inst = dyn_cast<StinkyInstruction>(&ir);
        if (inst == nullptr) continue;

        resetBarrierTokens(*inst, state);
        const auto* tokens = tokensOf(*inst);
        if (tokens == nullptr || tokens->empty()) continue;

        const uint8_t phase = classifyAccess(*inst);
        if (phase == Standby) {
            if (isTokenMemoryCandidate(*inst)) {
                // Eligible TDM loop regions must not contain token-bearing DS
                // atomics or global_store_async_from_lds instructions. Their
                // read/write semantics are outside this pass's phase model, so
                // fail fast if that pipeline invariant is violated.
                report_fatal_error("RebuildTDMBarrierPass: token-bearing memory instruction '" +
                                   std::string(inst->getHwInstDesc()->mnemonic) +
                                   "' has no read/write phase classification");
            }
            continue;
        }
        for (int token : *tokens) state[token] = phase;
    }
}

CFGEdgeSet findLoopBackEdges(Function& func) {
    CFGEdgeSet result;
    for (const Loop& loop : detectLoops(func)) result.emplace(loop.latchBB, loop.headerBB);
    return result;
}

TokenState mergePredecessors(const BasicBlock& bb, const BlockState& outStates,
                             const CFGEdgeSet& ignoredEdges) {
    TokenState merged;
    for (const BasicBlock* pred : bb.getPredecessors()) {
        if (ignoredEdges.count({pred, &bb}) != 0) continue;
        auto found = outStates.find(const_cast<BasicBlock*>(pred));
        if (found == outStates.end()) continue;
        for (const auto& [token, phases] : found->second) merged[token] |= phases;
    }
    return merged;
}

void solveDataflow(Function& func, BlockState& inStates, BlockState& outStates,
                   const CFGEdgeSet& ignoredEdges) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (BasicBlock& bb : func) {
            TokenState input = mergePredecessors(bb, outStates, ignoredEdges);
            TokenState output = input;
            transferBlock(bb, output);
            if (inStates[&bb] != input) {
                inStates[&bb] = std::move(input);
                changed = true;
            }
            if (outStates[&bb] != output) {
                outStates[&bb] = std::move(output);
                changed = true;
            }
        }
    }
}

std::string barrierComment(const std::vector<int>& tokens) {
    std::string comment = "auto token transition barrier";
    for (int token : tokens) comment += ", sync LDS" + std::to_string(token);
    return comment;
}

size_t insertTransitionBarriers(Function& func, const BlockState& inStates, GfxArchID arch) {
    const HwInstDesc* signalDesc = getMCIDByUOp(GFX::s_barrier_signal, arch);
    const HwInstDesc* waitDesc = getMCIDByUOp(GFX::s_barrier_wait, arch);
    if (signalDesc == nullptr || waitDesc == nullptr)
        report_fatal_error("RebuildTDMBarrierPass: gfx125 split barriers are unavailable");

    size_t insertedPairs = 0;
    for (BasicBlock& bb : func) {
        TokenState state;
        if (auto found = inStates.find(&bb); found != inStates.end()) state = found->second;

        for (auto it = bb.begin(); it != bb.end(); ++it) {
            auto* inst = dyn_cast<StinkyInstruction>(it.getNodePtr());
            if (inst == nullptr) continue;

            resetBarrierTokens(*inst, state);
            const auto* rawTokens = tokensOf(*inst);
            if (rawTokens == nullptr || rawTokens->empty()) continue;

            const uint8_t phase = classifyAccess(*inst);
            if (phase == Standby) {
                if (isTokenMemoryCandidate(*inst)) {
                    report_fatal_error("RebuildTDMBarrierPass: token-bearing memory instruction '" +
                                       std::string(inst->getHwInstDesc()->mnemonic) +
                                       "' has no read/write phase classification");
                }
                continue;
            }

            std::vector<int> conflicts;
            for (int token : *rawTokens) {
                const uint8_t prior = state[token];
                if ((phase == Reading && (prior & Writing) != 0) ||
                    (phase == Writing && (prior & Reading) != 0))
                    conflicts.push_back(token);
            }
            std::sort(conflicts.begin(), conflicts.end());
            conflicts.erase(std::unique(conflicts.begin(), conflicts.end()), conflicts.end());

            if (!conflicts.empty()) {
                AsmIRBuilder builder(bb, arch);
                StinkyInstruction* signal = builder.create(signalDesc, inst);
                signal->addSrcReg(StinkyRegister(-1));
                signal->addModifier<MemTokenData>(MemTokenData{conflicts});

                StinkyInstruction* wait = builder.create(waitDesc, inst);
                wait->addSrcReg(StinkyRegister(-1));
                wait->addModifier<MemTokenData>(MemTokenData{conflicts});
                wait->addModifier<CommentData>(CommentData{barrierComment(conflicts)});
                ++insertedPairs;
            }

            for (int token : *rawTokens) state[token] = phase;
        }
    }
    return insertedPairs;
}

class RebuildTDMBarrierPass final : public Pass {
   public:
    static char ID;

    explicit RebuildTDMBarrierPass(bool modelLoopBackEdges)
        : modelLoopBackEdges_(modelLoopBackEdges) {}

    const char* getName() const override {
        return "RebuildTDMBarrierPass";
    }

    PassID getPassID() const override {
        return &ID;
    }

    PreservedAnalyses run(Function& func, PassContext& passCtx, AnalysisManager& /*AM*/) override {
        const auto& archTriple = passCtx.getGemmTileConfig().arch;
        const GfxArchID arch = getGfxArchID(archTriple[0], archTriple[1], archTriple[2]);

        const size_t removedPairs = removeManagedBarrierPairs(func);
        BlockState inStates;
        BlockState outStates;
        const CFGEdgeSet ignoredEdges =
            modelLoopBackEdges_ ? CFGEdgeSet{} : findLoopBackEdges(func);
        solveDataflow(func, inStates, outStates, ignoredEdges);
        const size_t insertedPairs = insertTransitionBarriers(func, inStates, arch);

        PASS_DEBUG(std::cerr << "[RebuildTDMBarrierPass] removed_pairs=" << removedPairs
                             << " inserted_pairs=" << insertedPairs << "\n");
        return PreservedAnalyses::none();
    }

   private:
    bool modelLoopBackEdges_;
};

char RebuildTDMBarrierPass::ID = 0;

}  // namespace

namespace stinkytofu {
std::unique_ptr<Pass> createRebuildTDMBarrierPass(bool modelLoopBackEdges) {
    return std::make_unique<RebuildTDMBarrierPass>(modelLoopBackEdges);
}
}  // namespace stinkytofu
