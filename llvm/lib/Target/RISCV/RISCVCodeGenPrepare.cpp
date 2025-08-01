//===----- RISCVCodeGenPrepare.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is a RISC-V specific version of CodeGenPrepare.
// It munges the code in the input function to better prepare it for
// SelectionDAG-based code generation. This works around limitations in it's
// basic-block-at-a-time approach.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVTargetMachine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsRISCV.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-codegenprepare"
#define PASS_NAME "RISC-V CodeGenPrepare"

namespace {

class RISCVCodeGenPrepare : public FunctionPass,
                            public InstVisitor<RISCVCodeGenPrepare, bool> {
  const DataLayout *DL;
  LLVMContext *Ctx;
  const DominatorTree *DT;
  const RISCVSubtarget *ST;
  SmallVector<Instruction *, 2> DeadInstructions;

public:
  static char ID;

  RISCVCodeGenPrepare() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override;

  StringRef getPassName() const override { return PASS_NAME; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<TargetPassConfig>();
  }

  bool visitInstruction(Instruction &I) { return false; }
  bool visitAnd(BinaryOperator &BO);
  bool visitIntrinsicInst(IntrinsicInst &I);
  bool expandVPStrideLoad(IntrinsicInst &I);
  bool widenVPMerge(IntrinsicInst &I);
  void visitPDIVSTEPSRIntrinsic(IntrinsicInst &I);
  void writePDIVSTEPSRLoop(IntrinsicInst &I);
  uint64_t getGeneratorPolynomial(uint64_t QP, unsigned Degree);
  uint64_t computeReversedBarrettConstant(uint64_t GeneratorPolynomial);
  uint64_t computeReversedGStarShifted(uint64_t GeneratorPolynomial,
                                       unsigned Degree, unsigned XLen);

  bool matchRISCVAddAbsDiff(IntrinsicInst &I);
};

} // end anonymous namespace

// Try to optimize (i64 (and (zext/sext (i32 X), C1))) if C1 has bit 31 set,
// but bits 63:32 are zero. If we know that bit 31 of X is 0, we can fill
// the upper 32 bits with ones.
bool RISCVCodeGenPrepare::visitAnd(BinaryOperator &BO) {
  if (!ST->is64Bit())
    return false;

  if (!BO.getType()->isIntegerTy(64))
    return false;

  using namespace PatternMatch;

  // Left hand side should be a zext nneg.
  Value *LHSSrc;
  if (!match(BO.getOperand(0), m_NNegZExt(m_Value(LHSSrc))))
    return false;

  if (!LHSSrc->getType()->isIntegerTy(32))
    return false;

  // Right hand side should be a constant.
  Value *RHS = BO.getOperand(1);

  auto *CI = dyn_cast<ConstantInt>(RHS);
  if (!CI)
    return false;
  uint64_t C = CI->getZExtValue();

  // Look for constants that fit in 32 bits but not simm12, and can be made
  // into simm12 by sign extending bit 31. This will allow use of ANDI.
  // TODO: Is worth making simm32?
  if (!isUInt<32>(C) || isInt<12>(C) || !isInt<12>(SignExtend64<32>(C)))
    return false;

  // Sign extend the constant and replace the And operand.
  C = SignExtend64<32>(C);
  BO.setOperand(1, ConstantInt::get(RHS->getType(), C));

  return true;
}

// With EVL tail folding, an AnyOf reduction will generate an i1 vp.merge like
// follows:
//
// loop:
//   %phi = phi <vscale x 4 x i1> [ zeroinitializer, %entry ], [ %rec, %loop ]
//   %cmp = icmp ...
//   %rec = call <vscale x 4 x i1> @llvm.vp.merge(%cmp, i1 true, %phi, %evl)
//   ...
// middle:
//   %res = call i1 @llvm.vector.reduce.or(<vscale x 4 x i1> %rec)
//
// However RVV doesn't have any tail undisturbed mask instructions and so we
// need a convoluted sequence of mask instructions to lower the i1 vp.merge: see
// llvm/test/CodeGen/RISCV/rvv/vpmerge-sdnode.ll.
//
// To avoid that this widens the i1 vp.merge to an i8 vp.merge, which will
// generate a single vmerge.vim:
//
// loop:
//   %phi = phi <vscale x 4 x i8> [ zeroinitializer, %entry ], [ %rec, %loop ]
//   %cmp = icmp ...
//   %rec = call <vscale x 4 x i8> @llvm.vp.merge(%cmp, i8 true, %phi, %evl)
//   %trunc = trunc <vscale x 4 x i8> %rec to <vscale x 4 x i1>
//   ...
// middle:
//   %res = call i1 @llvm.vector.reduce.or(<vscale x 4 x i1> %rec)
//
// The trunc will normally be sunk outside of the loop, but even if there are
// users inside the loop it is still profitable.
bool RISCVCodeGenPrepare::widenVPMerge(IntrinsicInst &II) {
  if (!II.getType()->getScalarType()->isIntegerTy(1))
    return false;

  Value *Mask, *True, *PhiV, *EVL;
  using namespace PatternMatch;
  if (!match(&II,
             m_Intrinsic<Intrinsic::vp_merge>(m_Value(Mask), m_Value(True),
                                              m_Value(PhiV), m_Value(EVL))))
    return false;

  auto *Phi = dyn_cast<PHINode>(PhiV);
  if (!Phi || !Phi->hasOneUse() || Phi->getNumIncomingValues() != 2 ||
      !match(Phi->getIncomingValue(0), m_Zero()) ||
      Phi->getIncomingValue(1) != &II)
    return false;

  Type *WideTy =
      VectorType::get(IntegerType::getInt8Ty(II.getContext()),
                      cast<VectorType>(II.getType())->getElementCount());

  IRBuilder<> Builder(Phi);
  PHINode *WidePhi = Builder.CreatePHI(WideTy, 2);
  WidePhi->addIncoming(ConstantAggregateZero::get(WideTy),
                       Phi->getIncomingBlock(0));
  Builder.SetInsertPoint(&II);
  Value *WideTrue = Builder.CreateZExt(True, WideTy);
  Value *WideMerge = Builder.CreateIntrinsic(Intrinsic::vp_merge, {WideTy},
                                             {Mask, WideTrue, WidePhi, EVL});
  WidePhi->addIncoming(WideMerge, Phi->getIncomingBlock(1));
  Value *Trunc = Builder.CreateTrunc(WideMerge, II.getType());

  II.replaceAllUsesWith(Trunc);

  // Break the cycle and delete the old chain.
  Phi->setIncomingValue(1, Phi->getIncomingValue(0));
  llvm::RecursivelyDeleteTriviallyDeadInstructions(&II);

  return true;
}

// LLVM vector reduction intrinsics return a scalar result, but on RISC-V vector
// reduction instructions write the result in the first element of a vector
// register. So when a reduction in a loop uses a scalar phi, we end up with
// unnecessary scalar moves:
//
// loop:
// vfmv.s.f v10, fa0
// vfredosum.vs v8, v8, v10
// vfmv.f.s fa0, v8
//
// This mainly affects ordered fadd reductions and VP reductions that have a
// scalar start value, since other types of reduction typically use element-wise
// vectorisation in the loop body. This tries to vectorize any scalar phis that
// feed into these reductions:
//
// loop:
// %phi = phi <float> [ ..., %entry ], [ %acc, %loop ]
// %acc = call float @llvm.vector.reduce.fadd.nxv2f32(float %phi,
//                                                    <vscale x 2 x float> %vec)
//
// ->
//
// loop:
// %phi = phi <vscale x 2 x float> [ ..., %entry ], [ %acc.vec, %loop ]
// %phi.scalar = extractelement <vscale x 2 x float> %phi, i64 0
// %acc = call float @llvm.vector.reduce.fadd.nxv2f32(float %x,
//                                                    <vscale x 2 x float> %vec)
// %acc.vec = insertelement <vscale x 2 x float> poison, float %acc.next, i64 0
//
// Which eliminates the scalar -> vector -> scalar crossing during instruction
// selection.
bool RISCVCodeGenPrepare::visitIntrinsicInst(IntrinsicInst &I) {
  if (expandVPStrideLoad(I))
    return true;

  if (widenVPMerge(I))
    return true;

  if (I.getIntrinsicID() != Intrinsic::vector_reduce_fadd &&
      !isa<VPReductionIntrinsic>(&I))
    return false;

  auto *PHI = dyn_cast<PHINode>(I.getOperand(0));
  if (!PHI || !PHI->hasOneUse() ||
      !llvm::is_contained(PHI->incoming_values(), &I))
    return false;

  Type *VecTy = I.getOperand(1)->getType();
  IRBuilder<> Builder(PHI);
  auto *VecPHI = Builder.CreatePHI(VecTy, PHI->getNumIncomingValues());

  for (auto *BB : PHI->blocks()) {
    Builder.SetInsertPoint(BB->getTerminator());
    Value *InsertElt = Builder.CreateInsertElement(
        VecTy, PHI->getIncomingValueForBlock(BB), (uint64_t)0);
    VecPHI->addIncoming(InsertElt, BB);
  }

  Builder.SetInsertPoint(&I);
  I.setOperand(0, Builder.CreateExtractElement(VecPHI, (uint64_t)0));

  PHI->eraseFromParent();

  return true;
}

// Always expand zero strided loads so we match more .vx splat patterns, even if
// we have +optimized-zero-stride-loads. RISCVDAGToDAGISel::Select will convert
// it back to a strided load if it's optimized.
bool RISCVCodeGenPrepare::expandVPStrideLoad(IntrinsicInst &II) {
  Value *BasePtr, *VL;

  using namespace PatternMatch;
  if (!match(&II, m_Intrinsic<Intrinsic::experimental_vp_strided_load>(
                      m_Value(BasePtr), m_Zero(), m_AllOnes(), m_Value(VL))))
    return false;

  // If SEW>XLEN then a splat will get lowered as a zero strided load anyway, so
  // avoid expanding here.
  if (II.getType()->getScalarSizeInBits() > ST->getXLen())
    return false;

  if (!isKnownNonZero(VL, {*DL, DT, nullptr, &II}))
    return false;

  auto *VTy = cast<VectorType>(II.getType());

  IRBuilder<> Builder(&II);
  Type *STy = VTy->getElementType();
  Value *Val = Builder.CreateLoad(STy, BasePtr);
  Value *Res = Builder.CreateIntrinsic(Intrinsic::experimental_vp_splat, {VTy},
                                       {Val, II.getOperand(2), VL});

  II.replaceAllUsesWith(Res);
  II.eraseFromParent();
  return true;
}

uint64_t RISCVCodeGenPrepare::getGeneratorPolynomial(uint64_t QP,
                                                     unsigned Degree) {
  // all the coefficients past the Degree must be zero
  uint64_t maskAllHigh = ~((1 << (Degree + 1)) - 1);
  assert((QP & maskAllHigh) == 0);
  uint64_t ExplicitReversed = QP | 1;
  uint64_t GeneratorPolynomial = 0;
  for (unsigned i = 0; i < Degree + 1; ++i) {
    if (ExplicitReversed & (1 << i)) {
      GeneratorPolynomial |= 1 << (Degree - i);
    }
  }
  return GeneratorPolynomial;
}

uint64_t RISCVCodeGenPrepare::computeReversedGStarShifted(
    uint64_t GeneratorPolynomial, unsigned Degree, unsigned XLen) {
  return llvm::reverseBits<uint64_t>(GeneratorPolynomial ^ (1 << Degree)) >>
         (XLen - Degree) << 1;
}

uint64_t RISCVCodeGenPrepare::computeReversedBarrettConstant(
    uint64_t GeneratorPolynomial) {
  unsigned Degree = 0;
  for (unsigned i = 0; i < 64; ++i) {
    if ((GeneratorPolynomial >> i) & 1) {
      Degree = i;
    }
  }
  uint64_t X = (((uint64_t)1) << (2 * Degree));
  uint64_t Quotient = 0;
  for (int i = 2 * Degree; i >= (int)Degree; --i) {
    if ((X >> i) & 1) {
      X ^= (GeneratorPolynomial << (i - Degree));
      Quotient |= 1 << (i - Degree);
    }
  }
  // TODO: generalize this
  // discard the leading term
  uint64_t Reversed = 0;
  // if we do it up to 64, we will need the shift
  for (unsigned i = 0; i <= Degree; ++i) {
    Reversed |= ((Quotient >> (Degree - i)) & 1) << i;
  }
  return Reversed;
}

void RISCVCodeGenPrepare::writePDIVSTEPSRLoop(IntrinsicInst &I) {
  ConstantInt *Divisor = dyn_cast<ConstantInt>(I.getOperand(1));
  assert(Divisor && "Divisor has to be constant!");
  ConstantInt *NumIterations = dyn_cast<ConstantInt>(I.getOperand(2));
  assert(NumIterations && "Number of iterations has to be constant!");

  IRBuilder<> B(&I);
  Value *Res = I.getOperand(0);
  IntegerType *XLenTy = IntegerType::getIntNTy(*Ctx, ST->getXLen());
  ConstantInt *ReversedDivisor =
      ConstantInt::get(XLenTy, (Divisor->getLimitedValue()) | 1);
  for (uint64_t i = 0; i < NumIterations->getLimitedValue(); ++i) {
    Value *ExtractLastBit = B.CreateAnd(Res, ConstantInt::get(XLenTy, 1));
    Value *TestLastBit =
        B.CreateCmp(CmpInst::Predicate::ICMP_EQ, ExtractLastBit,
                    ConstantInt::get(XLenTy, 1));
    Value *Xored = B.CreateXor(Res, ReversedDivisor);
    Value *Sel = B.CreateSelect(TestLastBit, Xored, Res);
    Res = B.CreateLShr(Sel, 1);
  }
  I.replaceAllUsesWith(Res);
  DeadInstructions.push_back(&I);
  return;
}

void RISCVCodeGenPrepare::visitPDIVSTEPSRIntrinsic(IntrinsicInst &I) {
  // Match the following pattern:
  // %0 = and i16 %M0, %Mask
  // %1 = xor i16 %0, %C
  // %2 = zext i16 %1 to i64
  // %3 = tail call i64 @llvm.riscv.pdivstepsr.i64(i64 %2, i64 %Divisor1, i64
  // %NumSteps1) %4 = trunc i64 %3 to i16 %5 = lshr i16 %M1, %RShiftAmt %6 = xor
  // i16 %5, %4 %7 = zext i16 %6 to i64 %8 = tail call i64
  // @llvm.riscv.pdivstepsr.i64(i64 %Dividend, i64 %Divisor0, i64 %NumSteps0) %9
  // = trunc i64 %8 to i16
  Value *Dividend = I.getOperand(0);
  ConstantInt *Divisor0 = dyn_cast<ConstantInt>(I.getOperand(1));
  ConstantInt *NumSteps0 = dyn_cast<ConstantInt>(I.getOperand(2));

  using namespace PatternMatch;
  Value *M0 = nullptr;
  Value *M1 = nullptr;
  Value *C = nullptr;
  ConstantInt *RShiftAmt = nullptr;
  ConstantInt *Divisor1 = nullptr;
  ConstantInt *NumSteps1 = nullptr;
  ConstantInt *Mask = nullptr;
  Value *FirstPDIVSTEPSRIntr = nullptr;
  bool MatchSuccess = match(
      Dividend, m_ZExt(m_c_Xor(m_LShr(m_Value(M1), m_ConstantInt(RShiftAmt)),
                               m_Trunc(m_Value(FirstPDIVSTEPSRIntr)))));
  if (!MatchSuccess) {
    writePDIVSTEPSRLoop(I);
    return;
  }

  // TODO: check only one use of FirstPDIVSTEPSRIntr
  MatchSuccess =
      match(FirstPDIVSTEPSRIntr,
            m_Intrinsic<Intrinsic::riscv_pdivstepsr>(
                m_ZExt(m_c_Xor(m_c_And(m_Value(M0), m_ConstantInt(Mask)),
                               m_Value(C))),
                m_ConstantInt(Divisor1), m_ConstantInt(NumSteps1)));
  if (!MatchSuccess) {
    writePDIVSTEPSRLoop(I);
    return;
  }
  if (Divisor0 != Divisor1) {
    writePDIVSTEPSRLoop(I);
    return;
  }
  ConstantInt *Divisor = Divisor0;
  if (M0 != M1) {
    writePDIVSTEPSRLoop(I);
    return;
  }
  Value *M = M0;
  if (RShiftAmt->getLimitedValue() != NumSteps1->getLimitedValue()) {
    writePDIVSTEPSRLoop(I);
    return;
  }

  unsigned XLen = ST->getXLen();
  IntegerType *XLenTy = IntegerType::getIntNTy(*Ctx, XLen);
  ConstantInt *TotalNumSteps = ConstantInt::get(
      XLenTy, NumSteps0->getLimitedValue() + NumSteps1->getLimitedValue());

  KnownBits KnownDividend(XLen);
  computeKnownBits(Dividend, KnownDividend, *DL);
  unsigned S = XLen - KnownDividend.countMinLeadingZeros();

  if (S != TotalNumSteps->getLimitedValue()) {
    writePDIVSTEPSRLoop(I);
    return;
  }

  IRBuilder<> B(&I);
  C = B.CreateXor(C, M);
  C = B.CreateZExt(C, XLenTy);
  unsigned T = Divisor->getValue().getActiveBits() - 1;
  uint64_t GeneratorPolynomial =
      getGeneratorPolynomial(Divisor->getZExtValue(), T);
  Module *Mod = I.getParent()->getParent()->getParent();
  Function *CLMULFunction =
      Intrinsic::getOrInsertDeclaration(Mod, Intrinsic::riscv_clmul, XLenTy);
  Function *CLMULHFunction =
      Intrinsic::getOrInsertDeclaration(Mod, Intrinsic::riscv_clmulh, XLenTy);

  ConstantInt *ReversedBarretConstant = ConstantInt::get(
      XLenTy, computeReversedBarrettConstant(GeneratorPolynomial));

  C = B.CreateCall(CLMULFunction, {C, ReversedBarretConstant});
  C = B.CreateShl(C, XLen - T);
  ConstantInt *ReversedGStarShifted = ConstantInt::get(
      XLenTy, computeReversedGStarShifted(GeneratorPolynomial, T, XLen));
  Value *R = B.CreateCall(CLMULHFunction, {C, ReversedGStarShifted});

  I.replaceAllUsesWith(R);
  DeadInstructions.push_back(&I);
  return;
}

static Value *stripVZExt(const Value *X, Type *ElTy, Type *FromElTy) {
  VectorType *VTy = dyn_cast<VectorType>(X->getType());
  if (!VTy)
    return nullptr;
  if (VTy->getElementType() != ElTy)
    return nullptr;
  // TODO: non-zero values
  if (dyn_cast<ConstantAggregateZero>(X))
    return ConstantAggregateZero::get(
        VectorType::get(FromElTy, VTy->getElementCount()));

  const ZExtInst *ZExt = dyn_cast<ZExtInst>(X);
  if (!ZExt)
    return nullptr;
  VectorType *VTyIn = dyn_cast<VectorType>(ZExt->getSrcTy());
  if (!VTyIn)
    return nullptr;
  if (VTyIn->getElementType() != FromElTy)
    return nullptr;
  return ZExt->getOperand(0);
}

bool RISCVCodeGenPrepare::matchRISCVAddAbsDiff(IntrinsicInst &II) {
  if (!ST->hasVendorXVentanaVwadaccu())
    return false;
  // TODO: strictly speaking we don't need this check, we'll remove it when we
  // upstream this.
  if (!II.hasOneUse())
    return false;

  VectorType *VTy = dyn_cast<VectorType>(II.getType());
  if (!VTy)
    return false;

  ZExtInst *ZExtToI32 = dyn_cast<ZExtInst>(II.use_begin()->getUser());
  if (!ZExtToI32)
    return false;
  if (!stripVZExt(ZExtToI32, Type::getInt32Ty(*Ctx), Type::getInt16Ty(*Ctx)))
    return false;

  Value *X = nullptr;
  Value *Y = nullptr;
  using namespace PatternMatch;
  if (!match(II.getArgOperand(0), m_Sub(m_Value(X), m_Value(Y))))
    return false;

  Value *Arg0 = stripVZExt(X, Type::getInt16Ty(*Ctx), Type::getInt8Ty(*Ctx));
  if (!Arg0)
    return false;
  Value *Arg1 = stripVZExt(Y, Type::getInt16Ty(*Ctx), Type::getInt8Ty(*Ctx));
  if (!Arg1)
    return false;

  ElementCount EC = cast<VectorType>(ZExtToI32->getType())->getElementCount();
  if (EC.isScalable())
    return false;
  unsigned NumOfElements = EC.getKnownMinValue();
  // TODO: handle other vector sizes.
  if (NumOfElements != 16 && NumOfElements != 8)
    return false;

  Value *AccumulateInto = nullptr;
  Instruction *InstToReplace = nullptr;
  if (ZExtToI32->hasOneUse()) {
    Instruction *AddInst =
        dyn_cast<Instruction>(ZExtToI32->use_begin()->getUser());
    if (AddInst && (AddInst->getOpcode() == Instruction::Add)) {
      Value *OtherVal = AddInst->getOperand(1);
      if (OtherVal == ZExtToI32)
        OtherVal = AddInst->getOperand(0);
      AccumulateInto = OtherVal;
      InstToReplace = AddInst;
    }
  }
  if (!AccumulateInto) {
    AccumulateInto =
        Constant::getNullValue(VectorType::get(Type::getInt32Ty(*Ctx), EC));
    InstToReplace = ZExtToI32;
  }

  IRBuilder<> Builder(&II);
  Value *Res = Builder.CreateIntrinsic(
      Intrinsic::riscv_addabsdiff,
      {VectorType::get(Type::getInt32Ty(*Ctx), EC),
       VectorType::get(Type::getInt8Ty(*Ctx), EC)},
      {AccumulateInto, Arg0, Arg1,
       ConstantInt::get(IntegerType::getInt64Ty(*Ctx), NumOfElements),
       ConstantInt::get(IntegerType::getInt64Ty(*Ctx),
                        RISCVVType::TAIL_AGNOSTIC)});
  InstToReplace->replaceAllUsesWith(Res);
  DeadInstructions.push_back(InstToReplace);
  return true;
}

bool RISCVCodeGenPrepare::runOnFunction(Function &F) {
  if (skipFunction(F))
    return false;

  auto &TPC = getAnalysis<TargetPassConfig>();
  auto &TM = TPC.getTM<RISCVTargetMachine>();
  ST = &TM.getSubtarget<RISCVSubtarget>(F);

  DL = &F.getDataLayout();
  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  // TODO: do we use the DL declared above or this one?
  // DL = &F.getParent()->getDataLayout();
  Ctx = &F.getContext();

  bool MadeChange = false;
  for (auto &BB : F) {
    for (BasicBlock::reverse_iterator RI = BB.rbegin(); RI != BB.rend(); ++RI) {
      IntrinsicInst *Intr = dyn_cast<IntrinsicInst>(&*RI);
      if (!Intr)
        continue;
      if (Intr->getIntrinsicID() == Intrinsic::riscv_pdivstepsr) {
        visitPDIVSTEPSRIntrinsic(*Intr);
        MadeChange = true;
      }
      if (Intr->getIntrinsicID() == Intrinsic::abs) {
        MadeChange |= matchRISCVAddAbsDiff(*Intr);
      }
      continue;
    }
    for (Instruction &I : llvm::make_early_inc_range(BB))
      MadeChange |= visit(I);
  }

  for (Instruction *I : DeadInstructions) {
    bool InstructionIsDead = RecursivelyDeleteTriviallyDeadInstructions(I);
    assert(InstructionIsDead && "Trying to delete live instruction!");
  }
  DeadInstructions.clear();
  return MadeChange;
}

INITIALIZE_PASS_BEGIN(RISCVCodeGenPrepare, DEBUG_TYPE, PASS_NAME, false, false)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_END(RISCVCodeGenPrepare, DEBUG_TYPE, PASS_NAME, false, false)

char RISCVCodeGenPrepare::ID = 0;

FunctionPass *llvm::createRISCVCodeGenPreparePass() {
  return new RISCVCodeGenPrepare();
}
