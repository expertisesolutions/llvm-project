//===- RISCVLoopIdiomRecognition.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVLoopIdiomRecognition.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsRISCV.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "riscv-lir"

using namespace llvm;

static cl::opt<bool> DisableLIR("riscv-disable-lir", cl::init(false),
                                cl::Hidden, cl::desc("Disable riscv-lir"));
static cl::opt<unsigned> SimplifyLimit(
    "riscv-lir-simplify-limit", cl::init(10000), cl::Hidden,
    cl::desc("Maximum number of simplification steps in riscv-lir"));

namespace llvm {

void initializeRISCVLoopIdiomRecognizeLegacyPassPass(PassRegistry &);
Pass *createRISCVLoopIdiomPass();

} // end namespace llvm

namespace {

class RISCVLoopIdiomRecognize {
public:
  explicit RISCVLoopIdiomRecognize(AliasAnalysis *AA, DominatorTree *DT,
                                   LoopInfo *LF, const TargetLibraryInfo *TLI,
                                   ScalarEvolution *SE)
      : AA(AA), DT(DT), LF(LF), TLI(TLI), SE(SE) {}

  bool run(Loop *L);

private:
  int getSCEVStride(const SCEVAddRecExpr *StoreEv);
  bool isLegalStore(Loop *CurLoop, StoreInst *SI);
  void collectStores(Loop *CurLoop, BasicBlock *BB,
                     SmallVectorImpl<StoreInst *> &Stores);
  bool processCopyingStore(Loop *CurLoop, StoreInst *SI, const SCEV *BECount);
  bool coverLoop(Loop *L, SmallVectorImpl<Instruction *> &Insts) const;
  bool runOnCountableLoop(Loop *L);

  AliasAnalysis *AA;
  const DataLayout *DL;
  DominatorTree *DT;
  LoopInfo *LF;
  const TargetLibraryInfo *TLI;
  ScalarEvolution *SE;
  bool HasMemcpy, HasMemmove;
};

class RISCVLoopIdiomRecognizeLegacyPass : public LoopPass {
public:
  static char ID;

  explicit RISCVLoopIdiomRecognizeLegacyPass() : LoopPass(ID) {
    initializeRISCVLoopIdiomRecognizeLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override {
    return "Recognize RISCV-specific loop idioms";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequiredID(LoopSimplifyID);
    AU.addRequiredID(LCSSAID);
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addPreserved<TargetLibraryInfoWrapperPass>();
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override;
};

struct Simplifier {
  struct Rule {
    using FuncType = std::function<Value *(Instruction *, LLVMContext &)>;
    Rule(StringRef N, FuncType F) : Name(N), Fn(F) {}
    StringRef Name; // For debugging.
    FuncType Fn;
  };

  void addRule(StringRef N, const Rule::FuncType &F) {
    Rules.push_back(Rule(N, F));
  }

private:
  struct WorkListType {
    WorkListType() = default;

    void push_back(Value *V) {
      // Do not push back duplicates.
      if (S.insert(V).second)
        Q.push_back(V);
    }

    Value *pop_front_val() {
      Value *V = Q.front();
      Q.pop_front();
      S.erase(V);
      return V;
    }

    bool empty() const { return Q.empty(); }

  private:
    std::deque<Value *> Q;
    std::set<Value *> S;
  };

  using ValueSetType = std::set<Value *>;

  std::vector<Rule> Rules;

public:
  struct Context {
    using ValueMapType = DenseMap<Value *, Value *>;

    Value *Root;
    ValueSetType Used;   // The set of all cloned values used by Root.
    ValueSetType Clones; // The set of all cloned values.
    LLVMContext &Ctx;

    Context(Instruction *Exp)
        : Ctx(Exp->getParent()->getParent()->getContext()) {
      initialize(Exp);
    }

    ~Context() { cleanup(); }

    void print(raw_ostream &OS, const Value *V) const;
    Value *materialize(BasicBlock *B, BasicBlock::iterator At);

  private:
    friend struct Simplifier;

    void initialize(Instruction *Exp);
    void cleanup();

    template <typename FuncT> void traverse(Value *V, FuncT F);
    void record(Value *V);
    void use(Value *V);
    void unuse(Value *V);

    bool equal(const Instruction *I, const Instruction *J) const;
    Value *find(Value *Tree, Value *Sub) const;
    Value *subst(Value *Tree, Value *OldV, Value *NewV);
    void replace(Value *OldV, Value *NewV);
    void link(Instruction *I, BasicBlock *B, BasicBlock::iterator At);
  };

  Value *simplify(Context &C);
};

struct PE {
  PE(const Simplifier::Context &c, Value *v = nullptr) : C(c), V(v) {}

  const Simplifier::Context &C;
  const Value *V;
};

LLVM_ATTRIBUTE_USED
raw_ostream &operator<<(raw_ostream &OS, const PE &P) {
  P.C.print(OS, P.V ? P.V : P.C.Root);
  return OS;
}

} // end anonymous namespace

char RISCVLoopIdiomRecognizeLegacyPass::ID = 0;

INITIALIZE_PASS_BEGIN(RISCVLoopIdiomRecognizeLegacyPass, "riscv-loop-idiom",
                      "Recognize RISCV-specific loop idioms", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(LCSSAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(RISCVLoopIdiomRecognizeLegacyPass, "riscv-loop-idiom",
                    "Recognize RISCV-specific loop idioms", false, false)

template <typename FuncT>
void Simplifier::Context::traverse(Value *V, FuncT F) {
  WorkListType Q;
  Q.push_back(V);

  while (!Q.empty()) {
    Instruction *U = dyn_cast<Instruction>(Q.pop_front_val());
    if (!U || U->getParent())
      continue;
    if (!F(U))
      continue;
    for (Value *Op : U->operands())
      Q.push_back(Op);
  }
}

void Simplifier::Context::print(raw_ostream &OS, const Value *V) const {
  const auto *U = dyn_cast<const Instruction>(V);
  if (!U) {
    OS << V << '(' << *V << ')';
    return;
  }

  if (U->getParent()) {
    OS << U << '(';
    U->printAsOperand(OS, true);
    OS << ')';
    return;
  }

  unsigned N = U->getNumOperands();
  if (N != 0)
    OS << U << '(';
  OS << U->getOpcodeName();
  for (const Value *Op : U->operands()) {
    OS << ' ';
    print(OS, Op);
  }
  if (N != 0)
    OS << ')';
}

void Simplifier::Context::initialize(Instruction *Exp) {
  // Perform a deep clone of the expression, set Root to the root
  // of the clone, and build a map from the cloned values to the
  // original ones.
  ValueMapType M;
  BasicBlock *Block = Exp->getParent();
  WorkListType Q;
  Q.push_back(Exp);

  while (!Q.empty()) {
    Value *V = Q.pop_front_val();
    if (M.find(V) != M.end())
      continue;
    if (Instruction *U = dyn_cast<Instruction>(V)) {
      if (isa<PHINode>(U) || U->getParent() != Block)
        continue;
      for (Value *Op : U->operands())
        Q.push_back(Op);
      M.insert({U, U->clone()});
    }
  }

  for (std::pair<Value *, Value *> P : M) {
    Instruction *U = cast<Instruction>(P.second);
    for (unsigned i = 0, n = U->getNumOperands(); i != n; ++i) {
      auto F = M.find(U->getOperand(i));
      if (F != M.end())
        U->setOperand(i, F->second);
    }
  }

  auto R = M.find(Exp);
  assert(R != M.end());
  Root = R->second;

  record(Root);
  use(Root);
}

void Simplifier::Context::record(Value *V) {
  auto Record = [this](Instruction *U) -> bool {
    Clones.insert(U);
    return true;
  };
  traverse(V, Record);
}

void Simplifier::Context::use(Value *V) {
  auto Use = [this](Instruction *U) -> bool {
    Used.insert(U);
    return true;
  };
  traverse(V, Use);
}

void Simplifier::Context::unuse(Value *V) {
  if (!isa<Instruction>(V) || cast<Instruction>(V)->getParent() != nullptr)
    return;

  auto Unuse = [this](Instruction *U) -> bool {
    if (!U->use_empty())
      return false;
    Used.erase(U);
    return true;
  };
  traverse(V, Unuse);
}

Value *Simplifier::Context::subst(Value *Tree, Value *OldV, Value *NewV) {
  if (Tree == OldV)
    return NewV;
  if (OldV == NewV)
    return Tree;

  WorkListType Q;
  Q.push_back(Tree);
  while (!Q.empty()) {
    Instruction *U = dyn_cast<Instruction>(Q.pop_front_val());
    // If U is not an instruction, or it's not a clone, skip it.
    if (!U || U->getParent())
      continue;
    for (unsigned i = 0, n = U->getNumOperands(); i != n; ++i) {
      Value *Op = U->getOperand(i);
      if (Op == OldV) {
        U->setOperand(i, NewV);
        unuse(OldV);
      } else {
        Q.push_back(Op);
      }
    }
  }
  return Tree;
}

void Simplifier::Context::replace(Value *OldV, Value *NewV) {
  if (Root == OldV) {
    Root = NewV;
    use(Root);
    return;
  }

  // NewV may be a complex tree that has just been created by one of the
  // transformation rules. We need to make sure that it is commoned with
  // the existing Root to the maximum extent possible.
  // Identify all subtrees of NewV (including NewV itself) that have
  // equivalent counterparts in Root, and replace those subtrees with
  // these counterparts.
  WorkListType Q;
  Q.push_back(NewV);
  while (!Q.empty()) {
    Value *V = Q.pop_front_val();
    Instruction *U = dyn_cast<Instruction>(V);
    if (!U || U->getParent())
      continue;
    if (Value *DupV = find(Root, V)) {
      if (DupV != V)
        NewV = subst(NewV, V, DupV);
    } else {
      for (Value *Op : U->operands())
        Q.push_back(Op);
    }
  }

  // Now, simply replace OldV with NewV in Root.
  Root = subst(Root, OldV, NewV);
  use(Root);
}

void Simplifier::Context::cleanup() {
  for (Value *V : Clones) {
    Instruction *U = cast<Instruction>(V);
    if (!U->getParent())
      U->dropAllReferences();
  }

  for (Value *V : Clones) {
    Instruction *U = cast<Instruction>(V);
    if (!U->getParent())
      U->deleteValue();
  }
}

bool Simplifier::Context::equal(const Instruction *I,
                                const Instruction *J) const {
  if (I == J)
    return true;
  if (!I->isSameOperationAs(J))
    return false;
  if (isa<PHINode>(I))
    return I->isIdenticalTo(J);

  for (unsigned i = 0, n = I->getNumOperands(); i != n; ++i) {
    Value *OpI = I->getOperand(i), *OpJ = J->getOperand(i);
    if (OpI == OpJ)
      continue;
    auto *InI = dyn_cast<const Instruction>(OpI);
    auto *InJ = dyn_cast<const Instruction>(OpJ);
    if (InI && InJ) {
      if (!equal(InI, InJ))
        return false;
    } else if (InI != InJ || !InI)
      return false;
  }
  return true;
}

Value *Simplifier::Context::find(Value *Tree, Value *Sub) const {
  Instruction *SubI = dyn_cast<Instruction>(Sub);
  WorkListType Q;
  Q.push_back(Tree);

  while (!Q.empty()) {
    Value *V = Q.pop_front_val();
    if (V == Sub)
      return V;
    Instruction *U = dyn_cast<Instruction>(V);
    if (!U || U->getParent())
      continue;
    if (SubI && equal(SubI, U))
      return U;
    assert(!isa<PHINode>(U));
    for (Value *Op : U->operands())
      Q.push_back(Op);
  }
  return nullptr;
}

void Simplifier::Context::link(Instruction *I, BasicBlock *B,
                               BasicBlock::iterator At) {
  if (I->getParent())
    return;

  for (Value *Op : I->operands()) {
    if (Instruction *OpI = dyn_cast<Instruction>(Op))
      link(OpI, B, At);
  }

  I->insertInto(B, At);
}

Value *Simplifier::Context::materialize(BasicBlock *B,
                                        BasicBlock::iterator At) {
  if (Instruction *RootI = dyn_cast<Instruction>(Root))
    link(RootI, B, At);
  return Root;
}

Value *Simplifier::simplify(Context &C) {
  WorkListType Q;
  Q.push_back(C.Root);
  unsigned Count = 0;
  const unsigned Limit = SimplifyLimit;

  while (!Q.empty()) {
    if (Count++ >= Limit)
      break;
    Instruction *U = dyn_cast<Instruction>(Q.pop_front_val());
    if (!U || U->getParent() || !C.Used.count(U))
      continue;
    bool Changed = false;
    for (Rule &R : Rules) {
      Value *W = R.Fn(U, C.Ctx);
      if (!W)
        continue;
      Changed = true;
      C.record(W);
      C.replace(U, W);
      Q.push_back(C.Root);
      break;
    }
    if (!Changed) {
      for (Value *Op : U->operands())
        Q.push_back(Op);
    }
  }
  return Count < Limit ? C.Root : nullptr;
}

//===----------------------------------------------------------------------===//
//
//          Implementation of PolynomialMultiplyRecognize
//
//===----------------------------------------------------------------------===//

namespace {

class PolynomialMultiplyRecognize {
public:
  explicit PolynomialMultiplyRecognize(Loop *loop, const DataLayout &dl,
                                       const DominatorTree &dt,
                                       const TargetLibraryInfo &tli,
                                       ScalarEvolution &se)
      : CurLoop(loop), DL(dl), DT(dt), TLI(tli), SE(se) {}

  bool recognize();

private:
  struct ParsedValues {
    ParsedValues() = default;

    Value *M = nullptr;
    Value *P = nullptr;
    Value *Q = nullptr;
    Value *R = nullptr;
    Value *X = nullptr;
    Instruction *Res = nullptr;
    Instruction *ResAfterConvertShiftsToLeft = nullptr;
    unsigned IterCount = 0;
    bool Left = false;
    bool Inv = false;
  };

  using ValueSeq = SetVector<Value *>;

  IntegerType *getCLMULType() const {
    LLVMContext &Ctx = CurLoop->getHeader()->getParent()->getContext();
    return IntegerType::get(Ctx, 64);
  }

  bool isPromotableTo(Value *V, IntegerType *Ty);
  void promoteTo(Instruction *In, IntegerType *DestTy, BasicBlock *LoopB);
  bool promoteTypes(BasicBlock *LoopB, BasicBlock *ExitB);

  Value *getCountIV(BasicBlock *BB);
  bool findCycle(Value *Out, Value *In, ValueSeq &Cycle);
  void classifyCycle(Instruction *DivI, ValueSeq &Cycle, ValueSeq &Early,
                     ValueSeq &Late);
  bool classifyInst(Instruction *UseI, ValueSeq &Early, ValueSeq &Late);
  bool commutesWithShift(Instruction *I);
  bool highBitsAreZero(Value *V, unsigned IterCount);
  bool keepsHighBitsZero(Value *V, unsigned IterCount);
  bool isOperandShifted(Instruction *I, Value *Op);
  bool convertShiftsToLeft(BasicBlock *LoopB, BasicBlock *ExitB,
                           unsigned IterCount, ParsedValues &PV);
  void cleanupLoopBody(BasicBlock *LoopB);

  bool matchLeftShift(SelectInst *SelI, Value *CIV, ParsedValues &PV);
  bool matchRightShift(SelectInst *SelI, ParsedValues &PV);
  bool scanSelect(SelectInst *SI, BasicBlock *LoopB, BasicBlock *PrehB,
                  Value *CIV, ParsedValues &PV, bool PreScan);
  Value *generate(BasicBlock::iterator At, ParsedValues &PV);

  void setupPreSimplifier(Simplifier &S);
  void setupPostSimplifier(Simplifier &S);

  Loop *CurLoop;
  const DataLayout &DL;
  const DominatorTree &DT;
  const TargetLibraryInfo &TLI;
  ScalarEvolution &SE;
};

} // end anonymous namespace

Value *PolynomialMultiplyRecognize::getCountIV(BasicBlock *BB) {
  pred_iterator PI = pred_begin(BB), PE = pred_end(BB);
  if (std::distance(PI, PE) != 2)
    return nullptr;
  BasicBlock *PB = (*PI == BB) ? *std::next(PI) : *PI;

  for (auto I = BB->begin(), E = BB->end(); I != E && isa<PHINode>(I); ++I) {
    auto *PN = cast<PHINode>(I);
    Value *InitV = PN->getIncomingValueForBlock(PB);
    if (!isa<ConstantInt>(InitV) || !cast<ConstantInt>(InitV)->isZero())
      continue;
    Value *IterV = PN->getIncomingValueForBlock(BB);
    auto *BO = dyn_cast<BinaryOperator>(IterV);
    if (!BO)
      continue;
    if (BO->getOpcode() != Instruction::Add)
      continue;
    Value *IncV = nullptr;
    if (BO->getOperand(0) == PN)
      IncV = BO->getOperand(1);
    else if (BO->getOperand(1) == PN)
      IncV = BO->getOperand(0);
    if (IncV == nullptr)
      continue;

    if (auto *T = dyn_cast<ConstantInt>(IncV))
      if (T->getZExtValue() == 1)
        return PN;
  }
  return nullptr;
}

static void replaceAllUsesOfWithIn(Value *I, Value *J, BasicBlock *BB) {
  for (auto UI = I->user_begin(), UE = I->user_end(); UI != UE;) {
    Use &TheUse = UI.getUse();
    ++UI;
    if (auto *II = dyn_cast<Instruction>(TheUse.getUser()))
      if (BB == II->getParent())
        II->replaceUsesOfWith(I, J);
  }
}

bool PolynomialMultiplyRecognize::matchLeftShift(SelectInst *SelI, Value *CIV,
                                                 ParsedValues &PV) {
  // Match the following:
  //   select (X & (1 << i)) != 0 ? R ^ (Q << i) : R
  //   select (X & (1 << i)) == 0 ? R : R ^ (Q << i)
  // The condition may also check for equality with the masked value, i.e
  //   select (X & (1 << i)) == (1 << i) ? R ^ (Q << i) : R
  //   select (X & (1 << i)) != (1 << i) ? R : R ^ (Q << i);

  Value *CondV = SelI->getCondition();
  Value *TrueV = SelI->getTrueValue();
  Value *FalseV = SelI->getFalseValue();

  using namespace PatternMatch;

  // TODO: changed type CmpInst::Predicate P;
  CmpPredicate P;
  Value *A = nullptr, *B = nullptr, *C = nullptr;

  if (!match(CondV, m_ICmp(P, m_And(m_Value(A), m_Value(B)), m_Value(C))) &&
      !match(CondV, m_ICmp(P, m_Value(C), m_And(m_Value(A), m_Value(B)))))
    return false;
  if (P != CmpInst::ICMP_EQ && P != CmpInst::ICMP_NE)
    return false;
  // Matched: select (A & B) == C ? ... : ...
  //          select (A & B) != C ? ... : ...

  Value *X = nullptr, *Sh1 = nullptr;
  // Check (A & B) for (X & (1 << i)):
  if (match(A, m_Shl(m_One(), m_Specific(CIV)))) {
    Sh1 = A;
    X = B;
  } else if (match(B, m_Shl(m_One(), m_Specific(CIV)))) {
    Sh1 = B;
    X = A;
  } else {
    // TODO: Could also check for an induction variable containing single
    // bit shifted left by 1 in each iteration.
    return false;
  }

  bool TrueIfZero;

  // Check C against the possible values for comparison: 0 and (1 << i):
  if (match(C, m_Zero()))
    TrueIfZero = (P == CmpInst::ICMP_EQ);
  else if (C == Sh1)
    TrueIfZero = (P == CmpInst::ICMP_NE);
  else
    return false;

  // So far, matched:
  //   select (X & (1 << i)) ? ... : ...
  // including variations of the check against zero/non-zero value.

  Value *ShouldSameV = nullptr, *ShouldXoredV = nullptr;
  if (TrueIfZero) {
    ShouldSameV = TrueV;
    ShouldXoredV = FalseV;
  } else {
    ShouldSameV = FalseV;
    ShouldXoredV = TrueV;
  }

  Value *Q = nullptr, *R = nullptr, *Y = nullptr, *Z = nullptr;
  Value *T = nullptr;
  if (match(ShouldXoredV, m_Xor(m_Value(Y), m_Value(Z)))) {
    // Matched: select +++ ? ... : Y ^ Z
    //          select +++ ? Y ^ Z : ...
    // where +++ denotes previously checked matches.
    if (ShouldSameV == Y)
      T = Z;
    else if (ShouldSameV == Z)
      T = Y;
    else
      return false;
    R = ShouldSameV;
    // Matched: select +++ ? R : R ^ T
    //          select +++ ? R ^ T : R
    // depending on TrueIfZero.

  } else if (match(ShouldSameV, m_Zero())) {
    // Matched: select +++ ? 0 : ...
    //          select +++ ? ... : 0
    if (!SelI->hasOneUse())
      return false;
    T = ShouldXoredV;
    // Matched: select +++ ? 0 : T
    //          select +++ ? T : 0

    Value *U = *SelI->user_begin();
    if (!match(U, m_Xor(m_Specific(SelI), m_Value(R))) &&
        !match(U, m_Xor(m_Value(R), m_Specific(SelI))))
      return false;
    // Matched: xor (select +++ ? 0 : T), R
    //          xor (select +++ ? T : 0), R
  } else
    return false;

  // The xor input value T is isolated into its own match so that it could
  // be checked against an induction variable containing a shifted bit
  // (todo).
  // For now, check against (Q << i).
  if (!match(T, m_Shl(m_Value(Q), m_Specific(CIV))) &&
      !match(T, m_Shl(m_ZExt(m_Value(Q)), m_ZExt(m_Specific(CIV)))))
    return false;
  // Matched: select +++ ? R : R ^ (Q << i)
  //          select +++ ? R ^ (Q << i) : R

  PV.X = X;
  PV.Q = Q;
  PV.R = R;
  PV.Left = true;
  return true;
}

bool PolynomialMultiplyRecognize::matchRightShift(SelectInst *SelI,
                                                  ParsedValues &PV) {
  // Match the following:
  //   select (X & 1) != 0 ? (R >> 1) ^ Q : (R >> 1)
  //   select (X & 1) == 0 ? (R >> 1) : (R >> 1) ^ Q
  // The condition may also check for equality with the masked value, i.e
  //   select (X & 1) == 1 ? (R >> 1) ^ Q : (R >> 1)
  //   select (X & 1) != 1 ? (R >> 1) : (R >> 1) ^ Q

  Value *CondV = SelI->getCondition();
  Value *TrueV = SelI->getTrueValue();
  Value *FalseV = SelI->getFalseValue();

  using namespace PatternMatch;

  Value *C = nullptr;
  // TODO: changed type CmpInst::Predicate P;
  CmpPredicate P;
  bool TrueIfZero;

  if (match(CondV, m_ICmp(P, m_Value(C), m_Zero())) ||
      match(CondV, m_ICmp(P, m_Zero(), m_Value(C)))) {
    if (P != CmpInst::ICMP_EQ && P != CmpInst::ICMP_NE)
      return false;
    // Matched: select C == 0 ? ... : ...
    //          select C != 0 ? ... : ...
    TrueIfZero = (P == CmpInst::ICMP_EQ);
  } else if (match(CondV, m_ICmp(P, m_Value(C), m_One())) ||
             match(CondV, m_ICmp(P, m_One(), m_Value(C)))) {
    if (P != CmpInst::ICMP_EQ && P != CmpInst::ICMP_NE)
      return false;
    // Matched: select C == 1 ? ... : ...
    //          select C != 1 ? ... : ...
    TrueIfZero = (P == CmpInst::ICMP_NE);
  } else
    return false;

  Value *X = nullptr;
  if (!match(C, m_And(m_Value(X), m_One())) &&
      !match(C, m_And(m_One(), m_Value(X))))
    return false;
  // Matched: select (X & 1) == +++ ? ... : ...
  //          select (X & 1) != +++ ? ... : ...

  Value *R = nullptr, *Q = nullptr;
  if (TrueIfZero) {
    // The select's condition is true if the tested bit is 0.
    // TrueV must be the shift, FalseV must be the xor.
    if (!match(TrueV, m_LShr(m_Value(R), m_One())))
      return false;
    // Matched: select +++ ? (R >> 1) : ...
    if (!match(FalseV, m_Xor(m_Specific(TrueV), m_Value(Q))) &&
        !match(FalseV, m_Xor(m_Value(Q), m_Specific(TrueV))))
      return false;
    // Matched: select +++ ? (R >> 1) : (R >> 1) ^ Q
    // with commuting ^.
  } else {
    // The select's condition is true if the tested bit is 1.
    // TrueV must be the xor, FalseV must be the shift.
    if (!match(FalseV, m_LShr(m_Value(R), m_One())))
      return false;
    // Matched: select +++ ? ... : (R >> 1)
    if (!match(TrueV, m_Xor(m_Specific(FalseV), m_Value(Q))) &&
        !match(TrueV, m_Xor(m_Value(Q), m_Specific(FalseV))))
      return false;
    // Matched: select +++ ? (R >> 1) ^ Q : (R >> 1)
    // with commuting ^.
  }

  PV.X = X;
  PV.Q = Q;
  PV.R = R;
  PV.Left = false;
  return true;
}

bool PolynomialMultiplyRecognize::scanSelect(SelectInst *SelI,
                                             BasicBlock *LoopB,
                                             BasicBlock *PrehB, Value *CIV,
                                             ParsedValues &PV, bool PreScan) {
  using namespace PatternMatch;

  // The basic pattern for R = P.Q is:
  // for i = 0..31
  //   R = phi (0, R')
  //   if (P & (1 << i))        ; test-bit(P, i)
  //     R' = R ^ (Q << i)
  //
  // Similarly, the basic pattern for R = (P/Q).Q - P
  // for i = 0..31
  //   R = phi(P, R')
  //   if (R & (1 << i))
  //     R' = R ^ (Q << i)

  // There exist idioms, where instead of Q being shifted left, P is shifted
  // right. This produces a result that is shifted right by 32 bits (the
  // non-shifted result is 64-bit).
  //
  // For R = P.Q, this would be:
  // for i = 0..31
  //   R = phi (0, R')
  //   if ((P >> i) & 1)
  //     R' = (R >> 1) ^ Q      ; R is cycled through the loop, so it must
  //   else                     ; be shifted by 1, not i.
  //     R' = R >> 1
  //
  // And for the inverse:
  // for i = 0..31
  //   R = phi (P, R')
  //   if (R & 1)
  //     R' = (R >> 1) ^ Q
  //   else
  //     R' = R >> 1

  // The left-shifting idioms share the same pattern:
  //   select (X & (1 << i)) ? R ^ (Q << i) : R
  // Similarly for right-shifting idioms:
  //   select (X & 1) ? (R >> 1) ^ Q

  if (matchLeftShift(SelI, CIV, PV)) {
    // If this is a pre-scan, getting this far is sufficient.
    if (PreScan)
      return true;

    // Need to make sure that the SelI goes back into R.
    auto *RPhi = dyn_cast<PHINode>(PV.R);
    if (!RPhi)
      return false;
    if (SelI != RPhi->getIncomingValueForBlock(LoopB))
      return false;
    PV.Res = SelI;

    // If X is loop invariant, it must be the input polynomial, and the
    // idiom is the basic polynomial multiply.
    if (CurLoop->isLoopInvariant(PV.X)) {
      PV.P = PV.X;
      PV.Inv = false;
    } else {
      // X is not loop invariant. If X == R, this is the inverse pmpy.
      // Otherwise, check for an xor with an invariant value. If the
      // variable argument to the xor is R, then this is still a valid
      // inverse pmpy.
      PV.Inv = true;
      if (PV.X != PV.R) {
        Value *Var = nullptr, *Inv = nullptr, *X1 = nullptr, *X2 = nullptr;
        if (!match(PV.X, m_Xor(m_Value(X1), m_Value(X2))))
          return false;
        auto *I1 = dyn_cast<Instruction>(X1);
        auto *I2 = dyn_cast<Instruction>(X2);
        if (!I1 || I1->getParent() != LoopB) {
          Var = X2;
          Inv = X1;
        } else if (!I2 || I2->getParent() != LoopB) {
          Var = X1;
          Inv = X2;
        } else
          return false;
        if (Var != PV.R)
          return false;
        PV.M = Inv;
      }
      // The input polynomial P still needs to be determined. It will be
      // the entry value of R.
      Value *EntryP = RPhi->getIncomingValueForBlock(PrehB);
      PV.P = EntryP;
    }

    return true;
  }

  if (matchRightShift(SelI, PV)) {
    // If this is an inverse pattern, the Q polynomial must be known at
    // compile time.
    if (PV.Inv && !isa<ConstantInt>(PV.Q))
      return false;
    if (PreScan)
      return true;
    // There is no exact matching of right-shift pmpy.
    return false;
  }

  return false;
}

bool PolynomialMultiplyRecognize::isPromotableTo(Value *Val,
                                                 IntegerType *DestTy) {
  IntegerType *T = dyn_cast<IntegerType>(Val->getType());
  if (!T || T->getBitWidth() > DestTy->getBitWidth())
    return false;
  if (T->getBitWidth() == DestTy->getBitWidth())
    return true;
  // Non-instructions are promotable. The reason why an instruction may not
  // be promotable is that it may produce a different result if its operands
  // and the result are promoted, for example, it may produce more non-zero
  // bits. While it would still be possible to represent the proper result
  // in a wider type, it may require adding additional instructions (which
  // we don't want to do).
  Instruction *In = dyn_cast<Instruction>(Val);
  if (!In)
    return true;
  // The bitwidth of the source type is smaller than the destination.
  // Check if the individual operation can be promoted.
  switch (In->getOpcode()) {
  case Instruction::PHI:
  case Instruction::ZExt:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::LShr: // Shift right is ok.
  case Instruction::Select:
  case Instruction::Trunc:
    return true;
  case Instruction::ICmp:
    if (CmpInst *CI = cast<CmpInst>(In))
      return CI->isEquality() || CI->isUnsigned();
    llvm_unreachable("Cast failed unexpectedly");
  case Instruction::Add:
    return In->hasNoSignedWrap() && In->hasNoUnsignedWrap();
  }
  return false;
}

void PolynomialMultiplyRecognize::promoteTo(Instruction *In,
                                            IntegerType *DestTy,
                                            BasicBlock *LoopB) {
  Type *OrigTy = In->getType();
  assert(!OrigTy->isVoidTy() && "Invalid instruction to promote");

  // Leave boolean values alone.
  if (!In->getType()->isIntegerTy(1))
    In->mutateType(DestTy);
  unsigned DestBW = DestTy->getBitWidth();

  // Handle PHIs.
  if (PHINode *P = dyn_cast<PHINode>(In)) {
    unsigned N = P->getNumIncomingValues();
    for (unsigned i = 0; i != N; ++i) {
      BasicBlock *InB = P->getIncomingBlock(i);
      if (InB == LoopB)
        continue;
      Value *InV = P->getIncomingValue(i);
      IntegerType *Ty = cast<IntegerType>(InV->getType());
      // Do not promote values in PHI nodes of type i1.
      if (Ty != P->getType()) {
        // If the value type does not match the PHI type, the PHI type
        // must have been promoted.
        assert(Ty->getBitWidth() < DestBW);
        InV = IRBuilder<>(InB->getTerminator()).CreateZExt(InV, DestTy);
        P->setIncomingValue(i, InV);
      }
    }
  } else if (ZExtInst *Z = dyn_cast<ZExtInst>(In)) {
    Value *Op = Z->getOperand(0);
    if (Op->getType() == Z->getType())
      Z->replaceAllUsesWith(Op);
    Z->eraseFromParent();
    return;
  }
  if (TruncInst *T = dyn_cast<TruncInst>(In)) {
    IntegerType *TruncTy = cast<IntegerType>(OrigTy);
    Value *Mask = ConstantInt::get(DestTy, (1u << TruncTy->getBitWidth()) - 1);
    Value *And = IRBuilder<>(In).CreateAnd(T->getOperand(0), Mask);
    T->replaceAllUsesWith(And);
    T->eraseFromParent();
    return;
  }

  // Promote immediates.
  for (unsigned i = 0, n = In->getNumOperands(); i != n; ++i) {
    if (ConstantInt *CI = dyn_cast<ConstantInt>(In->getOperand(i)))
      if (CI->getIntegerType()->getBitWidth() < DestBW)
        In->setOperand(i, ConstantInt::get(DestTy, CI->getZExtValue()));
  }
}

bool PolynomialMultiplyRecognize::promoteTypes(BasicBlock *LoopB,
                                               BasicBlock *ExitB) {
  assert(LoopB);
  // Skip loops where the exit block has more than one predecessor. The values
  // coming from the loop block will be promoted to another type, and so the
  // values coming into the exit block from other predecessors would also have
  // to be promoted.
  if (!ExitB || (ExitB->getSinglePredecessor() != LoopB))
    return false;
  IntegerType *DestTy = getCLMULType();
  // Check if the exit values have types that are no wider than the type
  // that we want to promote to.
  unsigned DestBW = DestTy->getBitWidth();
  for (PHINode &P : ExitB->phis()) {
    if (P.getNumIncomingValues() != 1)
      return false;
    assert(P.getIncomingBlock(0) == LoopB);
    IntegerType *T = dyn_cast<IntegerType>(P.getType());
    if (!T || T->getBitWidth() > DestBW)
      return false;
  }

  // Check all instructions in the loop.
  for (Instruction &In : *LoopB)
    if (!In.isTerminator() && !isPromotableTo(&In, DestTy))
      return false;

  // Perform the promotion.
  std::vector<Instruction *> LoopIns;
  std::transform(LoopB->begin(), LoopB->end(), std::back_inserter(LoopIns),
                 [](Instruction &In) { return &In; });
  for (Instruction *In : LoopIns)
    if (!In->isTerminator())
      promoteTo(In, DestTy, LoopB);

  // Fix up the PHI nodes in the exit block.
  Instruction *EndI = &*ExitB->getFirstNonPHIIt();
  BasicBlock::iterator End = EndI ? EndI->getIterator() : ExitB->end();
  for (auto I = ExitB->begin(); I != End; ++I) {
    PHINode *P = dyn_cast<PHINode>(I);
    if (!P)
      break;
    Type *Ty0 = P->getIncomingValue(0)->getType();
    Type *PTy = P->getType();
    if (PTy != Ty0) {
      assert(Ty0 == DestTy);
      // In order to create the trunc, P must have the promoted type.
      P->mutateType(Ty0);
      Value *T = IRBuilder<>(ExitB, End).CreateTrunc(P, PTy);
      // In order for the RAUW to work, the types of P and T must match.
      P->mutateType(PTy);
      P->replaceAllUsesWith(T);
      // Final update of the P's type.
      P->mutateType(Ty0);
      cast<Instruction>(T)->setOperand(0, P);
    }
  }

  return true;
}

bool PolynomialMultiplyRecognize::findCycle(Value *Out, Value *In,
                                            ValueSeq &Cycle) {
  // Out = ..., In, ...
  if (Out == In)
    return true;

  auto *BB = cast<Instruction>(Out)->getParent();
  bool HadPhi = false;

  for (auto *U : Out->users()) {
    auto *I = dyn_cast<Instruction>(&*U);
    if (I == nullptr || I->getParent() != BB)
      continue;
    // Make sure that there are no multi-iteration cycles, e.g.
    //   p1 = phi(p2)
    //   p2 = phi(p1)
    // The cycle p1->p2->p1 would span two loop iterations.
    // Check that there is only one phi in the cycle.
    bool IsPhi = isa<PHINode>(I);
    if (IsPhi && HadPhi)
      return false;
    HadPhi |= IsPhi;
    if (!Cycle.insert(I))
      return false;
    if (findCycle(I, In, Cycle))
      break;
    Cycle.remove(I);
  }
  return !Cycle.empty();
}

void PolynomialMultiplyRecognize::classifyCycle(Instruction *DivI,
                                                ValueSeq &Cycle,
                                                ValueSeq &Early,
                                                ValueSeq &Late) {
  // All the values in the cycle that are between the phi node and the
  // divider instruction will be classified as "early", all other values
  // will be "late".

  bool IsE = true;
  unsigned I, N = Cycle.size();
  for (I = 0; I < N; ++I) {
    Value *V = Cycle[I];
    if (DivI == V)
      IsE = false;
    else if (!isa<PHINode>(V))
      continue;
    // Stop if found either.
    break;
  }
  // "I" is the index of either DivI or the phi node, whichever was first.
  // "E" is "false" or "true" respectively.
  ValueSeq &First = !IsE ? Early : Late;
  for (unsigned J = 0; J < I; ++J)
    First.insert(Cycle[J]);

  ValueSeq &Second = IsE ? Early : Late;
  Second.insert(Cycle[I]);
  for (++I; I < N; ++I) {
    Value *V = Cycle[I];
    if (DivI == V || isa<PHINode>(V))
      break;
    Second.insert(V);
  }

  for (; I < N; ++I)
    First.insert(Cycle[I]);
}

bool PolynomialMultiplyRecognize::classifyInst(Instruction *UseI,
                                               ValueSeq &Early,
                                               ValueSeq &Late) {
  // Select is an exception, since the condition value does not have to be
  // classified in the same way as the true/false values. The true/false
  // values do have to be both early or both late.
  if (UseI->getOpcode() == Instruction::Select) {
    Value *TV = UseI->getOperand(1), *FV = UseI->getOperand(2);
    if (Early.count(TV) || Early.count(FV)) {
      if (Late.count(TV) || Late.count(FV))
        return false;
      Early.insert(UseI);
    } else if (Late.count(TV) || Late.count(FV)) {
      if (Early.count(TV) || Early.count(FV))
        return false;
      Late.insert(UseI);
    }
    return true;
  }

  // Not sure what would be the example of this, but the code below relies
  // on having at least one operand.
  if (UseI->getNumOperands() == 0)
    return true;

  bool AE = true, AL = true;
  for (auto &I : UseI->operands()) {
    if (Early.count(&*I))
      AL = false;
    else if (Late.count(&*I))
      AE = false;
  }
  // If the operands appear "all early" and "all late" at the same time,
  // then it means that none of them are actually classified as either.
  // This is harmless.
  if (AE && AL)
    return true;
  // Conversely, if they are neither "all early" nor "all late", then
  // we have a mixture of early and late operands that is not a known
  // exception.
  if (!AE && !AL)
    return false;

  // Check that we have covered the two special cases.
  assert(AE != AL);

  if (AE)
    Early.insert(UseI);
  else
    Late.insert(UseI);
  return true;
}

bool PolynomialMultiplyRecognize::commutesWithShift(Instruction *I) {
  switch (I->getOpcode()) {
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::LShr:
  case Instruction::Shl:
  case Instruction::Select:
  case Instruction::ICmp:
  case Instruction::PHI:
    break;
  default:
    return false;
  }
  return true;
}

bool PolynomialMultiplyRecognize::highBitsAreZero(Value *V,
                                                  unsigned IterCount) {
  auto *T = dyn_cast<IntegerType>(V->getType());
  if (!T)
    return false;

  KnownBits Known(T->getBitWidth());
  computeKnownBits(V, Known, DL);
  return Known.countMinLeadingZeros() >= IterCount;
}

bool PolynomialMultiplyRecognize::keepsHighBitsZero(Value *V,
                                                    unsigned IterCount) {
  // Assume that all inputs to the value have the high bits zero.
  // Check if the value itself preserves the zeros in the high bits.
  if (auto *C = dyn_cast<ConstantInt>(V))
    return C->getValue().countLeadingZeros() >= IterCount;

  if (auto *I = dyn_cast<Instruction>(V)) {
    switch (I->getOpcode()) {
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::LShr:
    case Instruction::Select:
    case Instruction::ICmp:
    case Instruction::PHI:
    case Instruction::ZExt:
      return true;
    }
  }

  return false;
}

bool PolynomialMultiplyRecognize::isOperandShifted(Instruction *I, Value *Op) {
  unsigned Opc = I->getOpcode();
  if (Opc == Instruction::Shl || Opc == Instruction::LShr)
    return Op != I->getOperand(1);
  return true;
}

bool PolynomialMultiplyRecognize::convertShiftsToLeft(BasicBlock *LoopB,
                                                      BasicBlock *ExitB,
                                                      unsigned IterCount,
                                                      ParsedValues &PV) {
  Value *CIV = getCountIV(LoopB);
  if (CIV == nullptr)
    return false;
  auto *CIVTy = dyn_cast<IntegerType>(CIV->getType());
  if (CIVTy == nullptr)
    return false;

  ValueSeq RShifts;
  ValueSeq Early, Late, Cycled;

  // Find all value cycles that contain logical right shifts by 1.
  for (Instruction &I : *LoopB) {
    using namespace PatternMatch;

    Value *V = nullptr;
    if (!match(&I, m_LShr(m_Value(V), m_One())))
      continue;
    ValueSeq C;
    if (!findCycle(&I, V, C))
      continue;

    // Found a cycle.
    C.insert(&I);
    classifyCycle(&I, C, Early, Late);
    Cycled.insert(C.begin(), C.end());
    RShifts.insert(&I);
  }

  // Find the set of all values affected by the shift cycles, i.e. all
  // cycled values, and (recursively) all their users.
  ValueSeq Users(Cycled.begin(), Cycled.end());
  for (unsigned i = 0; i < Users.size(); ++i) {
    Value *V = Users[i];
    if (!isa<IntegerType>(V->getType()))
      return false;
    auto *R = cast<Instruction>(V);
    // If the instruction does not commute with shifts, the loop cannot
    // be unshifted.
    if (!commutesWithShift(R))
      return false;
    for (User *U : R->users()) {
      auto *T = cast<Instruction>(U);
      // Skip users from outside of the loop. They will be handled later.
      // Also, skip the right-shifts and phi nodes, since they mix early
      // and late values.
      if (T->getParent() != LoopB || RShifts.count(T) || isa<PHINode>(T))
        continue;

      Users.insert(T);
      if (!classifyInst(T, Early, Late))
        return false;
    }
  }

  if (Users.empty())
    return false;

  // Verify that high bits remain zero.
  ValueSeq Internal(Users.begin(), Users.end());
  ValueSeq Inputs;
  for (unsigned i = 0; i < Internal.size(); ++i) {
    auto *R = dyn_cast<Instruction>(Internal[i]);
    if (!R)
      continue;
    for (Value *Op : R->operands()) {
      auto *T = dyn_cast<Instruction>(Op);
      if (T && T->getParent() != LoopB)
        Inputs.insert(Op);
      else
        Internal.insert(Op);
    }
  }
  for (Value *V : Inputs)
    if (!highBitsAreZero(V, IterCount))
      return false;
  for (Value *V : Internal)
    if (!keepsHighBitsZero(V, IterCount))
      return false;

  // Finally, the work can be done. Unshift each user.
  IRBuilder<> IRB(LoopB);
  std::map<Value *, Value *> ShiftMap;

  using CastMapType = std::map<std::pair<Value *, Type *>, Value *>;

  CastMapType CastMap;

  auto upcast = [](CastMapType &CM, IRBuilder<> &IRB, Value *V,
                   IntegerType *Ty) -> Value * {
    auto H = CM.find(std::make_pair(V, Ty));
    if (H != CM.end())
      return H->second;
    Value *CV = IRB.CreateIntCast(V, Ty, false);
    CM.insert(std::make_pair(std::make_pair(V, Ty), CV));
    return CV;
  };

  for (auto I = LoopB->begin(), E = LoopB->end(); I != E; ++I) {
    using namespace PatternMatch;

    if (isa<PHINode>(I) || !Users.count(&*I))
      continue;

    // Match lshr x, 1.
    Value *V = nullptr;
    if (match(&*I, m_LShr(m_Value(V), m_One()))) {
      replaceAllUsesOfWithIn(&*I, V, LoopB);
      continue;
    }
    // For each non-cycled operand, replace it with the corresponding
    // value shifted left.
    for (auto &J : I->operands()) {
      Value *Op = J.get();
      if (!isOperandShifted(&*I, Op))
        continue;
      if (Users.count(Op))
        continue;
      // Skip shifting zeros.
      if (isa<ConstantInt>(Op) && cast<ConstantInt>(Op)->isZero())
        continue;
      // Check if we have already generated a shift for this value.
      auto F = ShiftMap.find(Op);
      Value *W = (F != ShiftMap.end()) ? F->second : nullptr;
      if (W == nullptr) {
        IRB.SetInsertPoint(&*I);
        // First, the shift amount will be CIV or CIV+1, depending on
        // whether the value is early or late. Instead of creating CIV+1,
        // do a single shift of the value.
        Value *ShAmt = CIV, *ShVal = Op;
        auto *VTy = cast<IntegerType>(ShVal->getType());
        auto *ATy = cast<IntegerType>(ShAmt->getType());
        if (Late.count(&*I))
          ShVal = IRB.CreateShl(Op, ConstantInt::get(VTy, 1));
        // Second, the types of the shifted value and the shift amount
        // must match.
        if (VTy != ATy) {
          if (VTy->getBitWidth() < ATy->getBitWidth())
            ShVal = upcast(CastMap, IRB, ShVal, ATy);
          else
            ShAmt = upcast(CastMap, IRB, ShAmt, VTy);
        }
        // Ready to generate the shift and memoize it.
        W = IRB.CreateShl(ShVal, ShAmt);
        ShiftMap.insert(std::make_pair(Op, W));
      }
      I->replaceUsesOfWith(Op, W);
    }
  }

  // Update the users outside of the loop to account for having left
  // shifts. They would normally be shifted right in the loop, so shift
  // them right after the loop exit.
  // Take advantage of the loop-closed SSA form, which has all the post-
  // loop values in phi nodes.
  // TODO: extend this to multiple PHIs.
  PHINode *PN = &*ExitB->phis().begin();
  assert(std::distance(ExitB->phis().begin(), ExitB->phis().end()) &&
         "Can't handle this crc pattern yet!");
  IRB.SetInsertPoint(ExitB, ExitB->getFirstInsertionPt());
  Value *U = PN->getIncomingValueForBlock(LoopB);
  if (Users.count(U)) {
    Value *S = IRB.CreateLShr(PN, ConstantInt::get(PN->getType(), IterCount));
    PN->replaceAllUsesWith(S);
    // The above RAUW will create
    //   S = lshr S, IterCount
    // so we need to fix it back into
    //   S = lshr PN, IterCount
    cast<User>(S)->replaceUsesOfWith(S, PN);
    PV.ResAfterConvertShiftsToLeft = dyn_cast<Instruction>(S);
  }

  return true;
}

void PolynomialMultiplyRecognize::cleanupLoopBody(BasicBlock *LoopB) {
  for (auto &I : *LoopB)
    if (Value *SV = simplifyInstruction(&I, {DL, &TLI, &DT}))
      I.replaceAllUsesWith(SV);

  for (Instruction &I : llvm::make_early_inc_range(*LoopB))
    RecursivelyDeleteTriviallyDeadInstructions(&I, &TLI);
}

Value *PolynomialMultiplyRecognize::generate(BasicBlock::iterator At,
                                             ParsedValues &PV) {
  if (!PV.Inv)
    return nullptr;
  Type *CLMULType = getCLMULType();
  unsigned XLen = CLMULType->getPrimitiveSizeInBits();

  Value *C = PV.P;
  KnownBits KnownC(XLen);
  computeKnownBits(C, KnownC, DL);
  unsigned S = XLen - KnownC.countMinLeadingZeros();
  if (PV.M) {
    KnownBits KnownM(XLen);
    computeKnownBits(PV.M, KnownM, DL);
    if (XLen - KnownM.countMinLeadingZeros() != PV.IterCount)
      return nullptr;
  }

  Value *G = PV.Q;
  IntegerType *GType = dyn_cast<IntegerType>(G->getType());
  if (!GType)
    return nullptr;
  ConstantInt *GI = dyn_cast<ConstantInt>(G);
  assert(GI && "Can't handle this crc pattern yet!");
  unsigned T = GI->getValue().getActiveBits() - 1;

  if (S + T > XLen)
    return nullptr;

  IRBuilder<> B(&*At);
  Module *M = At->getParent()->getParent()->getParent();
  Function *PDIVSTEPSRIntrinsic = Intrinsic::getOrInsertDeclaration(
      M, Intrinsic::riscv_pdivstepsr, CLMULType);
  if (PV.M != nullptr)
    C = B.CreateXor(C, PV.M);
  Value *R = B.CreateCall(PDIVSTEPSRIntrinsic,
                          {C, G, ConstantInt::get(CLMULType, PV.IterCount)});

  return R;
}

static bool hasZeroSignBit(const Value *V) {
  if (const auto *CI = dyn_cast<const ConstantInt>(V))
    return (CI->getIntegerType()->getSignBit() & CI->getSExtValue()) == 0;
  const Instruction *I = dyn_cast<const Instruction>(V);
  if (!I)
    return false;
  switch (I->getOpcode()) {
  case Instruction::LShr:
    if (const auto SI = dyn_cast<const ConstantInt>(I->getOperand(1)))
      return SI->getZExtValue() > 0;
    return false;
  case Instruction::Or:
  case Instruction::Xor:
    return hasZeroSignBit(I->getOperand(0)) && hasZeroSignBit(I->getOperand(1));
  case Instruction::And:
    return hasZeroSignBit(I->getOperand(0)) || hasZeroSignBit(I->getOperand(1));
  }
  return false;
}

void PolynomialMultiplyRecognize::setupPreSimplifier(Simplifier &S) {
  S.addRule("sink-zext",
            // Sink zext past bitwise operations.
            [](Instruction *I, LLVMContext &Ctx) -> Value * {
              if (I->getOpcode() != Instruction::ZExt)
                return nullptr;
              Instruction *T = dyn_cast<Instruction>(I->getOperand(0));
              if (!T)
                return nullptr;
              switch (T->getOpcode()) {
              case Instruction::And:
              case Instruction::Or:
              case Instruction::Xor:
                break;
              default:
                return nullptr;
              }
              IRBuilder<> B(Ctx);
              return B.CreateBinOp(
                  cast<BinaryOperator>(T)->getOpcode(),
                  B.CreateZExt(T->getOperand(0), I->getType()),
                  B.CreateZExt(T->getOperand(1), I->getType()));
            });
  S.addRule("xor/and -> and/xor",
            // (xor (and x a) (and y a)) -> (and (xor x y) a)
            [](Instruction *I, LLVMContext &Ctx) -> Value * {
              if (I->getOpcode() != Instruction::Xor)
                return nullptr;
              Instruction *And0 = dyn_cast<Instruction>(I->getOperand(0));
              Instruction *And1 = dyn_cast<Instruction>(I->getOperand(1));
              if (!And0 || !And1)
                return nullptr;
              if (And0->getOpcode() != Instruction::And ||
                  And1->getOpcode() != Instruction::And)
                return nullptr;
              if (And0->getOperand(1) != And1->getOperand(1))
                return nullptr;
              IRBuilder<> B(Ctx);
              return B.CreateAnd(
                  B.CreateXor(And0->getOperand(0), And1->getOperand(0)),
                  And0->getOperand(1));
            });
  S.addRule(
      "sink binop into select",
      // (Op (select c x y) z) -> (select c (Op x z) (Op y z))
      // (Op x (select c y z)) -> (select c (Op x y) (Op x z))
      [](Instruction *I, LLVMContext &Ctx) -> Value * {
        BinaryOperator *BO = dyn_cast<BinaryOperator>(I);
        if (!BO)
          return nullptr;
        Instruction::BinaryOps Op = BO->getOpcode();
        if (SelectInst *Sel = dyn_cast<SelectInst>(BO->getOperand(0))) {
          IRBuilder<> B(Ctx);
          Value *X = Sel->getTrueValue(), *Y = Sel->getFalseValue();
          Value *Z = BO->getOperand(1);
          return B.CreateSelect(Sel->getCondition(), B.CreateBinOp(Op, X, Z),
                                B.CreateBinOp(Op, Y, Z));
        }
        if (SelectInst *Sel = dyn_cast<SelectInst>(BO->getOperand(1))) {
          IRBuilder<> B(Ctx);
          Value *X = BO->getOperand(0);
          Value *Y = Sel->getTrueValue(), *Z = Sel->getFalseValue();
          return B.CreateSelect(Sel->getCondition(), B.CreateBinOp(Op, X, Y),
                                B.CreateBinOp(Op, X, Z));
        }
        return nullptr;
      });
  S.addRule(
      "fold select-select",
      // (select c (select c x y) z) -> (select c x z)
      // (select c x (select c y z)) -> (select c x z)
      [](Instruction *I, LLVMContext &Ctx) -> Value * {
        SelectInst *Sel = dyn_cast<SelectInst>(I);
        if (!Sel)
          return nullptr;
        IRBuilder<> B(Ctx);
        Value *C = Sel->getCondition();
        if (SelectInst *Sel0 = dyn_cast<SelectInst>(Sel->getTrueValue())) {
          if (Sel0->getCondition() == C)
            return B.CreateSelect(C, Sel0->getTrueValue(),
                                  Sel->getFalseValue());
        }
        if (SelectInst *Sel1 = dyn_cast<SelectInst>(Sel->getFalseValue())) {
          if (Sel1->getCondition() == C)
            return B.CreateSelect(C, Sel->getTrueValue(),
                                  Sel1->getFalseValue());
        }
        return nullptr;
      });
  S.addRule("or-signbit -> xor-signbit",
            // (or (lshr x 1) 0x800.0) -> (xor (lshr x 1) 0x800.0)
            [](Instruction *I, LLVMContext &Ctx) -> Value * {
              if (I->getOpcode() != Instruction::Or)
                return nullptr;
              ConstantInt *Msb = dyn_cast<ConstantInt>(I->getOperand(1));
              if (!Msb ||
                  Msb->getZExtValue() != Msb->getIntegerType()->getSignBit())
                return nullptr;
              if (!hasZeroSignBit(I->getOperand(0)))
                return nullptr;
              return IRBuilder<>(Ctx).CreateXor(I->getOperand(0), Msb);
            });
  S.addRule("sink lshr into binop",
            // (lshr (BitOp x y) c) -> (BitOp (lshr x c) (lshr y c))
            [](Instruction *I, LLVMContext &Ctx) -> Value * {
              if (I->getOpcode() != Instruction::LShr)
                return nullptr;
              BinaryOperator *BitOp =
                  dyn_cast<BinaryOperator>(I->getOperand(0));
              if (!BitOp)
                return nullptr;
              switch (BitOp->getOpcode()) {
              case Instruction::And:
              case Instruction::Or:
              case Instruction::Xor:
                break;
              default:
                return nullptr;
              }
              IRBuilder<> B(Ctx);
              Value *S = I->getOperand(1);
              return B.CreateBinOp(BitOp->getOpcode(),
                                   B.CreateLShr(BitOp->getOperand(0), S),
                                   B.CreateLShr(BitOp->getOperand(1), S));
            });
  S.addRule("expose bitop-const",
            // (BitOp1 (BitOp2 x a) b) -> (BitOp2 x (BitOp1 a b))
            [](Instruction *I, LLVMContext &Ctx) -> Value * {
              auto IsBitOp = [](unsigned Op) -> bool {
                switch (Op) {
                case Instruction::And:
                case Instruction::Or:
                case Instruction::Xor:
                  return true;
                }
                return false;
              };
              BinaryOperator *BitOp1 = dyn_cast<BinaryOperator>(I);
              if (!BitOp1 || !IsBitOp(BitOp1->getOpcode()))
                return nullptr;
              BinaryOperator *BitOp2 =
                  dyn_cast<BinaryOperator>(BitOp1->getOperand(0));
              if (!BitOp2 || !IsBitOp(BitOp2->getOpcode()))
                return nullptr;
              ConstantInt *CA = dyn_cast<ConstantInt>(BitOp2->getOperand(1));
              ConstantInt *CB = dyn_cast<ConstantInt>(BitOp1->getOperand(1));
              if (!CA || !CB)
                return nullptr;
              IRBuilder<> B(Ctx);
              Value *X = BitOp2->getOperand(0);
              return B.CreateBinOp(BitOp2->getOpcode(), X,
                                   B.CreateBinOp(BitOp1->getOpcode(), CA, CB));
            });
}

void PolynomialMultiplyRecognize::setupPostSimplifier(Simplifier &S) {
  S.addRule("(and (xor (and x a) y) b) -> (and (xor x y) b), if b == b&a",
            [](Instruction *I, LLVMContext &Ctx) -> Value * {
              if (I->getOpcode() != Instruction::And)
                return nullptr;
              Instruction *Xor = dyn_cast<Instruction>(I->getOperand(0));
              ConstantInt *C0 = dyn_cast<ConstantInt>(I->getOperand(1));
              if (!Xor || !C0)
                return nullptr;
              if (Xor->getOpcode() != Instruction::Xor)
                return nullptr;
              Instruction *And0 = dyn_cast<Instruction>(Xor->getOperand(0));
              Instruction *And1 = dyn_cast<Instruction>(Xor->getOperand(1));
              // Pick the first non-null and.
              if (!And0 || And0->getOpcode() != Instruction::And)
                std::swap(And0, And1);
              ConstantInt *C1 = dyn_cast<ConstantInt>(And0->getOperand(1));
              if (!C1)
                return nullptr;
              uint32_t V0 = C0->getZExtValue();
              uint32_t V1 = C1->getZExtValue();
              if (V0 != (V0 & V1))
                return nullptr;
              IRBuilder<> B(Ctx);
              return B.CreateAnd(B.CreateXor(And0->getOperand(0), And1), C0);
            });
}

bool PolynomialMultiplyRecognize::recognize() {
  LLVM_DEBUG(dbgs() << "Starting PolynomialMultiplyRecognize on loop\n"
                    << *CurLoop << '\n');
  // Restrictions:
  // - The loop must consist of a single block.
  // - The iteration count must be known at compile-time.
  // - The loop must have an induction variable starting from 0, and
  //   incremented in each iteration of the loop.
  BasicBlock *LoopB = CurLoop->getHeader();
  LLVM_DEBUG(dbgs() << "Loop header:\n" << *LoopB);

  if (LoopB != CurLoop->getLoopLatch())
    return false;
  BasicBlock *ExitB = CurLoop->getExitBlock();
  if (ExitB == nullptr)
    return false;
  BasicBlock *EntryB = CurLoop->getLoopPreheader();
  if (EntryB == nullptr)
    return false;

  unsigned IterCount = 0;
  const SCEV *CT = SE.getBackedgeTakenCount(CurLoop);
  if (isa<SCEVCouldNotCompute>(CT))
    return false;
  if (auto *CV = dyn_cast<SCEVConstant>(CT))
    IterCount = CV->getValue()->getZExtValue() + 1;

  Value *CIV = getCountIV(LoopB);
  ParsedValues PV;
  Simplifier PreSimp;
  PV.IterCount = IterCount;
  LLVM_DEBUG(dbgs() << "Loop IV: " << *CIV << "\nIterCount: " << IterCount
                    << '\n');

  setupPreSimplifier(PreSimp);

  // Perform a preliminary scan of select instructions to see if any of them
  // looks like a generator of the polynomial multiply steps. Assume that a
  // loop can only contain a single transformable operation, so stop the
  // traversal after the first reasonable candidate was found.
  // XXX: Currently this approach can modify the loop before being 100% sure
  // that the transformation can be carried out.
  bool FoundPreScan = false;
  auto FeedsPHI = [LoopB](const Value *V) -> bool {
    for (const Value *U : V->users()) {
      if (const auto *P = dyn_cast<const PHINode>(U))
        if (P->getParent() == LoopB)
          return true;
    }
    return false;
  };
  for (Instruction &In : *LoopB) {
    SelectInst *SI = dyn_cast<SelectInst>(&In);
    if (!SI || !FeedsPHI(SI))
      continue;

    Simplifier::Context C(SI);
    Value *T = PreSimp.simplify(C);
    SelectInst *SelI = (T && isa<SelectInst>(T)) ? cast<SelectInst>(T) : SI;
    LLVM_DEBUG(dbgs() << "scanSelect(pre-scan): " << PE(C, SelI) << '\n');
    if (scanSelect(SelI, LoopB, EntryB, CIV, PV, true)) {
      FoundPreScan = true;
      if (SelI != SI) {
        Value *NewSel = C.materialize(LoopB, SI->getIterator());
        SI->replaceAllUsesWith(NewSel);
        RecursivelyDeleteTriviallyDeadInstructions(SI, &TLI);
      }
      break;
    }
  }

  if (!FoundPreScan) {
    LLVM_DEBUG(dbgs() << "Have not found candidates for pmpy\n");
    return false;
  }

  if (!PV.Left) {
    // The right shift version actually only returns the higher bits of
    // the result (each iteration discards the LSB). If we want to convert it
    // to a left-shifting loop, the working data type must be at least as
    // wide as the target's pmpy instruction.
    if (!promoteTypes(LoopB, ExitB))
      return false;
    // Run post-promotion simplifications.
    Simplifier PostSimp;
    setupPostSimplifier(PostSimp);
    for (Instruction &In : *LoopB) {
      SelectInst *SI = dyn_cast<SelectInst>(&In);
      if (!SI || !FeedsPHI(SI))
        continue;
      Simplifier::Context C(SI);
      Value *T = PostSimp.simplify(C);
      SelectInst *SelI = dyn_cast_or_null<SelectInst>(T);
      if (SelI != SI) {
        Value *NewSel = C.materialize(LoopB, SI->getIterator());
        SI->replaceAllUsesWith(NewSel);
        RecursivelyDeleteTriviallyDeadInstructions(SI, &TLI);
      }
      break;
    }

    if (!convertShiftsToLeft(LoopB, ExitB, IterCount, PV))
      return false;
    cleanupLoopBody(LoopB);
  }

  // Scan the loop again, find the generating select instruction.
  bool FoundScan = false;
  for (Instruction &In : *LoopB) {
    SelectInst *SelI = dyn_cast<SelectInst>(&In);
    if (!SelI)
      continue;
    LLVM_DEBUG(dbgs() << "scanSelect: " << *SelI << '\n');
    FoundScan = scanSelect(SelI, LoopB, EntryB, CIV, PV, false);
    if (FoundScan)
      break;
  }
  assert(FoundScan);

  LLVM_DEBUG({
    StringRef PP = (PV.M ? "(P+M)" : "P");
    if (!PV.Inv)
      dbgs() << "Found polynomial multiply idiom: R = " << PP << ".Q\n";
    else
      dbgs() << "Found crc idiom: R = (" << PP << "/Q).Q) + " << PP << "\n";
    dbgs() << "  Res:" << *PV.Res << "\n  P:" << *PV.P << "\n";
    if (PV.M)
      dbgs() << "  M:" << *PV.M << "\n";
    dbgs() << "  Q:" << *PV.Q << "\n";
    dbgs() << "  Iteration count:" << PV.IterCount << "\n";
  });

  BasicBlock::iterator At(EntryB->getTerminator());
  Value *PM = generate(At, PV);
  if (PM == nullptr)
    return false;

  if (PM->getType() != PV.Res->getType())
    PM = IRBuilder<>(&*At).CreateIntCast(PM, PV.Res->getType(), false);

  Instruction *ToReplace =
      PV.ResAfterConvertShiftsToLeft ? PV.ResAfterConvertShiftsToLeft : PV.Res;
  ToReplace->replaceAllUsesWith(PM);
  ToReplace->eraseFromParent();
  return true;
}

bool RISCVLoopIdiomRecognize::runOnCountableLoop(Loop *L) {
  PolynomialMultiplyRecognize PMR(L, *DL, *DT, *TLI, *SE);
  if (PMR.recognize())
    return true;
  return false;
}

bool RISCVLoopIdiomRecognize::run(Loop *L) {
  if (DisableLIR)
    return false;

  Function &F = *L->getHeader()->getParent();
  const Module &M = *F.getParent();
  if (Triple(M.getTargetTriple()).getArch() != Triple::riscv64)
    return false;
  if (F.getFnAttribute("target-features").getValueAsString().find("+zbkc") ==
      StringRef::npos)
    return false;

  // If the loop could not be converted to canonical form, it must have an
  // indirectbr in it, just give up.
  if (!L->getLoopPreheader())
    return false;

  // Disable loop idiom recognition if the function's name is a common idiom.
  StringRef Name = L->getHeader()->getParent()->getName();
  if (Name == "memset" || Name == "memcpy" || Name == "memmove")
    return false;

  DL = &L->getHeader()->getModule()->getDataLayout();

  HasMemcpy = TLI->has(LibFunc_memcpy);
  HasMemmove = TLI->has(LibFunc_memmove);

  if (SE->hasLoopInvariantBackedgeTakenCount(L))
    return runOnCountableLoop(L);
  return false;
}

bool RISCVLoopIdiomRecognizeLegacyPass::runOnLoop(Loop *L, LPPassManager &LPM) {
  if (skipLoop(L))
    return false;

  auto *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  auto *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto *LF = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto *TLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(
      *L->getHeader()->getParent());
  auto *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  return RISCVLoopIdiomRecognize(AA, DT, LF, TLI, SE).run(L);
}

Pass *llvm::createRISCVLoopIdiomPass() {
  return new RISCVLoopIdiomRecognizeLegacyPass();
}

PreservedAnalyses
RISCVLoopIdiomRecognitionPass::run(Loop &L, LoopAnalysisManager &AM,
                                   LoopStandardAnalysisResults &AR,
                                   LPMUpdater &U) {
  return RISCVLoopIdiomRecognize(&AR.AA, &AR.DT, &AR.LI, &AR.TLI, &AR.SE)
                 .run(&L)
             ? getLoopPassPreservedAnalyses()
             : PreservedAnalyses::all();
}
