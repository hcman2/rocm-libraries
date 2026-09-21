/* ************************************************************************
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
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

#include <gtest/gtest.h>

#include <memory>

#include "TestHelpers.hpp"
#include "stinkytofu/analysis/AnalysisRegistration.hpp"
#include "stinkytofu/core/PassManager.hpp"
#include "stinkytofu/hardware/ArchHelper.hpp"
#include "stinkytofu/ir/asm/StinkyAsmIR.hpp"
#include "stinkytofu/ir/asm/StinkyModifiers.hpp"
#include "stinkytofu/transforms/asm/WaitAwareScheduleRepairPass.hpp"

using namespace stinkytofu;
using namespace stinkytofu::test;

class WaitAwareScheduleRepairPassTest : public ::testing::Test {
   protected:
    static constexpr GfxArchID kArch = GfxArchID::Gfx1250;

    void SetUp() override {
        func = std::make_unique<Function>("test");
        setFunctionArch(*func, kArch);
        bb = func->createBasicBlock("entry");
        registerAllAnalyses(am);
    }

    StinkyInstruction* addWmma(int destReg, int src0Reg) {
        AsmIRBuilder builder(*bb, kArch);
        StinkyInstruction* inst =
            builder.create(getMCIDByUOp(GFX::v_wmma_f32_16x16x16_bf16, kArch));
        inst->addDestReg(StinkyRegister("v", destReg, 8));
        inst->addSrcReg(StinkyRegister("v", src0Reg, 8));
        inst->addSrcReg(StinkyRegister("v", src0Reg + 8, 8));
        inst->addSrcReg(StinkyRegister("v", destReg, 8));
        return inst;
    }

    StinkyInstruction* addScalarTensorAddress() {
        AsmIRBuilder builder(*bb, kArch);
        StinkyInstruction* inst = builder.create(getMCIDByUOp(GFX::s_add_i32, kArch));
        inst->addDestReg(StinkyRegister("s", 220, 1));
        inst->addSrcReg(StinkyRegister("s", 240, 1));
        inst->addSrcReg(StinkyRegister(16));
        return inst;
    }

    void addWaitDscntZero() {
        AsmIRBuilder builder(*bb, kArch);
        StinkyInstruction* inst = builder.create(getMCIDByUOp(GFX::s_wait_dscnt, kArch));
        inst->addSrcReg(StinkyRegister(0));
        inst->addModifier<SWaitCntData>(
            SWaitCntData{/*vlcnt=*/-1, /*vscnt=*/-1, /*dlcnt=*/0, /*dscnt=*/0});
    }

    void runPass(bool preserve3LdsbTensorOrder) {
        auto pass = createWaitAwareScheduleRepairPass(/*kSlotsToMovePastAnchor=*/1,
                                                      preserve3LdsbTensorOrder);
        ASSERT_NE(pass, nullptr);
        PassContext ctx;
        ctx.setGemmTileConfig(func->getGemmTileConfig());
        pass->run(*func, ctx, am);
    }

    size_t positionOf(const StinkyInstruction* target) const {
        size_t position = 0;
        for (const IRBase& ir : *bb) {
            if (&ir == target) return position;
            ++position;
        }
        return bb->size();
    }

    void createTensorRepairWindow(StinkyInstruction*& tensorLoad, StinkyInstruction*& dsLoad) {
        addWmma(/*destReg=*/100, /*src0Reg=*/300);
        addScalarTensorAddress();
        tensorLoad = createTensorLoadInBlock(bb, kArch, /*src0Reg=*/220,
                                             /*src1Reg=*/224, /*memTokens=*/{2});
        tensorLoad->addDestReg(StinkyRegister(RegType::LDS, 2, 1));
        dsLoad = createDsReadB128InBlock(bb, kArch, /*destReg=*/400, /*addrReg=*/500);
        dsLoad->addSrcReg(StinkyRegister(RegType::LDS, 1, 1));
        addWaitDscntZero();
        addWmma(/*destReg=*/120, /*src0Reg=*/400);
    }

    std::unique_ptr<Function> func;
    BasicBlock* bb = nullptr;
    AnalysisManager am;
};

TEST_F(WaitAwareScheduleRepairPassTest, DefaultModeLeavesTensorOrderUnprotected) {
    StinkyInstruction* tensorLoad = nullptr;
    StinkyInstruction* dsLoad = nullptr;
    createTensorRepairWindow(tensorLoad, dsLoad);

    runPass(/*preserve3LdsbTensorOrder=*/false);

    EXPECT_LT(positionOf(dsLoad), positionOf(tensorLoad));
}

TEST_F(WaitAwareScheduleRepairPassTest, ThreeLdsBuffersPreserveTensorAheadOfDsLoad) {
    StinkyInstruction* tensorLoad = nullptr;
    StinkyInstruction* dsLoad = nullptr;
    createTensorRepairWindow(tensorLoad, dsLoad);

    runPass(/*preserve3LdsbTensorOrder=*/true);

    EXPECT_LT(positionOf(tensorLoad), positionOf(dsLoad));
}
