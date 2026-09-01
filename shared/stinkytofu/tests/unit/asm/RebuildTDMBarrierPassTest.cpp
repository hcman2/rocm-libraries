// Copyright Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "TestHelpers.hpp"
#include "stinkytofu/analysis/AnalysisRegistration.hpp"
#include "stinkytofu/bindings/python/Module.hpp"
#include "stinkytofu/core/PassManager.hpp"
#include "stinkytofu/ir/asm/StinkyAsmIR.hpp"
#include "stinkytofu/support/Casting.hpp"
#include "stinkytofu/transforms/asm/RebuildTDMBarrierPass.hpp"

using namespace stinkytofu;
using namespace stinkytofu::test;

namespace {

class RebuildTDMBarrierPassTest : public ::testing::Test {
   protected:
    GfxArchID arch = GfxArchID::Gfx1250;
    GemmTileConfig config;
    std::unique_ptr<Function> func;
    BasicBlock* entry = nullptr;
    AnalysisManager am;

    void SetUp() override {
        config.arch = {12, 5, 0};
        config.NumWaves = 4;
        func = std::make_unique<Function>("rebuild_tdm_barrier_test");
        setFunctionArch(*func, arch);
        entry = func->createBasicBlock("entry");
        registerAllAnalyses(am);
    }

    void runPass(bool modelLoopBackEdges = true) {
        PassContext ctx;
        ctx.setGemmTileConfig(config);
        auto pass = createRebuildTDMBarrierPass(modelLoopBackEdges);
        pass->run(*func, ctx, am);
    }

    StinkyInstruction* createBarrier(BasicBlock* bb, GFX opcode, int id,
                                     std::vector<int> tokens = {}) {
        AsmIRBuilder builder(*bb, arch);
        StinkyInstruction* inst = builder.create(getMCIDByUOp(opcode, arch));
        inst->addSrcReg(StinkyRegister(id));
        if (!tokens.empty()) inst->addModifier<MemTokenData>(MemTokenData{std::move(tokens)});
        return inst;
    }

    int count(GFX opcode) const {
        int result = 0;
        for (const BasicBlock& bb : *func)
            for (const IRBase& ir : bb) {
                const auto* inst = dyn_cast<StinkyInstruction>(&ir);
                if (inst != nullptr && inst->getUnifiedOpcode() == opcode) ++result;
            }
        return result;
    }

    std::vector<int> firstBarrierTokens(GFX opcode) const {
        for (const BasicBlock& bb : *func)
            for (const IRBase& ir : bb) {
                const auto* inst = dyn_cast<StinkyInstruction>(&ir);
                if (inst == nullptr || inst->getUnifiedOpcode() != opcode) continue;
                const auto* tokenData = inst->getModifier<MemTokenData>();
                if (tokenData != nullptr) return tokenData->tokens;
            }
        return {};
    }
};

TEST_F(RebuildTDMBarrierPassTest, InsertsBarrierForWriteToReadTransition) {
    createTensorLoadInBlock(entry, arch, 0, 4, {3});
    createDSLoadInBlock(entry, arch, 0, 8, {3});

    runPass();

    EXPECT_EQ(count(GFX::s_barrier_signal), 1);
    EXPECT_EQ(count(GFX::s_barrier_wait), 1);
    EXPECT_EQ(firstBarrierTokens(GFX::s_barrier_signal), (std::vector<int>{3}));
}

TEST_F(RebuildTDMBarrierPassTest, MergesConflictingTokensAtSameAccess) {
    createTensorLoadInBlock(entry, arch, 0, 4, {5, 2});
    createDSLoadInBlock(entry, arch, 0, 8, {2, 5, 5});

    runPass();

    EXPECT_EQ(count(GFX::s_barrier_signal), 1);
    EXPECT_EQ(firstBarrierTokens(GFX::s_barrier_signal), (std::vector<int>{2, 5}));
}

TEST_F(RebuildTDMBarrierPassTest, ReplacesManagedAllWavePair) {
    createTensorLoadInBlock(entry, arch, 0, 4, {1});
    createBarrier(entry, GFX::s_barrier_signal, -1, {1});
    createBarrier(entry, GFX::s_barrier_wait, -1, {1});
    createDSLoadInBlock(entry, arch, 0, 8, {1});

    runPass();

    EXPECT_EQ(count(GFX::s_barrier_signal), 1);
    EXPECT_EQ(count(GFX::s_barrier_wait), 1);
}

TEST_F(RebuildTDMBarrierPassTest, PreservesClusterAndUnmanagedBarriers) {
    createBarrier(entry, GFX::s_barrier_signal, -3);
    createBarrier(entry, GFX::s_barrier_wait, -3);
    createBarrier(entry, GFX::s_barrier_signal, -1);
    createBarrier(entry, GFX::s_barrier_wait, -1);

    runPass();

    EXPECT_EQ(count(GFX::s_barrier_signal), 2);
    EXPECT_EQ(count(GFX::s_barrier_wait), 2);
}

TEST_F(RebuildTDMBarrierPassTest, PGR1ModelsLoopBackEdge) {
    BasicBlock* loop = func->createBasicBlock("loop");
    func->addEdge(entry, loop);
    func->addEdge(loop, loop);
    createDSLoadInBlock(loop, arch, 0, 8, {7});
    createTensorLoadInBlock(loop, arch, 0, 4, {7});

    runPass();

    // The loop carries a write phase into its first read, and the in-body
    // read-to-write transition requires a second pair.
    EXPECT_EQ(count(GFX::s_barrier_signal), 2);
    EXPECT_EQ(count(GFX::s_barrier_wait), 2);
}

TEST_F(RebuildTDMBarrierPassTest, PGR2IgnoresLoopBackEdge) {
    BasicBlock* loop = func->createBasicBlock("loop");
    func->addEdge(entry, loop);
    func->addEdge(loop, loop);
    createDSLoadInBlock(loop, arch, 0, 8, {7});
    createTensorLoadInBlock(loop, arch, 0, 4, {7});

    runPass(false);

    // PGR2+ pre-stages the steady-state data in its prologue, so only the
    // in-body read-to-write transition needs a barrier pair.
    EXPECT_EQ(count(GFX::s_barrier_signal), 1);
    EXPECT_EQ(count(GFX::s_barrier_wait), 1);
}

TEST_F(RebuildTDMBarrierPassTest, ModelsDiamondJoinConservatively) {
    BasicBlock* writer = func->createBasicBlock("writer");
    BasicBlock* reader = func->createBasicBlock("reader");
    BasicBlock* join = func->createBasicBlock("join");
    func->addEdge(entry, writer);
    func->addEdge(entry, reader);
    func->addEdge(writer, join);
    func->addEdge(reader, join);
    createTensorLoadInBlock(writer, arch, 0, 4, {9});
    createDSLoadInBlock(reader, arch, 0, 8, {9});
    createDSLoadInBlock(join, arch, 4, 12, {9});

    runPass();

    // Only the writer predecessor conflicts with the join read. The barrier is
    // conservatively placed at the join and is safe on both incoming paths.
    EXPECT_EQ(count(GFX::s_barrier_signal), 1);
    EXPECT_EQ(count(GFX::s_barrier_wait), 1);
}

int runBackendPipelineAndCountBarriers(bool enabled) {
    StinkyAsmModule::ModuleOptions opts{};
    opts.OptLevel = 3;
    opts.EnableWaitCntInsertion = false;
    opts.EnableTDMBarrierRebuild = enabled;
    opts.TDMBarrierRebuildEligible = true;
    StinkyAsmModule module("pipeline_gate_test", {12, 5, 0}, opts);
    module.addGroup("loopWithPrefetch");
    module.addGroup("noLoadLoopBody");

    const std::string groupName = "loopWithPrefetch";
    const std::vector<const std::string*> groups = {&groupName};
    BasicBlock* bb = module.getFunction().getEntryBlock();
    const size_t beforeWrite = bb->size();
    createTensorLoadInBlock(bb, GfxArchID::Gfx1250, 0, 4, {11});
    module.updateInstructionGroups(groups, beforeWrite);
    const size_t beforeRead = bb->size();
    createDSLoadInBlock(bb, GfxArchID::Gfx1250, 0, 8, {11});
    module.updateInstructionGroups(groups, beforeRead);

    module.runOptimizationPipeline();

    int signals = 0;
    for (const BasicBlock& block : module.getFunction())
        for (const IRBase& ir : block) {
            const auto* inst = dyn_cast<StinkyInstruction>(&ir);
            if (inst != nullptr && inst->getUnifiedOpcode() == GFX::s_barrier_signal) ++signals;
        }
    return signals;
}

TEST(RebuildTDMBarrierPipelineTest, ModuleOptionEnablesEligiblePass) {
    EXPECT_EQ(runBackendPipelineAndCountBarriers(true), 1);
}

TEST(RebuildTDMBarrierPipelineTest, ModuleOptionDisablesEligiblePass) {
    EXPECT_EQ(runBackendPipelineAndCountBarriers(false), 0);
}

}  // namespace
