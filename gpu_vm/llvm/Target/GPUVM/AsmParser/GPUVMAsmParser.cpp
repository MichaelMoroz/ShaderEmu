//===-- GPUVMAsmParser.cpp - Parse gpu_vm assembly to MCInsts -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The syntax is gpu_vm/llvm/ASM.md. Each statement is one op or control op;
// the object streamer packs them into bundles. `;;` after a statement and
// `.endbundle` close the open bundle.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMELFStreamer.h"
#include "MCTargetDesc/GPUVMFixupKinds.h"
#include "MCTargetDesc/GPUVMInstPrinter.h"
#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "TargetInfo/GPUVMTargetInfo.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Casting.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-asm-parser"

namespace {
struct GPUVMOperand;

class GPUVMAsmParser : public MCTargetAsmParser {
  // `;;` ended the statement: close the bundle after it.
  bool CloseAfter = false;

  SMLoc getLoc() const { return getParser().getTok().getLoc(); }

  GPUVMTargetStreamer &getTargetStreamer() {
    MCTargetStreamer &TS = *getParser().getStreamer().getTargetStreamer();
    return static_cast<GPUVMTargetStreamer &>(TS);
  }

  ParseStatus parseDirective(AsmToken DirectiveID) override;
  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;
  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;
  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

#define GET_ASSEMBLER_HEADER
#include "GPUVMGenAsmMatcher.inc"

  MCRegister matchRegister(StringRef Name) const;
  bool parseOperand(OperandVector &Operands, StringRef Mnemonic);
  bool parseImmExpr(const MCExpr *&E, SMLoc &End);
  ParseStatus parseFImm(OperandVector &Operands);
  ParseStatus parseSR(OperandVector &Operands);

public:
  enum GPUVMMatchResultTy {
    Match_Dummy = FIRST_TARGET_MATCH_RESULT_TY,
#define GET_OPERAND_DIAGNOSTIC_TYPES
#include "GPUVMGenAsmMatcher.inc"
#undef GET_OPERAND_DIAGNOSTIC_TYPES
  };

  GPUVMAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                 const MCInstrInfo &MII)
      : MCTargetAsmParser(STI, MII) {
    Parser.addAliasForDirective(".half", ".2byte");
    Parser.addAliasForDirective(".hword", ".2byte");
    Parser.addAliasForDirective(".word", ".4byte");
    Parser.addAliasForDirective(".dword", ".8byte");
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

struct GPUVMOperand : public MCParsedAsmOperand {
  enum KindTy { Token, Register, Immediate } Kind;
  SMLoc StartLoc, EndLoc;
  StringRef Tok;
  MCRegister Reg;
  const MCExpr *Imm = nullptr;

  GPUVMOperand(KindTy K) : Kind(K) {}

  bool isToken() const override { return Kind == Token; }
  bool isReg() const override { return Kind == Register; }
  bool isImm() const override { return Kind == Immediate; }
  bool isMem() const override { return false; }
  MCRegister getReg() const override { return Reg; }
  StringRef getToken() const { return Tok; }
  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  bool constant(int64_t &V) const {
    if (!isImm())
      return false;
    if (auto *CE = dyn_cast<MCConstantExpr>(Imm)) {
      V = CE->getValue();
      return true;
    }
    return false;
  }
  bool constIn(int64_t Lo, int64_t Hi) const {
    int64_t V;
    return constant(V) && V >= Lo && V <= Hi;
  }

  // Any 32-bit value, or a symbol's expression (a long immediate).
  bool isImm32() const {
    int64_t V;
    if (!isImm())
      return false;
    if (constant(V))
      return V >= INT32_MIN && V <= UINT32_MAX;
    return true;
  }
  bool isFImm() const { return isImm32(); }
  bool isSImm6() const { return constIn(-32, 31); }
  bool isUImm5() const { return constIn(0, 31); }
  bool isLen32() const { return constIn(1, 32); }
  bool isSImm12() const { return constIn(-2048, 2047); }
  bool isSR() const { return constIn(0, 4095); }
  bool isBrTarget() const {
    int64_t V;
    if (!isImm())
      return false;
    if (constant(V))
      return (V & 15) == 0 && isInt<32>(V);
    return true;
  }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    Inst.addOperand(MCOperand::createReg(Reg));
  }
  void addImmOperands(MCInst &Inst, unsigned N) const {
    int64_t V;
    if (constant(V))
      Inst.addOperand(MCOperand::createImm(V));
    else
      Inst.addOperand(MCOperand::createExpr(Imm));
  }

  void print(raw_ostream &OS, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case Token:
      OS << "'" << Tok << "'";
      break;
    case Register:
      OS << "<register " << Reg.id() << ">";
      break;
    case Immediate:
      OS << "<imm ";
      MAI.printExpr(OS, *Imm);
      OS << ">";
      break;
    }
  }

  static std::unique_ptr<GPUVMOperand> createToken(StringRef S, SMLoc L) {
    auto Op = std::make_unique<GPUVMOperand>(Token);
    Op->Tok = S;
    Op->StartLoc = Op->EndLoc = L;
    return Op;
  }
  static std::unique_ptr<GPUVMOperand> createReg(MCRegister R, SMLoc S,
                                                 SMLoc E) {
    auto Op = std::make_unique<GPUVMOperand>(Register);
    Op->Reg = R;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }
  static std::unique_ptr<GPUVMOperand> createImm(const MCExpr *V, SMLoc S,
                                                 SMLoc E) {
    auto Op = std::make_unique<GPUVMOperand>(Immediate);
    Op->Imm = V;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }
};
} // namespace

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#define GET_MNEMONIC_SPELL_CHECKER
#include "GPUVMGenAsmMatcher.inc"

MCRegister GPUVMAsmParser::matchRegister(StringRef Name) const {
  std::string L = Name.lower();
  MCRegister R = MatchRegisterName(L);
  if (!R)
    R = MatchRegisterAltName(L);
  if (!R && L == "fp")
    R = GPUVM::R32;
  return R;
}

bool GPUVMAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                   SMLoc &EndLoc) {
  if (!tryParseRegister(Reg, StartLoc, EndLoc).isSuccess())
    return Error(StartLoc, "invalid register name");
  return false;
}

ParseStatus GPUVMAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                             SMLoc &EndLoc) {
  const AsmToken &Tok = getParser().getTok();
  StartLoc = Tok.getLoc();
  EndLoc = Tok.getEndLoc();
  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;
  Reg = matchRegister(Tok.getIdentifier());
  if (!Reg)
    return ParseStatus::NoMatch;
  getParser().Lex();
  return ParseStatus::Success;
}

// An immediate's expression; `%tprel(expr)` is TLS's offset, `%fnrel(sym -
// fn)` an address from fn, a symbol of the same section (PIC's globals).
bool GPUVMAsmParser::parseImmExpr(const MCExpr *&E, SMLoc &End) {
  MCAsmParser &P = getParser();
  if (P.getTok().is(AsmToken::Percent)) {
    SMLoc L = getLoc();
    P.Lex();
    if (P.getTok().isNot(AsmToken::Identifier) ||
        (P.getTok().getIdentifier() != "tprel" &&
         P.getTok().getIdentifier() != "fnrel"))
      return Error(L, "unknown operator: only %tprel and %fnrel");
    auto Spec = P.getTok().getIdentifier() == "tprel" ? GPUVM::S_TPREL
                                                      : GPUVM::S_FNREL;
    P.Lex();
    if (P.getTok().isNot(AsmToken::LParen))
      return Error(getLoc(), "expected '('");
    P.Lex();
    const MCExpr *Sub;
    if (P.parseExpression(Sub))
      return true;
    if (P.getTok().isNot(AsmToken::RParen))
      return Error(getLoc(), "expected ')'");
    End = P.getTok().getEndLoc();
    P.Lex();
    E = MCSpecifierExpr::create(Sub, Spec, getContext(), L);
    return false;
  }
  return P.parseExpression(E, End);
}

bool GPUVMAsmParser::parseOperand(OperandVector &Operands,
                                  StringRef Mnemonic) {
  ParseStatus Res = MatchOperandParserImpl(Operands, Mnemonic);
  if (Res.isSuccess())
    return false;
  if (Res.isFailure())
    return true;

  MCAsmParser &P = getParser();
  MCRegister Reg;
  SMLoc S, E;
  if (tryParseRegister(Reg, S, E).isSuccess()) {
    Operands.push_back(GPUVMOperand::createReg(Reg, S, E));
    return false;
  }

  // imm(reg), (reg), or imm; an imm may start with '(' too: `(1 << 4)`
  bool BaseOnly = false;
  if (P.getTok().is(AsmToken::LParen)) {
    AsmToken Next = getLexer().peekTok();
    BaseOnly =
        Next.is(AsmToken::Identifier) && matchRegister(Next.getIdentifier());
  }
  if (!BaseOnly) {
    S = getLoc();
    const MCExpr *Ex;
    if (parseImmExpr(Ex, E))
      return true;
    Operands.push_back(GPUVMOperand::createImm(Ex, S, E));
    if (P.getTok().isNot(AsmToken::LParen))
      return false;
  }
  Operands.push_back(GPUVMOperand::createToken("(", getLoc()));
  P.Lex();
  if (!tryParseRegister(Reg, S, E).isSuccess())
    return Error(getLoc(), "expected a register");
  Operands.push_back(GPUVMOperand::createReg(Reg, S, E));
  if (P.getTok().isNot(AsmToken::RParen))
    return Error(getLoc(), "expected ')'");
  Operands.push_back(GPUVMOperand::createToken(")", getLoc()));
  P.Lex();
  return false;
}

// A float literal (1.5, -2e3, inf, nan) as its bits; anything else is an
// integer (the bits) or an expression, parsed as any immediate.
ParseStatus GPUVMAsmParser::parseFImm(OperandVector &Operands) {
  MCAsmParser &P = getParser();
  SMLoc S = getLoc();
  bool Neg = false;
  AsmToken Tok = P.getTok();
  if (Tok.is(AsmToken::Minus)) {
    AsmToken Next = getLexer().peekTok();
    if (Next.isNot(AsmToken::Real) && Next.isNot(AsmToken::Identifier))
      return ParseStatus::NoMatch;
    if (Next.is(AsmToken::Identifier) && Next.getIdentifier() != "inf" &&
        Next.getIdentifier() != "nan")
      return ParseStatus::NoMatch;
    Neg = true;
    Tok = Next;
  }
  APFloat F(APFloat::IEEEsingle());
  if (Tok.is(AsmToken::Real)) {
    auto St = F.convertFromString(Tok.getString(),
                                  APFloat::rmNearestTiesToEven);
    if (!St)
      return Error(S, "invalid float");
  } else if (Tok.is(AsmToken::Identifier) && Tok.getIdentifier() == "inf") {
    F = APFloat::getInf(APFloat::IEEEsingle());
  } else if (Tok.is(AsmToken::Identifier) && Tok.getIdentifier() == "nan") {
    F = APFloat::getQNaN(APFloat::IEEEsingle());
  } else {
    return ParseStatus::NoMatch;
  }
  if (Neg) {
    F.changeSign();
    P.Lex();
  }
  SMLoc E = P.getTok().getEndLoc();
  P.Lex();
  uint64_t Bits = F.bitcastToAPInt().getZExtValue();
  Operands.push_back(GPUVMOperand::createImm(
      MCConstantExpr::create(Bits, getContext()), S, E));
  return ParseStatus::Success;
}

// A special register's name; a number is parsed as any immediate.
ParseStatus GPUVMAsmParser::parseSR(OperandVector &Operands) {
  const AsmToken &Tok = getParser().getTok();
  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;
  std::string Name = Tok.getIdentifier().lower();
  for (unsigned N = 0;; ++N) {
    const char *SR = GPUVMInstPrinter::getSRName(N);
    if (!SR)
      return ParseStatus::NoMatch;
    if (Name == SR) {
      SMLoc S = Tok.getLoc(), E = Tok.getEndLoc();
      getParser().Lex();
      Operands.push_back(GPUVMOperand::createImm(
          MCConstantExpr::create(N, getContext()), S, E));
      return ParseStatus::Success;
    }
  }
}

bool GPUVMAsmParser::parseInstruction(ParseInstructionInfo &Info,
                                      StringRef Name, SMLoc NameLoc,
                                      OperandVector &Operands) {
  MCAsmParser &P = getParser();
  Operands.push_back(GPUVMOperand::createToken(Name, NameLoc));
  if (P.getTok().isNot(AsmToken::EndOfStatement)) {
    if (parseOperand(Operands, Name))
      return true;
    while (P.getTok().is(AsmToken::Comma)) {
      P.Lex();
      if (parseOperand(Operands, Name))
        return true;
    }
  }
  if (P.getTok().isNot(AsmToken::EndOfStatement))
    return Error(getLoc(), "unexpected token");
  // `;;`: two statement separators in a row.
  if (P.getTok().getString() == ";") {
    AsmToken Next = getLexer().peekTok();
    if (Next.is(AsmToken::EndOfStatement) && Next.getString() == ";")
      CloseAfter = true;
  }
  P.Lex();
  return false;
}

ParseStatus GPUVMAsmParser::parseDirective(AsmToken DirectiveID) {
  if (DirectiveID.getString() == ".endbundle") {
    if (parseEOL())
      return ParseStatus::Failure;
    getTargetStreamer().emitBundleEnd();
    return ParseStatus::Success;
  }
  return ParseStatus::NoMatch;
}

static const char *diagnostic(unsigned Code) {
  switch (Code) {
  case GPUVMAsmParser::Match_InvalidImm32:
    return "immediate must be a 32-bit value or a symbol";
  case GPUVMAsmParser::Match_InvalidFImm:
    return "expected a float or its bits";
  case GPUVMAsmParser::Match_InvalidSImm6:
    return "immediate must be an integer in the range [-32, 31]";
  case GPUVMAsmParser::Match_InvalidUImm5:
    return "immediate must be an integer in the range [0, 31]";
  case GPUVMAsmParser::Match_InvalidLen32:
    return "length must be an integer in the range [1, 32]";
  case GPUVMAsmParser::Match_InvalidSImm12:
    return "immediate must be an integer in the range [-2048, 2047]";
  case GPUVMAsmParser::Match_InvalidSR:
    return "expected a special register (a name or 0-4095)";
  case GPUVMAsmParser::Match_InvalidBrTarget:
    return "branch target must be a label or a multiple of 16";
  }
  return nullptr;
}

bool GPUVMAsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                             OperandVector &Operands,
                                             MCStreamer &Out,
                                             uint64_t &ErrorInfo,
                                             bool MatchingInlineAsm) {
  MCInst Inst;
  bool Close = CloseAfter;
  CloseAfter = false;
  unsigned Result =
      MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm);
  switch (Result) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, getSTI());
    if (Close)
      getTargetStreamer().emitBundleEnd();
    return false;
  case Match_MissingFeature:
    return Error(IDLoc, "instruction requires a feature not enabled");
  case Match_MnemonicFail: {
    FeatureBitset FBS = ComputeAvailableFeatures(getSTI().getFeatureBits());
    std::string Suggestion = GPUVMMnemonicSpellCheck(
        ((GPUVMOperand &)*Operands[0]).getToken(), FBS, 0);
    return Error(IDLoc, "unrecognized instruction mnemonic" + Suggestion);
  }
  case Match_InvalidOperand: {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0ULL) {
      if (ErrorInfo >= Operands.size())
        return Error(ErrorLoc, "too few operands for instruction");
      ErrorLoc = ((GPUVMOperand &)*Operands[ErrorInfo]).getStartLoc();
      if (ErrorLoc == SMLoc())
        ErrorLoc = IDLoc;
    }
    return Error(ErrorLoc, "invalid operand for instruction");
  }
  default:
    break;
  }
  if (const char *Msg = diagnostic(Result)) {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0ULL && ErrorInfo < Operands.size())
      ErrorLoc = ((GPUVMOperand &)*Operands[ErrorInfo]).getStartLoc();
    return Error(ErrorLoc, Msg);
  }
  return Error(IDLoc, "invalid instruction");
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUVMAsmParser() {
  RegisterMCAsmParser<GPUVMAsmParser> X(getTheGPUVMTarget());
}
