//===-- GPUVMGlobalFill.cpp - move ops across blocks into empty slots -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A bundle is one iteration of the tick whatever it holds, and a block's last
// bundle often has empty op slots: a branch compares registers as its bundle
// found them, so after an op that writes one (a value just loaded or
// computed) the branch takes a bundle of its own. This pass moves ops up
// across block boundaries into such slots, after register allocation and the
// bundle scheduler, by the streamer's own packing (GPUVMBundleModel):
//
// - fill: an op near the top of a block S with one predecessor B moves into
//   B (before its terminators, or earlier where it fits), and runs on B's
//   other paths too. Its result may go to a register the function does not
//   use, the uses in S rewritten, with a `mv` back where the old register
//   lives on;
// - rotation: an op at the top of a loop's header moves to the bottom of its
//   latch and of the block that enters the loop, so that the next
//   iteration's first op shares the latch's branch bundle.
//
// One op or two together (the second found after the first has moved). An
// op placed before a `mv u, v` it would have read u after reads v instead.
// An op that runs where it did not before must be safe there: no store, no
// side effect, and a load only from the stack or a global, from a pointer the
// branch tests against 0 (the boot area is readable: a load of [0 + small]
// does not fault), or near an address accessed through the same register on
// every path to it. Each move is kept when it lowers the bundles run (block
// frequencies), counting what a needless op costs.
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMBundleModel.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <cmath>

using namespace llvm;

#define DEBUG_TYPE "gpuvm-global-fill"

static cl::opt<bool> GlobalFill("gpuvm-global-fill", cl::Hidden,
                                cl::init(true),
                                cl::desc("gpu_vm: move ops across blocks into "
                                         "empty bundle slots"));
static cl::opt<bool> Rotate("gpuvm-fill-rotate", cl::Hidden, cl::init(true),
                            cl::desc("gpu_vm: move a loop header's first ops "
                                     "into its latch's last bundle"));
static cl::opt<bool> Rename("gpuvm-fill-rename", cl::Hidden, cl::init(true),
                            cl::desc("gpu_vm: rename a moved op's result"));
static cl::opt<bool> Join("gpuvm-fill-join", cl::Hidden, cl::init(true),
                          cl::desc("gpu_vm: move the first ops of a block "
                                   "several others lead to into all of them"));
static cl::opt<bool> Pairs("gpuvm-fill-pairs", cl::Hidden, cl::init(true),
                           cl::desc("gpu_vm: move two ops together"));
static cl::opt<unsigned>
    SpecWindow("gpuvm-spec-window", cl::Hidden, cl::init(16),
               cl::desc("gpu_vm: a load may run where it did not when an "
                        "access through the same register is this many "
                        "bytes near on every path"));
static cl::opt<unsigned> NullWindow(
    "gpuvm-null-window", cl::Hidden, cl::init(0x10000),
    cl::desc("gpu_vm: a load from a pointer the branch tests against 0 may "
             "run when its offset is below this (the boot area)"));
static cl::opt<double>
    LoadWaste("gpuvm-fill-load-waste", cl::Hidden, cl::init(0.5),
              cl::desc("gpu_vm: what a load run for nothing costs, in "
                       "bundles"));
static cl::opt<double>
    OpWaste("gpuvm-fill-op-waste", cl::Hidden, cl::init(0.15),
            cl::desc("gpu_vm: what another op run for nothing costs, in "
                     "bundles"));
static cl::opt<double>
    LoopWeight("gpuvm-fill-loop-weight", cl::Hidden, cl::init(1.0),
               cl::desc("gpu_vm: a block's weight in a loop, a level, over "
                        "its static frequency"));
static cl::opt<unsigned>
    ScanOps("gpuvm-fill-scan", cl::Hidden, cl::init(6),
            cl::desc("gpu_vm: how deep into a block to look for an op to "
                     "move"));

namespace {

// A trial change, undone unless kept.
struct Trial {
  struct Moved {
    MachineInstr *MI;
    MachineBasicBlock *From;
    MachineInstr *Next; // its old successor in From (nullptr: the end)
  };
  SmallVector<Moved, 2> Moves;
  SmallVector<std::pair<MachineOperand *, Register>, 8> Renamed;
  SmallVector<MachineInstr *, 2> Added;

  void move(MachineInstr *MI, MachineBasicBlock &To,
            MachineBasicBlock::iterator At) {
    MachineBasicBlock *From = MI->getParent();
    auto N = std::next(MI->getIterator());
    Moves.push_back({MI, From, N == From->end() ? nullptr : &*N});
    To.splice(At, From, MI->getIterator());
  }
  void rename(MachineOperand &MO, Register R) {
    Renamed.push_back({&MO, MO.getReg()});
    MO.setReg(R);
  }
  SmallVector<std::pair<MachineOperand *, int64_t>, 2> Adjusted;
  void adjust(MachineOperand &MO, int64_t K) {
    Adjusted.push_back({&MO, MO.getImm()});
    MO.setImm(MO.getImm() + K);
  }
  void undo() {
    for (auto &[MO, V] : reverse(Adjusted))
      MO->setImm(V);
    for (MachineInstr *MI : reverse(Added))
      MI->eraseFromParent();
    for (auto &[MO, R] : reverse(Renamed))
      MO->setReg(R);
    for (Moved &M : reverse(Moves)) {
      MachineBasicBlock::iterator At =
          M.Next ? M.Next->getIterator() : M.From->end();
      M.From->splice(At, M.MI->getParent(), M.MI->getIterator());
    }
    clear();
  }
  void clear() {
    Moves.clear();
    Renamed.clear();
    Added.clear();
    Adjusted.clear();
  }
};

using Substs = SmallVector<std::pair<unsigned, Register>, 2>;

// A place for an op: before an instruction (nullptr: the block's end), its
// uses read from other registers there.
struct Place {
  MachineBasicBlock *Into = nullptr;
  MachineInstr *Before = nullptr;
  Substs Subst;
  int64_t Adj = 0; // added to X's immediate (an `addi base, base, k` passed)
  // the block's own last value of X's result register, used only by its
  // terminators, renamed out of the way
  MachineInstr *RenDef = nullptr;
  Register RenTo;
  SmallVector<MachineBasicBlock *, 4> RenChain; // RenDef's block .. Into
};

// One way to move an op.
struct Plan;
static SmallVector<Register, 4> planRegs(const Plan &P);
struct Plan {
  MachineInstr *X = nullptr;
  Place At;
  Register To;   // the new register of X's result, or none
  int MvAt = -1; // -1 no mv; 0 at the end of X's block; 1 at X's place
  SmallVector<Place, 2> Enter; // rotation: a copy in each block entering the loop
  double Waste = 0;
};

// The fresh registers a plan takes
static SmallVector<Register, 4> planRegs(const Plan &P) {
  SmallVector<Register, 4> R;
  for (Register X : {P.To, P.At.RenTo})
    if (X)
      R.push_back(X);
  for (const Place &E : P.Enter)
    if (E.RenTo)
      R.push_back(E.RenTo);
  return R;
}

class GPUVMGlobalFill : public MachineFunctionPass {
public:
  static char ID;
  GPUVMGlobalFill() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override {
    return "gpu_vm fill bundles across blocks";
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
    AU.addRequired<MachineBranchProbabilityInfoWrapperPass>();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  MachineFunction *MF = nullptr;
  const GPUVMInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const MachineRegisterInfo *MRI = nullptr;
  MachineBlockFrequencyInfo *MBFI = nullptr;
  const MachineBranchProbabilityInfo *MBPI = nullptr;
  MachineLoopInfo *MLI = nullptr;
  BitVector Used; // registers some operand names (renames go elsewhere)
  DenseMap<const MachineBasicBlock *, MachineBasicBlock *> Head;

  // The static estimate takes a loop with several exits for one of few
  // iterations: a block in a loop weighs LoopWeight times more a level.
  double freq(const MachineBasicBlock *B) const {
    return MBFI->getBlockFreqRelativeToEntryBlock(B) *
           std::pow((double)LoopWeight, (double)MLI->getLoopDepth(B));
  }
  double edgeFreq(const MachineBasicBlock *B,
                  const MachineBasicBlock *S) const {
    return freq(B) * MBPI->getEdgeProbability(B, S).getNumerator() /
           double(BranchProbability::getDenominator());
  }
  double chainCost(MachineBasicBlock *H) const;
  double cost(ArrayRef<MachineBasicBlock *> Blocks) const;

  bool liveIn(const MachineBasicBlock &MBB, Register R) const;
  bool liveInOthers(const MachineBasicBlock &B, const MachineBasicBlock *S,
                    Register R) const;
  bool movableOp(const MachineInstr &I) const;
  Register singleDef(const MachineInstr &I) const;
  bool speculable(const MachineInstr &X, const Place &P,
                  const MachineBasicBlock *To) const;
  bool nearOnPaths(Register Base, int64_t Off, MachineBasicBlock *MBB,
                   MachineBasicBlock::iterator I, int64_t Delta,
                   unsigned &Budget,
                   SmallPtrSetImpl<MachineBasicBlock *> &OnPath) const;
  bool nearAhead(Register Base, int64_t Off, MachineBasicBlock *O) const;
  bool nullTested(Register Base, int64_t Off, const MachineBasicBlock &B,
                  const MachineBasicBlock *To) const;
  bool hoistsInBlock(const MachineInstr &X) const;
  bool fitsAfter(const MachineInstr &X, MachineBasicBlock::iterator At,
                 MachineBasicBlock &B, Register Def, Substs &Subst,
                 int64_t &Adj, bool DefRenamed = false) const;
  void places(const MachineInstr &X, MachineBasicBlock &B, Register Def,
              SmallVectorImpl<Place> &Out, Register Avoid = Register()) const;
  void localRename(const MachineInstr &X, MachineBasicBlock &B, Register Def,
                   SmallVectorImpl<Place> &Out, Register Avoid) const;
  Register freshReg(Register Avoid = Register()) const;
  Register pickReg(MachineInstr &X, Register Def, ArrayRef<MachineBasicBlock *> Ins,
                   SmallVectorImpl<MachineOperand *> &Uses, bool &NeedMv,
                   ArrayRef<Register> Skip = {}) const;
  bool renameRange(MachineInstr &X, Register Def, Register N,
                   SmallVectorImpl<MachineOperand *> &Uses,
                   bool &NeedMv) const;
  void apply(const Plan &P, Trial &T);
  void fillPlans(MachineBasicBlock &B, MachineBasicBlock &S,
                 SmallVectorImpl<Plan> &Out);
  void rotatePlans(MachineLoop &L, SmallVectorImpl<Plan> &Out);
  void joinPlans(MachineBasicBlock &S, SmallVectorImpl<Plan> &Out);
  bool join(MachineBasicBlock &S);
  bool search(function_ref<void(SmallVectorImpl<Plan> &)> Enumerate,
              ArrayRef<MachineBasicBlock *> Blocks, double Slack = 0);
  bool fill(MachineBasicBlock &B);
  bool rotate(MachineLoop &L);
};

} // namespace

char GPUVMGlobalFill::ID = 0;

FunctionPass *llvm::createGPUVMGlobalFillPass() {
  return new GPUVMGlobalFill();
}

// The bundles a chain of blocks makes (a labeled block and the blocks only
// fallen into after it), each weighed by the frequency of the block where it
// starts.
double GPUVMGlobalFill::chainCost(MachineBasicBlock *H) const {
  GPUVM::Packer P;
  double C = 0;
  for (MachineFunction::iterator It = H->getIterator(), E = MF->end();
       It != E; ++It) {
    if (&*It != H && GPUVM::startsBundle(*It))
      break;
    double F = freq(&*It);
    for (const MachineInstr &MI : *It)
      if (P.put(GPUVM::describeOp(MI, TRI)))
        C += F;
  }
  return C;
}

double GPUVMGlobalFill::cost(ArrayRef<MachineBasicBlock *> Blocks) const {
  SmallPtrSet<MachineBasicBlock *, 4> Done;
  double C = 0;
  for (MachineBasicBlock *B : Blocks) {
    MachineBasicBlock *H = Head.lookup(B);
    if (Done.insert(H).second)
      C += chainCost(H);
  }
  return C;
}

bool GPUVMGlobalFill::liveIn(const MachineBasicBlock &MBB, Register R) const {
  for (const auto &LI : MBB.liveins())
    if (TRI->regsOverlap(LI.PhysReg, R))
      return true;
  return false;
}

// R is live into a successor of B other than S
bool GPUVMGlobalFill::liveInOthers(const MachineBasicBlock &B,
                                   const MachineBasicBlock *S,
                                   Register R) const {
  for (const MachineBasicBlock *O : B.successors())
    if (O != S && liveIn(*O, R))
      return true;
  return false;
}

// An op that may run elsewhere: one op word, one result, no side effect, no
// store
bool GPUVMGlobalFill::movableOp(const MachineInstr &I) const {
  if (I.isTerminator() || I.isCall() || I.hasUnmodeledSideEffects() ||
      I.mayStore() || I.isInlineAsm() || I.isMetaInstruction() ||
      I.isDebugInstr() || I.hasOrderedMemoryRef() || I.isPseudo() ||
      I.getFlag(MachineInstr::FrameSetup) ||
      I.getFlag(MachineInstr::FrameDestroy) ||
      I.getNumImplicitOperands() != 0)
    return false;
  for (const MachineOperand &MO : I.operands()) {
    if (!MO.isReg() || !MO.getReg())
      continue;
    if (!MO.getReg().isPhysical() ||
        !GPUVM::GPRRegClass.contains(MO.getReg()))
      return false;
    if (MO.isDef() && MRI->isReserved(MO.getReg()))
      return false;
  }
  return singleDef(I).isValid();
}

Register GPUVMGlobalFill::singleDef(const MachineInstr &I) const {
  Register D;
  for (const MachineOperand &MO : I.operands())
    if (MO.isReg() && MO.isDef()) {
      if (D)
        return Register();
      D = MO.getReg();
    }
  if (D == GPUVM::R0)
    return Register();
  return D;
}

// B's branch tests Base against 0: the path to To has it not 0, the others
// 0, where a load of [Base + Off] reads the boot area.
bool GPUVMGlobalFill::nullTested(Register Base, int64_t Off,
                                 const MachineBasicBlock &B,
                                 const MachineBasicBlock *To) const {
  if (Off < 0 || Off >= NullWindow || B.succ_size() != 2)
    return false;
  MachineBasicBlock::const_iterator T = B.getFirstTerminator();
  if (T == B.end())
    return false;
  const MachineInstr &Br = *T;
  bool Eq;
  switch (Br.getOpcode()) {
  case GPUVM::BEQI:
  case GPUVM::BNEI:
    if (Br.getOperand(0).getReg() != Base || Br.getOperand(1).getImm() != 0)
      return false;
    Eq = Br.getOpcode() == GPUVM::BEQI;
    break;
  case GPUVM::BEQ:
  case GPUVM::BNE: {
    Register A = Br.getOperand(0).getReg(), C = Br.getOperand(1).getReg();
    if (!((A == Base && C == GPUVM::R0) || (C == Base && A == GPUVM::R0)))
      return false;
    Eq = Br.getOpcode() == GPUVM::BEQ;
    break;
  }
  default:
    return false;
  }
  const MachineBasicBlock *Taken =
      Br.getOperand(Br.getNumOperands() - 1).getMBB();
  // Eq: taken when base == 0, so To must be the other one
  return Eq ? Taken != To : Taken == To;
}

// On every path to I (in MBB) an access through Base near Base + Off was made
// (Delta: Base at the start point - Base here).
bool GPUVMGlobalFill::nearOnPaths(
    Register Base, int64_t Off, MachineBasicBlock *MBB,
    MachineBasicBlock::iterator I, int64_t Delta, unsigned &Budget,
    SmallPtrSetImpl<MachineBasicBlock *> &OnPath) const {
  while (I != MBB->begin()) {
    --I;
    if (I->isDebugInstr() || I->isMetaInstruction())
      continue;
    if (Budget == 0)
      return false;
    --Budget;
    if (I->isCall())
      return false;
    // An instruction that writes Base: what it accessed went through Base's
    // value before it (`lw p, 0(p)`), another value than the load's
    // (spec_load.c, QuickJS): only a step or a copy is followed.
    if (I->modifiesRegister(Base, TRI)) {
      // Base = V + k (a step of it, or a copy of another register): on with
      // V
      if (I->getOpcode() == GPUVM::ADDI && I->getOperand(0).getReg() == Base &&
          I->getOperand(1).isReg() && I->getOperand(1).getReg() != GPUVM::R0 &&
          I->getOperand(2).isImm()) {
        Delta += I->getOperand(2).getImm();
        Base = I->getOperand(1).getReg();
        continue;
      }
      return false;
    }
    unsigned F = GPUVMII::getForm(I->getDesc().TSFlags);
    if ((F == GPUVMII::FrmLoad || F == GPUVMII::FrmStore) &&
        I->mayLoadOrStore() && !I->hasOrderedMemoryRef()) {
      const MachineOperand &BO = I->getOperand(1);
      const MachineOperand &OO = I->getOperand(2);
      if (BO.isReg() && BO.getReg() == Base && OO.isImm()) {
        int64_t A = OO.getImm() - Delta;
        if (std::abs(A - Off) <= (int64_t)SpecWindow)
          return true;
      }
    }
  }
  if (MBB->pred_empty() || !OnPath.insert(MBB).second)
    return false;
  for (MachineBasicBlock *P : MBB->predecessors())
    if (!nearOnPaths(Base, Off, P, P->end(), Delta, Budget, OnPath))
      return false;
  return true;
}

// X may run on every path out of the place's block, where it ran only on the
// way to To.
bool GPUVMGlobalFill::speculable(const MachineInstr &X, const Place &P,
                                 const MachineBasicBlock *To) const {
  MachineBasicBlock &B = *P.Into;
  if (B.succ_size() == 1 && *B.succ_begin() == To)
    return true;
  if (!X.mayLoad())
    return true;
  if (GPUVMII::getForm(X.getDesc().TSFlags) != GPUVMII::FrmLoad)
    return false;
  Register Base = X.getOperand(1).getReg();
  for (auto &[Idx, R] : P.Subst)
    if (Idx == 1)
      Base = R;
  // [r0 + symbol], [sp + k], PIC's [the function + %fnrel(symbol)]
  const MachineOperand &Imm = X.getOperand(2);
  // (an undefined weak symbol is 0: not a load that cannot fault)
  if (Imm.isGlobal() && Imm.getGlobal()->hasExternalWeakLinkage())
    return false;
  if (Base == GPUVM::R0 || Base == GPUVM::R2 ||
      (!Imm.isImm() && Imm.getTargetFlags() == GPUVMII::MO_FNREL))
    return true;
  if (Base == GPUVM::R32 && MF->getSubtarget().getFrameLowering()->hasFP(*MF))
    return true;
  if (gpuvmLoadInsideGlobal(X))
    return true;
  if (!X.getOperand(2).isImm())
    return false;
  int64_t Off = X.getOperand(2).getImm();
  if (nullTested(Base, Off, B, To))
    return true;
  unsigned Budget = 60;
  SmallPtrSet<MachineBasicBlock *, 8> OnPath;
  MachineBasicBlock::iterator I =
      P.Before ? P.Before->getIterator() : B.end();
  if (nearOnPaths(Base, Off + P.Adj, &B, I, 0, Budget, OnPath))
    return true;
  // or every other way out of B makes such an access before long (X's base
  // at B's end is its own register: what follows the place does not write
  // it, or a mv gives it the value X reads)
  Register Orig = X.getOperand(1).getReg();
  for (MachineBasicBlock *O : B.successors())
    if (O != To && !nearAhead(Orig, Off, O))
      return false;
  return true;
}

// The path into O makes an access through Base near Base + Off (Base as it
// is on entering O) before anything else could leave it.
bool GPUVMGlobalFill::nearAhead(Register Base, int64_t Off,
                                MachineBasicBlock *O) const {
  int64_t Delta = 0; // Base here - Base on entering O
  unsigned Budget = 30;
  SmallPtrSet<MachineBasicBlock *, 4> Seen;
  while (Seen.insert(O).second) {
    for (MachineInstr &I : *O) {
      if (I.isDebugInstr() || I.isMetaInstruction())
        continue;
      if (Budget == 0 || I.isCall() || I.isTerminator())
        break;
      --Budget;
      unsigned F = GPUVMII::getForm(I.getDesc().TSFlags);
      if ((F == GPUVMII::FrmLoad || F == GPUVMII::FrmStore) &&
          I.mayLoadOrStore() && !I.hasOrderedMemoryRef() &&
          I.getOperand(1).isReg() && I.getOperand(1).getReg() == Base &&
          I.getOperand(2).isImm() &&
          std::abs(I.getOperand(2).getImm() + Delta - Off) <=
              (int64_t)SpecWindow)
        return true;
      if (I.modifiesRegister(Base, TRI)) {
        if (I.getOpcode() == GPUVM::ADDI && I.getOperand(1).isReg() &&
            I.getOperand(1).getReg() == Base && I.getOperand(2).isImm()) {
          Delta += I.getOperand(2).getImm();
          continue;
        }
        return false;
      }
    }
    // on through a block with one way out (no branch taken before it)
    if (O->succ_size() != 1 || Budget == 0)
      return false;
    for (MachineInstr &T : O->terminators())
      if (T.isConditionalBranch() || T.isIndirectBranch())
        return false;
    O = *O->succ_begin();
  }
  return false;
}

// X can go to the top of its block: nothing above it there conflicts.
bool GPUVMGlobalFill::hoistsInBlock(const MachineInstr &X) const {
  const MachineBasicBlock &S = *X.getParent();
  for (const MachineInstr &I : S) {
    if (&I == &X)
      return true;
    if (I.isDebugInstr() || I.isMetaInstruction())
      continue;
    if (I.isCall() || I.hasUnmodeledSideEffects() || I.isInlineAsm())
      return false;
    if (X.mayLoad() && I.mayStore())
      return false;
    for (const MachineOperand &MO : X.operands()) {
      if (!MO.isReg() || !MO.getReg())
        continue;
      if (MO.isUse() && I.modifiesRegister(MO.getReg(), TRI))
        return false;
      if (MO.isDef() && (I.readsRegister(MO.getReg(), TRI) ||
                         I.modifiesRegister(MO.getReg(), TRI)))
        return false;
    }
  }
  return false;
}

// X (its result in Def) placed before At in B sees what it saw at the end of
// B, and nothing after it sees Def: nothing from At on reads or writes Def,
// writes what X reads (unless a `mv u, v`: X reads v then, which nothing
// before the mv writes), or stores when X loads.
bool GPUVMGlobalFill::fitsAfter(const MachineInstr &X,
                                MachineBasicBlock::iterator At,
                                MachineBasicBlock &B, Register Def,
                                Substs &Subst, int64_t &Adj,
                                bool DefRenamed) const {
  Subst.clear();
  Adj = 0;
  // X reads its base (operand 1) and an immediate: a load or an addi
  unsigned F = GPUVMII::getForm(X.getDesc().TSFlags);
  bool Based = (F == GPUVMII::FrmLoad || X.getOpcode() == GPUVM::ADDI) &&
               X.getOperand(1).isReg() && X.getOperand(2).isImm();
  Register AdjReg;
  SmallVector<std::pair<unsigned, Register>, 3> Uses; // operand, register
  for (unsigned K = 0; K < X.getNumOperands(); ++K) {
    const MachineOperand &MO = X.getOperand(K);
    if (MO.isReg() && MO.isUse() && MO.getReg() && MO.getReg() != GPUVM::R0)
      Uses.push_back({K, MO.getReg()});
  }
  SmallVector<Register, 8> Written;
  for (MachineBasicBlock::iterator I = At; I != B.end(); ++I) {
    if (&*I == &X || I->isDebugInstr())
      continue;
    if (I->isCall() || I->isInlineAsm() ||
        (X.mayLoad() && (I->mayStore() || I->hasUnmodeledSideEffects())))
      return false;
    if ((!DefRenamed && I->readsRegister(Def, TRI)) ||
        I->modifiesRegister(Def, TRI))
      return false;
    for (auto &[K, R] : Uses) {
      if (!R || !I->modifiesRegister(R, TRI))
        continue;
      // a mv R, V: X reads V (if nothing wrote V since At; R must not be
      // written again after it, checked at the end)
      bool Fwd = I->getOpcode() == GPUVM::ADDI &&
                 I->getOperand(0).getReg() == R &&
                 I->getOperand(1).isReg() && I->getOperand(2).isImm() &&
                 I->getOperand(2).getImm() == 0;
      // an addi R, R, k when R is X's base alone: X's immediate takes k
      if (Based && K == 1 && I->getOpcode() == GPUVM::ADDI &&
          I->getOperand(0).getReg() == R && I->getOperand(1).isReg() &&
          I->getOperand(1).getReg() == R && I->getOperand(2).isImm() &&
          !X.getOperand(2).isImm() == false) {
        bool Only = true;
        for (unsigned J = 0; J < X.getNumOperands(); ++J)
          if (J != 1 && X.getOperand(J).isReg() &&
              X.getOperand(J).getReg() == R)
            Only = false;
        if (Only) {
          Adj += I->getOperand(2).getImm();
          AdjReg = R;
          continue;
        }
      }
      if (!Fwd || AdjReg == R)
        return false;
      Register V = I->getOperand(1).getReg();
      if (V == Def || llvm::is_contained(Written, V))
        return false;
      Subst.push_back({K, V});
      R = Register();
    }
    for (const MachineOperand &MO : I->operands())
      if (MO.isReg() && MO.isDef() && MO.getReg())
        Written.push_back(MO.getReg());
  }
  // A forwarded register must not be written again after its mv.
  for (auto &[K, V] : Subst) {
    Register Orig = X.getOperand(K).getReg();
    unsigned Writes = 0;
    for (MachineBasicBlock::iterator I = At; I != B.end(); ++I)
      if (&*I != &X && !I->isDebugInstr() && I->modifiesRegister(Orig, TRI))
        ++Writes;
    if (Writes != 1)
      return false;
  }
  return true;
}

// Where in B X may go: before its first terminator, and earlier while it
// still fits (up to a few ops back).
void GPUVMGlobalFill::places(const MachineInstr &X, MachineBasicBlock &B,
                             Register Def, SmallVectorImpl<Place> &Out,
                             Register Avoid) const {
  MachineBasicBlock::iterator I = B.getFirstTerminator();
  for (unsigned N = 0; N < 6; ++N) {
    Place P;
    P.Into = &B;
    P.Before = I == B.end() ? nullptr : &*I;
    if (!fitsAfter(X, I, B, Def, P.Subst, P.Adj) ||
        !isInt<32>(X.getNumOperands() > 2 && X.getOperand(2).isImm()
                       ? X.getOperand(2).getImm() + P.Adj
                       : 0)) {
      if (N == 0)
        localRename(X, B, Def, Out, Avoid);
      return;
    }
    Out.push_back(P);
    // the place before the previous op
    do {
      if (I == B.begin())
        return;
      --I;
    } while (I->isDebugInstr() || I->isMetaInstruction() || &*I == &X);
  }
}

// X at B's end, where B's terminators read B's own last value of Def: that
// value goes to a fresh register (its def and its reads in B), X reads it
// there if it reads Def.
// Whether I has a use tied to a def (addbnz's counter): its def and that use
// are one register field, renamed together or not at all.
static bool hasTiedUse(const MachineInstr &I) {
  for (const MachineOperand &MO : I.operands())
    if (MO.isReg() && MO.isUse() && MO.isTied())
      return true;
  return false;
}

void GPUVMGlobalFill::localRename(const MachineInstr &X, MachineBasicBlock &B,
                                  Register Def, SmallVectorImpl<Place> &Out,
                                  Register Avoid) const {
  MachineBasicBlock::iterator T = B.getFirstTerminator();
  for (MachineBasicBlock::iterator I = T; I != B.end(); ++I)
    if (I->modifiesRegister(Def, TRI) || I->isCall() ||
        I->isIndirectBranch())
      return;
    else
      for (const MachineOperand &MO : X.operands())
        if (MO.isReg() && MO.isUse() && MO.getReg() &&
            I->modifiesRegister(MO.getReg(), TRI))
          return;
  // the last def of Def: in B or up the blocks that alone lead to it (whose
  // other ways out do not read Def)
  MachineInstr *D = nullptr;
  SmallVector<MachineBasicBlock *, 4> Chain{&B};
  MachineBasicBlock *Cur = &B;
  MachineBasicBlock::iterator I = T;
  while (!D) {
    while (I != Cur->begin()) {
      --I;
      if (I->isDebugInstr())
        continue;
      if (I->modifiesRegister(Def, TRI)) {
        D = &*I;
        break;
      }
    }
    if (D)
      break;
    if (Cur->pred_size() != 1 || Chain.size() == 4)
      return;
    MachineBasicBlock *P = *Cur->pred_begin();
    if (P == &B || llvm::is_contained(Chain, P) || liveInOthers(*P, Cur, Def))
      return;
    Chain.push_back(P);
    Cur = P;
    I = P->end();
  }
  std::reverse(Chain.begin(), Chain.end()); // D's block first
  if (D == &X || D->isCall() || D->isInlineAsm() || singleDef(*D) != Def ||
      D->getNumImplicitOperands() != 0 || hasTiedUse(*D))
    return;
  Register N = freshReg(Avoid);
  if (!N)
    return;
  for (MachineBasicBlock *C : Chain)
    for (MachineBasicBlock::iterator J =
             C == D->getParent() ? std::next(D->getIterator()) : C->begin();
         J != C->end(); ++J)
      for (const MachineOperand &MO : J->operands())
        if ((J->isInlineAsm() && MO.isReg() && MO.getReg() == Def) || // (its register is the constraint's)
            (MO.isReg() && MO.getReg() && MO.getReg() != Def &&
             TRI->regsOverlap(MO.getReg(), Def)) ||
            (MO.isReg() && MO.getReg() == Def &&
             (MO.isImplicit() || MO.isTied())) ||
            (MO.isRegMask() && MO.clobbersPhysReg(N)))
          return;
  // X before the terminators, and earlier while it fits (after D)
  MachineBasicBlock::iterator At = T;
  for (unsigned K = 0; K < 6; ++K) {
    Place P;
    P.Into = &B;
    P.Before = At == B.end() ? nullptr : &*At;
    P.RenDef = D;
    P.RenTo = N;
    P.RenChain.assign(Chain.begin(), Chain.end());
    if (!fitsAfter(X, At, B, Def, P.Subst, P.Adj, true))
      return;
    for (unsigned J = 0; J < X.getNumOperands(); ++J) {
      const MachineOperand &MO = X.getOperand(J);
      if (MO.isReg() && MO.isUse() && MO.getReg() == Def)
        P.Subst.push_back({J, N});
    }
    Out.push_back(P);
    do {
      if (At == B.begin())
        return;
      --At;
    } while (At->isDebugInstr() || At->isMetaInstruction() || &*At == &X);
    if (&*At == D)
      return;
  }
}

// A register for X's result moved from its block S into Ins: one the function
// does not use, or else one free there: not named in S, live into none of the
// ways out of Ins (S's too) and, if callee-saved, already saved (the function
// uses it). The places then check that nothing after X in Ins names it.
Register GPUVMGlobalFill::pickReg(MachineInstr &X, Register Def,
                                  ArrayRef<MachineBasicBlock *> Ins,
                                  SmallVectorImpl<MachineOperand *> &Uses,
                                  bool &NeedMv, ArrayRef<Register> Skip) const {
  Register F = freshReg();
  if (F && !is_contained(Skip, F)) {
    Uses.clear();
    return renameRange(X, Def, F, Uses, NeedMv) ? F : Register();
  }
  MachineBasicBlock &S = *X.getParent();
  unsigned Tries = 0;
  for (MCPhysReg R : GPUVM::GPRRegClass) {
    unsigned E = TRI->getEncodingValue(R);
    if (E < 5 || E == 63 || R == Def || MRI->isReserved(R) || !Used.test(R) ||
        is_contained(Skip, Register(R)))
      continue;
    if (++Tries > 24)
      break;
    bool Free = true;
    for (MachineInstr &I : S) {
      for (const MachineOperand &MO : I.operands())
        if ((MO.isReg() && MO.getReg() && TRI->regsOverlap(MO.getReg(), R)) ||
            (MO.isRegMask() && MO.clobbersPhysReg(R)))
          Free = false;
      if (!Free)
        break;
    }
    for (MachineBasicBlock *B : Ins)
      for (MachineBasicBlock *O : B->successors())
        Free &= !liveIn(*O, R);
    for (MachineBasicBlock *O : S.successors())
      Free &= !liveIn(*O, R);
    if (!Free)
      continue;
    Uses.clear();
    if (renameRange(X, Def, R, Uses, NeedMv))
      return R;
  }
  return Register();
}

Register GPUVMGlobalFill::freshReg(Register Avoid) const {
  const MCPhysReg *CSR = TRI->getCalleeSavedRegs(MF);
  for (MCPhysReg R : GPUVM::GPRRegClass) {
    unsigned E = TRI->getEncodingValue(R);
    // not zero, ra, sp, gp, tp, nor t26 (lld's thunks)
    if (E < 5 || E == 63 || Used.test(R) || MRI->isReserved(R) || R == Avoid)
      continue;
    bool Saved = false;
    for (const MCPhysReg *C = CSR; *C; ++C)
      if (*C == R)
        Saved = true;
    if (!Saved)
      return R;
  }
  return Register();
}

// The uses of Def after X in its block until Def is written again (those to
// rewrite to N), and whether Def lives on past them (a mv back is needed).
bool GPUVMGlobalFill::renameRange(MachineInstr &X, Register Def, Register N,
                                  SmallVectorImpl<MachineOperand *> &Uses,
                                  bool &NeedMv) const {
  MachineBasicBlock &S = *X.getParent();
  NeedMv = false;
  for (MachineBasicBlock::iterator I = std::next(X.getIterator());
       I != S.end(); ++I) {
    if (I->isDebugInstr())
      continue;
    if (I->isInlineAsm())
      return false;
    for (MachineOperand &MO : I->operands()) {
      if (MO.isRegMask() && MO.clobbersPhysReg(N))
        return false;
      if (!MO.isReg() || !MO.getReg())
        continue;
      if (MO.getReg() != Def && TRI->regsOverlap(MO.getReg(), Def))
        return false;
      if (MO.getReg() == Def && MO.isUse()) {
        // (a use tied to a def, addbnz's counter: one register field, so the
        // def would be renamed with it and Def's later readers left behind)
        if (MO.isImplicit() || MO.isTied())
          return false;
        Uses.push_back(&MO);
      }
    }
    if (I->modifiesRegister(Def, TRI))
      return true; // written again: Def's old value ends here
  }
  for (const MachineBasicBlock *O : S.successors())
    if (liveIn(*O, Def))
      NeedMv = true;
  return true;
}

void GPUVMGlobalFill::apply(const Plan &P, Trial &T) {
  SmallVector<const Place *, 3> All{&P.At};
  for (const Place &E : P.Enter)
    All.push_back(&E);
  for (const Place *Pl : All)
    if (Pl->RenDef) {
      Register Def = singleDef(*Pl->RenDef);
      for (MachineBasicBlock *C : Pl->RenChain)
        for (MachineBasicBlock::iterator I =
                 C == Pl->RenDef->getParent() ? Pl->RenDef->getIterator()
                                              : C->begin();
             I != C->end(); ++I)
          for (MachineOperand &MO : I->operands())
            if (MO.isReg() && MO.getReg() == Def &&
                (&*I == Pl->RenDef ? MO.isDef() : MO.isUse()))
              T.rename(MO, Pl->RenTo);
    }
  MachineInstr *X = P.X;
  MachineBasicBlock *S = X->getParent();
  MachineBasicBlock::iterator XAt = std::next(X->getIterator());
  Register Def = singleDef(*X);
  for (const Place &E : P.Enter) {
    MachineInstr *C = MF->CloneMachineInstr(X);
    for (auto &[K, R] : E.Subst)
      C->getOperand(K).setReg(R);
    if (E.Adj)
      C->getOperand(2).setImm(C->getOperand(2).getImm() + E.Adj);
    if (P.To)
      for (MachineOperand &MO : C->operands())
        if (MO.isReg() && MO.isDef())
          MO.setReg(P.To);
    E.Into->insert(E.Before ? E.Before->getIterator() : E.Into->end(), C);
    T.Added.push_back(C);
  }
  if (P.To) {
    SmallVector<MachineOperand *, 4> Uses;
    bool NeedMv;
    renameRange(*X, Def, P.To, Uses, NeedMv);
    for (MachineOperand *MO : Uses)
      T.rename(*MO, P.To);
    for (MachineOperand &MO : X->operands())
      if (MO.isReg() && MO.isDef())
        T.rename(MO, P.To);
  }
  for (auto &[K, R] : P.At.Subst)
    T.rename(X->getOperand(K), R);
  if (P.At.Adj)
    T.adjust(X->getOperand(2), P.At.Adj);
  // (moved before the mv is added: the undo puts it back before XAt)
  T.move(X, *P.At.Into,
         P.At.Before ? P.At.Before->getIterator() : P.At.Into->end());
  if (P.To && P.MvAt >= 0) {
    MachineBasicBlock::iterator At =
        P.MvAt == 0 ? S->getFirstTerminator() : XAt;
    MachineInstr *Mv =
        BuildMI(*S, At, X->getDebugLoc(), TII->get(GPUVM::ADDI), Def)
            .addReg(P.To)
            .addImm(0);
    T.Added.push_back(Mv);
  }
}

// The ways to move an op of S (whose one predecessor is B) into B.
void GPUVMGlobalFill::fillPlans(MachineBasicBlock &B, MachineBasicBlock &S,
                                SmallVectorImpl<Plan> &Out) {
  double Wasted = std::max(0.0, freq(&B) - freq(&S));
  unsigned Scanned = 0;
  for (MachineInstr &X : S) {
    if (X.isDebugInstr() || X.isMetaInstruction())
      continue;
    if (++Scanned > ScanOps || X.isTerminator())
      break;
    if (!movableOp(X) || !hoistsInBlock(X))
      continue;
    Register Def = singleDef(X);
    double Waste = Wasted * (X.mayLoad() ? LoadWaste : OpWaste);
    for (int Ren = 0; Ren < (Rename ? 2 : 1); ++Ren) {
      Register N;
      bool NeedMv = false;
      SmallVector<MachineOperand *, 4> Uses;
      if (Ren) {
        N = pickReg(X, Def, {&B}, Uses, NeedMv);
        if (!N)
          break;
      } else if (liveInOthers(B, &S, Def)) {
        continue;
      }
      SmallVector<Place, 6> Ps;
      places(X, B, Ren ? N : Def, Ps, N);
      for (Place &At : Ps) {
        if (!speculable(X, At, &S))
          continue;
        for (int Mv = NeedMv ? 0 : -1; Mv <= (NeedMv ? 1 : -1); ++Mv) {
          Plan P;
          P.X = &X;
          P.At = At;
          P.To = N;
          P.MvAt = Mv;
          P.Waste = Waste + (Mv >= 0 ? OpWaste * freq(&S) : 0.0);
          Out.push_back(P);
        }
      }
    }
  }
}

// The ways to move an op at the top of L's header to the bottom of its latch
// and of the block that enters it.
void GPUVMGlobalFill::rotatePlans(MachineLoop &L, SmallVectorImpl<Plan> &Out) {
  MachineBasicBlock *H = L.getHeader();
  MachineBasicBlock *Latch = L.getLoopLatch();
  SmallVector<MachineBasicBlock *, 2> Ins;
  for (MachineBasicBlock *P : H->predecessors())
    if (P != Latch)
      Ins.push_back(P);
  double Wasted = std::max(0.0, freq(Latch) - edgeFreq(Latch, H));
  for (MachineBasicBlock *P : Ins)
    Wasted += std::max(0.0, freq(P) - edgeFreq(P, H));
  unsigned Scanned = 0;
  for (MachineInstr &X : *H) {
    if (X.isDebugInstr() || X.isMetaInstruction())
      continue;
    if (++Scanned > ScanOps || X.isTerminator())
      break;
    if (!movableOp(X) || !hoistsInBlock(X))
      continue;
    Register Def = singleDef(X);
    bool Live = liveInOthers(*Latch, H, Def);
    for (MachineBasicBlock *P : Ins)
      Live |= liveInOthers(*P, H, Def);
    if (Live)
      continue;
    SmallVector<Place, 6> AtL;
    places(X, *Latch, Def, AtL);
    // each entering block: its first safe place
    SmallVector<Place, 2> Enter;
    for (MachineBasicBlock *P : Ins) {
      SmallVector<Place, 6> AtP;
      places(X, *P, Def, AtP, AtL.empty() ? Register() : AtL[0].RenTo);
      for (Place &A2 : AtP)
        if (speculable(X, A2, H)) {
          Enter.push_back(A2);
          break;
        }
    }
    LLVM_DEBUG(dbgs() << "rotate? " << X << "  " << AtL.size() << " places in "
                      << printMBBReference(*Latch) << ", " << Enter.size()
                      << " of " << Ins.size() << " entering\n");
    if (Enter.size() != Ins.size())
      continue;
    for (Place &A : AtL) {
      if (!speculable(X, A, H))
        continue;
      Plan Pl;
      Pl.X = &X;
      Pl.At = A;
      Pl.Enter = Enter;
      Pl.Waste = Wasted * (X.mayLoad() ? LoadWaste : OpWaste);
      Out.push_back(Pl);
    }
  }
}

// The best of the plans Enumerate gives, alone or followed by a second (found
// after the first is made), by the bundles of Blocks; made when it gains.
bool GPUVMGlobalFill::search(
    function_ref<void(SmallVectorImpl<Plan> &)> Enumerate,
    ArrayRef<MachineBasicBlock *> Blocks, double Slack) {
  double Base = cost(Blocks);
  double Best = -1e-9;
  Plan Best1, Best2;
  bool Two = false;
  SmallVector<Plan, 16> First;
  Enumerate(First);
  for (const Plan &P1 : First) {
    Trial T1;
    apply(P1, T1);
    BitVector Saved = Used;
    for (Register R : planRegs(P1))
      Used.set(R);

    double C1 = cost(Blocks) - Base;
    LLVM_DEBUG(dbgs() << "  try " << *P1.X << "    bundles " << C1
                      << " waste " << P1.Waste << "\n");
    if (C1 + P1.Waste < Best) {
      Best = C1 + P1.Waste;
      Best1 = P1;
      Two = false;
    }
    if (Pairs && C1 <= Slack + 1e-9) {
      SmallVector<Plan, 16> Second;
      Enumerate(Second);
      for (const Plan &P2 : Second) {
        Trial T2;
        apply(P2, T2);
        double C2 = cost(Blocks) - Base + P1.Waste + P2.Waste;
        T2.undo();
        if (C2 < Best) {
          Best = C2;
          Best1 = P1;
          Best2 = P2;
          Two = true;
        }
      }
    }
    Used = Saved;
    T1.undo();
  }
  if (!Best1.X)
    return false;
  LLVM_DEBUG(dbgs() << "move " << *Best1.X
                    << (Two ? "  and " : "") ;
             if (Two) dbgs() << *Best2.X;
             dbgs() << "  gain " << -Best << "\n");
  Trial T;
  apply(Best1, T);
  for (Register R : planRegs(Best1))
    Used.set(R);
  if (Two) {
    apply(Best2, T);
    for (Register R : planRegs(Best2))
      Used.set(R);
  }
  SmallVector<MachineBasicBlock *, 4> Bs(Blocks.begin(), Blocks.end());
  fullyRecomputeLiveIns(Bs);
  return true;
}

bool GPUVMGlobalFill::fill(MachineBasicBlock &B) {
  if (B.succ_empty())
    return false;
  for (MachineBasicBlock::iterator I = B.getFirstTerminator(); I != B.end();
       ++I)
    if (I->isIndirectBranch() || I->isReturn() || I->isCall())
      return false;
  for (MachineBasicBlock *S : B.successors()) {
    if (S == &B || S->pred_size() != 1 || S->isEHPad() ||
        S->hasAddressTaken())
      continue;
    if (search([&](SmallVectorImpl<Plan> &Out) { fillPlans(B, *S, Out); },
               {&B, S}))
      return true;
  }
  return false;
}

// The ways to move an op at the top of S (a block several others branch or
// fall to, not a loop's header) into every one of them, its result renamed
// where one of their other ways out reads the register.
void GPUVMGlobalFill::joinPlans(MachineBasicBlock &S,
                                SmallVectorImpl<Plan> &Out) {
  SmallVector<MachineBasicBlock *, 4> Ins(S.predecessors());
  double Wasted = 0;
  for (MachineBasicBlock *P : Ins)
    Wasted += std::max(0.0, freq(P) - edgeFreq(P, &S));
  unsigned Scanned = 0;
  for (MachineInstr &X : S) {
    if (X.isDebugInstr() || X.isMetaInstruction())
      continue;
    if (++Scanned > ScanOps || X.isTerminator())
      break;
    if (!movableOp(X) || !hoistsInBlock(X))
      continue;
    Register Def = singleDef(X);
    SmallVector<Register, 6> Skip;
    for (int Try = 0; Try < (Rename ? 7 : 1); ++Try) {
      bool Ren = Try > 0;
      Register N;
      bool NeedMv = false;
      SmallVector<MachineOperand *, 4> Uses;
      if (Ren) {
        N = pickReg(X, Def, Ins, Uses, NeedMv, Skip);
        if (!N)
          break;
        Skip.push_back(N);
      } else {
        bool Live = false;
        for (MachineBasicBlock *P : Ins)
          Live |= liveInOthers(*P, &S, Def);
        if (Live)
          continue;
      }
      Register D = Ren ? N : Def;
      // each predecessor: its first safe place
      SmallVector<Place, 4> At;
      for (MachineBasicBlock *P : Ins) {
        SmallVector<Place, 6> Ps;
        places(X, *P, D, Ps, N);
        for (Place &A : Ps)
          if (speculable(X, A, &S)) {
            At.push_back(A);
            break;
          }
      }
      LLVM_DEBUG(dbgs() << "join? " << X << "  rename " << Ren << ": "
                        << At.size() << " of " << Ins.size() << " places\n");
      if (At.size() != Ins.size())
        continue;
      for (int Mv = NeedMv ? 0 : -1; Mv <= (NeedMv ? 1 : -1); ++Mv) {
        Plan Pl;
        Pl.X = &X;
        Pl.At = At[0];
        Pl.Enter.assign(At.begin() + 1, At.end());
        Pl.To = N;
        Pl.MvAt = Mv;
        Pl.Waste = Wasted * (X.mayLoad() ? LoadWaste : OpWaste) +
                   (Mv >= 0 ? OpWaste * freq(&S) : 0.0);
        Out.push_back(Pl);
      }
      if (Ren)
        break;
    }
  }
}

bool GPUVMGlobalFill::join(MachineBasicBlock &S) {
  if (S.pred_size() < 2 || S.pred_size() > 6 || S.isEHPad() ||
      S.hasAddressTaken() || MLI->isLoopHeader(&S))
    return false;
  SmallVector<MachineBasicBlock *, 8> Blocks{&S};
  for (MachineBasicBlock *P : S.predecessors()) {
    if (P == &S)
      return false;
    for (MachineBasicBlock::iterator I = P->getFirstTerminator(); I != P->end();
         ++I)
      if (I->isIndirectBranch() || I->isReturn() || I->isCall())
        return false;
    Blocks.push_back(P);
  }
  return search([&](SmallVectorImpl<Plan> &Out) { joinPlans(S, Out); },
                Blocks);
}

bool GPUVMGlobalFill::rotate(MachineLoop &L) {
  MachineBasicBlock *H = L.getHeader();
  MachineBasicBlock *Latch = L.getLoopLatch();
  if (!Latch || H->isEHPad() || H->hasAddressTaken() || H->pred_size() > 4)
    return false;
  SmallVector<MachineBasicBlock *, 4> Blocks{Latch, H};
  for (MachineBasicBlock *P : H->predecessors()) {
    if (P == Latch)
      continue;
    if (L.contains(P))
      return false;
    Blocks.push_back(P);
  }
  for (MachineBasicBlock *B : Blocks)
    for (MachineBasicBlock::iterator I = B->getFirstTerminator();
         I != B->end(); ++I)
      if (I->isIndirectBranch() || I->isReturn() || I->isCall())
        return false;
  // a first op whose copies cost the entering blocks a bundle may still open
  // the way for a second that takes one from the header
  return search([&](SmallVectorImpl<Plan> &Out) { rotatePlans(L, Out); },
                Blocks, 0.999 * freq(H));
}

bool GPUVMGlobalFill::runOnMachineFunction(MachineFunction &Fn) {
  if (skipFunction(Fn.getFunction()) || !GlobalFill ||
      !Fn.getRegInfo().tracksLiveness())
    return false;
  MF = &Fn;
  TII = Fn.getSubtarget<GPUVMSubtarget>().getInstrInfo();
  TRI = Fn.getSubtarget().getRegisterInfo();
  MRI = &Fn.getRegInfo();
  MBFI = &getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  MBPI = &getAnalysis<MachineBranchProbabilityInfoWrapperPass>().getMBPI();
  MLI = &getAnalysis<MachineLoopInfoWrapperPass>().getLI();

  Used.reset();
  Used.resize(TRI->getNumRegs());
  for (MachineBasicBlock &MBB : Fn) {
    for (const auto &LI : MBB.liveins())
      for (MCRegAliasIterator A(LI.PhysReg, TRI, true); A.isValid(); ++A)
        Used.set(*A);
    for (MachineInstr &MI : MBB)
      for (const MachineOperand &MO : MI.operands())
        if (MO.isReg() && MO.getReg() && MO.getReg().isPhysical())
          for (MCRegAliasIterator A(MO.getReg(), TRI, true); A.isValid(); ++A)
            Used.set(*A);
  }
  Head.clear();
  MachineBasicBlock *H = nullptr;
  for (MachineBasicBlock &MBB : Fn) {
    if (!H || GPUVM::startsBundle(MBB))
      H = &MBB;
    Head[&MBB] = H;
  }

  bool Changed = false;
  for (unsigned Round = 0; Round < 4; ++Round) {
    bool Any = false;
    if (Rotate) {
      SmallVector<MachineLoop *, 8> Loops;
      for (MachineLoop *L : *MLI)
        for (MachineLoop *Sub : depth_first(L))
          Loops.push_back(Sub);
      for (MachineLoop *L : Loops)
        for (unsigned K = 0; K < 4 && rotate(*L); ++K)
          Any = true;
    }
    for (MachineBasicBlock &B : Fn)
      for (unsigned K = 0; K < 4 && fill(B); ++K)
        Any = true;
    if (Join)
      for (MachineBasicBlock &B : Fn)
        for (unsigned K = 0; K < 2 && join(B); ++K)
          Any = true;
    Changed |= Any;
    if (!Any)
      break;
  }
  if (Changed) {
    SmallVector<MachineBasicBlock *, 32> Blocks;
    for (MachineBasicBlock &MBB : reverse(Fn))
      Blocks.push_back(&MBB);
    fullyRecomputeLiveIns(Blocks);
    for (MachineBasicBlock &MBB : Fn)
      recomputeLivenessFlags(MBB);
  }
  return Changed;
}
