//===-- GPUVMAsmPrinter.cpp - gpu_vm assembly printer ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each machine instruction becomes one MCInst (an op or a control op); the
// ELF streamer packs them into bundles as the assembler does, and the
// assembly printer prints one a line (the assembler packs those the same way).
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMTargetMachine.h"
#include "MCTargetDesc/GPUVMELFStreamer.h"
#include "MCTargetDesc/GPUVMFixupKinds.h"
#include "MCTargetDesc/GPUVMInstPrinter.h"
#include "TargetInfo/GPUVMTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/IR/Mangler.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {
class GPUVMAsmPrinter : public AsmPrinter {
public:
  static char ID;

  explicit GPUVMAsmPrinter(TargetMachine &TM,
                           std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "gpu_vm Assembly Printer"; }

  void emitInstruction(const MachineInstr *MI) override;
  void emitStartOfAsmFile(Module &M) override;
  void emitBasicBlockStart(const MachineBasicBlock &MBB) override;
  void emitFunctionEntryLabel() override;

  // The function's start as a temporary label (GPUVMII::MO_FNBASE): what the
  // lpc of %fnrel's base names, resolved by the assembler, so that a --wrap
  // of the function's own symbol does not move it.
  MCSymbol *FnBase = nullptr;
  void emitFunctionBodyEnd() override;
  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &OS) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                             const char *ExtraCode, raw_ostream &OS) override;

private:
  void printOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &OS);
};
} // namespace

char GPUVMAsmPrinter::ID = 0;

INITIALIZE_PASS(GPUVMAsmPrinter, "gpuvm-asm-printer", "gpu_vm Assembly Printer",
                false, false)

static MCOperand symbolOperand(const MachineOperand &MO, MCSymbol *Sym,
                               AsmPrinter &AP) {
  MCContext &Ctx = AP.OutContext;
  const MCExpr *E = MCSymbolRefExpr::create(Sym, Ctx);
  if (!MO.isJTI() && !MO.isMBB() && MO.getOffset())
    E = MCBinaryExpr::createAdd(
        E, MCConstantExpr::create(MO.getOffset(), Ctx), Ctx);
  if (MO.getTargetFlags() == GPUVMII::MO_FNBASE)
    E = MCSymbolRefExpr::create(static_cast<GPUVMAsmPrinter &>(AP).FnBase, Ctx);
  if (MO.getTargetFlags() == GPUVMII::MO_TPREL)
    E = MCSpecifierExpr::create(E, GPUVM::S_TPREL, Ctx);
  if (MO.getTargetFlags() == GPUVMII::MO_FNREL)
    E = MCSpecifierExpr::create(
        MCBinaryExpr::createSub(
            E, MCSymbolRefExpr::create(AP.CurrentFnSym, Ctx), Ctx),
        GPUVM::S_FNREL, Ctx);
  return MCOperand::createExpr(E);
}

bool llvm::lowerGPUVMMachineOperandToMCOperand(const MachineOperand &MO,
                                               MCOperand &MCOp,
                                               AsmPrinter &AP) {
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    if (MO.isImplicit())
      return false;
    MCOp = MCOperand::createReg(MO.getReg());
    return true;
  case MachineOperand::MO_Immediate:
    MCOp = MCOperand::createImm(MO.getImm());
    return true;
  case MachineOperand::MO_MachineBasicBlock:
    MCOp = symbolOperand(MO, MO.getMBB()->getSymbol(), AP);
    return true;
  case MachineOperand::MO_GlobalAddress:
    MCOp = symbolOperand(MO, AP.getSymbol(MO.getGlobal()), AP);
    return true;
  case MachineOperand::MO_ExternalSymbol:
    MCOp = symbolOperand(MO, AP.GetExternalSymbolSymbol(MO.getSymbolName()),
                         AP);
    return true;
  case MachineOperand::MO_BlockAddress:
    MCOp = symbolOperand(MO, AP.GetBlockAddressSymbol(MO.getBlockAddress()),
                         AP);
    return true;
  case MachineOperand::MO_JumpTableIndex:
    MCOp = symbolOperand(MO, AP.GetJTISymbol(MO.getIndex()), AP);
    return true;
  case MachineOperand::MO_ConstantPoolIndex:
    MCOp = symbolOperand(MO, AP.GetCPISymbol(MO.getIndex()), AP);
    return true;
  case MachineOperand::MO_RegisterMask:
    return false;
  default:
    report_fatal_error("gpu_vm: unknown operand type");
  }
}

// The first register of a GPRQ tuple: what lq and sq name.
static MCRegister quadFirst(MCRegister Reg, const AsmPrinter &AP) {
  const MCRegisterInfo *MRI = AP.OutContext.getRegisterInfo();
  return MRI->getSubReg(Reg, GPUVM::sub0);
}

void llvm::lowerGPUVMMachineInstrToMCInst(const MachineInstr *MI,
                                          MCInst &OutMI, AsmPrinter &AP) {
  switch (MI->getOpcode()) {
  case GPUVM::PseudoRET:
    OutMI.setOpcode(GPUVM::JR);
    OutMI.addOperand(MCOperand::createReg(GPUVM::R1));
    return;
  case GPUVM::PseudoTAIL:
  case GPUVM::PseudoTAILR: {
    OutMI.setOpcode(MI->getOpcode() == GPUVM::PseudoTAIL ? GPUVM::J
                                                         : GPUVM::JR);
    MCOperand MCOp;
    lowerGPUVMMachineOperandToMCOperand(MI->getOperand(0), MCOp, AP);
    OutMI.addOperand(MCOp);
    return;
  }
  default:
    break;
  }
  unsigned Opc = MI->getOpcode();
  bool Quad = false;
  if (Opc == GPUVM::LQ_Q) {
    Opc = GPUVM::LQ;
    Quad = true;
  } else if (Opc == GPUVM::SQ_Q || Opc == GPUVM::SQ_OP) {
    Opc = GPUVM::SQ;
    Quad = true;
  }
  OutMI.setOpcode(Opc);
  for (const MachineOperand &MO : MI->operands()) {
    MCOperand MCOp;
    if (!lowerGPUVMMachineOperandToMCOperand(MO, MCOp, AP))
      continue;
    if (Quad && MCOp.isReg() && OutMI.getNumOperands() == 0)
      MCOp = MCOperand::createReg(quadFirst(MCOp.getReg(), AP));
    OutMI.addOperand(MCOp);
  }
}

void GPUVMAsmPrinter::emitInstruction(const MachineInstr *MI) {
  MCInst I;
  lowerGPUVMMachineInstrToMCInst(MI, I, *this);
  EmitToStreamer(*OutStreamer, I);
}

void GPUVMAsmPrinter::printOperand(const MachineInstr *MI, unsigned OpNo,
                                   raw_ostream &OS) {
  const MachineOperand &MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    OS << GPUVMInstPrinter::getRegisterName(MO.getReg(), GPUVM::NoRegAltName);
    return;
  case MachineOperand::MO_Immediate:
    OS << MO.getImm();
    return;
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_ExternalSymbol:
  case MachineOperand::MO_BlockAddress:
  case MachineOperand::MO_MachineBasicBlock:
  case MachineOperand::MO_JumpTableIndex:
  case MachineOperand::MO_ConstantPoolIndex: {
    MCOperand MCOp;
    lowerGPUVMMachineOperandToMCOperand(MO, MCOp, *this);
    MAI.printExpr(OS, *MCOp.getExpr());
    return;
  }
  default:
    llvm_unreachable("gpu_vm: an inline asm operand of an unknown kind");
  }
}

bool GPUVMAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                      const char *ExtraCode, raw_ostream &OS) {
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0)
      return true;
    const MachineOperand &MO = MI->getOperand(OpNo);
    switch (ExtraCode[0]) {
    case 'z': // zero for 0
      if (MO.isImm() && MO.getImm() == 0) {
        OS << GPUVMInstPrinter::getRegisterName(GPUVM::R0, GPUVM::NoRegAltName);
        return false;
      }
      break;
    default:
      return AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, OS);
    }
  }
  printOperand(MI, OpNo, OS);
  return false;
}

// base, offset: off(base)
bool GPUVMAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                            unsigned OpNo,
                                            const char *ExtraCode,
                                            raw_ostream &OS) {
  if (ExtraCode && ExtraCode[0])
    return true;
  const MachineOperand &Base = MI->getOperand(OpNo);
  const MachineOperand &Off = MI->getOperand(OpNo + 1);
  if (!Base.isReg())
    return true;
  if (Off.isImm())
    OS << Off.getImm();
  else
    printOperand(MI, OpNo + 1, OS);
  OS << '(' << GPUVMInstPrinter::getRegisterName(Base.getReg(), GPUVM::NoRegAltName) << ')';
  return false;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUVMAsmPrinter() {
  RegisterAsmPrinter<GPUVMAsmPrinter> X(getTheGPUVMTarget());
}

static GPUVMTargetStreamer *targetStreamer(MCStreamer &S) {
  return static_cast<GPUVMTargetStreamer *>(S.getTargetStreamer());
}

// Labels of the code generator do not close bundles (debug info's would
// change the code); a block that is branched to closes the open one.
void GPUVMAsmPrinter::emitStartOfAsmFile(Module &M) {
  if (GPUVMTargetStreamer *TS = targetStreamer(*OutStreamer))
    TS->setSoftLabels(true);
}

void GPUVMAsmPrinter::emitFunctionEntryLabel() {
  AsmPrinter::emitFunctionEntryLabel();
  FnBase = OutContext.createTempSymbol("gvmfn");
  OutStreamer->emitLabel(FnBase);
}

void GPUVMAsmPrinter::emitBasicBlockStart(const MachineBasicBlock &MBB) {
  bool Labeled = MBB.isEHPad() || MBB.hasAddressTaken() ||
                 MBB.hasLabelMustBeEmitted() ||
                 !isBlockOnlyReachableByFallthrough(&MBB);
  if (Labeled)
    if (GPUVMTargetStreamer *TS = targetStreamer(*OutStreamer))
      TS->emitBlockStart();
  AsmPrinter::emitBasicBlockStart(MBB);
}

// A function whose last bundle has no control op (one ending in `break`, an
// abstract class's deleting destructor) leaves its bundle open: close it, so
// that the labels after the body (debug info's, the function's end) are placed
// in the section before debug info asks for them.
void GPUVMAsmPrinter::emitFunctionBodyEnd() {
  if (GPUVMTargetStreamer *TS = targetStreamer(*OutStreamer))
    TS->emitBlockStart();
}
