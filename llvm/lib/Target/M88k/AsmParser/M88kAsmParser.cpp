//===-- M88kAsmParser.cpp - Parse M88k assembly to MCInst instructions ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/M88kInstPrinter.h"
#include "MCTargetDesc/M88kMCTargetDesc.h"
#include "MCTargetDesc/M88kTargetStreamer.h"
#include "TargetInfo/M88kTargetInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCAsmMacro.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/MCAsmLexer.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

using namespace llvm;

namespace {

// Return true if Expr is in the range [MinValue, MaxValue].
bool inRange(const MCExpr *Expr, int64_t MinValue, int64_t MaxValue) {
  if (auto *CE = dyn_cast<MCConstantExpr>(Expr)) {
    int64_t Value = CE->getValue();
    return Value >= MinValue && Value <= MaxValue;
  }
  return false;
}

// Instances of this class represented a parsed machine instruction
class M88kOperand : public MCParsedAsmOperand {
  enum OperandKind {
    // KindInvalid,
    OpKind_Token,
    OpKind_Reg,
    OpKind_Imm,
  };

  OperandKind Kind;
  SMLoc StartLoc, EndLoc;

  struct TokenOp {
    StringRef Token;
  };

  struct RegOp {
    unsigned RegNo;
  };

  union {
    TokenOp Tok;
    RegOp Reg;
    const MCExpr *Imm;
  };

  void addExpr(MCInst &Inst, const MCExpr *Expr) const {
    // Add as immediates when possible.  Null MCExpr = 0.
    if (!Expr)
      Inst.addOperand(MCOperand::createImm(0));
    else if (auto *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Expr));
  }

public:
  M88kOperand(OperandKind Kind, SMLoc StartLoc, SMLoc EndLoc)
      : Kind(Kind), StartLoc(StartLoc), EndLoc(EndLoc) {}

  // getStartLoc - Gets location of the first token of this operand
  SMLoc getStartLoc() const override { return StartLoc; }

  // getEndLoc - Gets location of the last token of this operand
  SMLoc getEndLoc() const override { return EndLoc; }

  bool isReg() const override { return Kind == OpKind_Reg; }

  MCRegister getReg() const override {
    assert(isReg() && "Invalid type access!");
    return Reg.RegNo;
  }

  bool isImm() const override { return Kind == OpKind_Imm; }

  bool isImm(int64_t MinValue, int64_t MaxValue) const {
    return Kind == OpKind_Imm && inRange(Imm, MinValue, MaxValue);
  }

  const MCExpr *getImm() const {
    assert(isImm() && "Invalid type access!");
    return Imm;
  }

  bool isToken() const override { return Kind == OpKind_Token; }

  StringRef getToken() const {
    assert(isToken() && "Not a token");
    return Tok.Token;
  }

  bool isMem() const override { return false; }

  static std::unique_ptr<M88kOperand> createToken(StringRef Str, SMLoc Loc) {
    auto Op = std::make_unique<M88kOperand>(OpKind_Token, Loc, Loc);
    Op->Tok.Token = Str;
    return Op;
  }

  static std::unique_ptr<M88kOperand> createReg(unsigned Num, SMLoc StartLoc,
                                                SMLoc EndLoc) {
    auto Op = std::make_unique<M88kOperand>(OpKind_Reg, StartLoc, EndLoc);
    Op->Reg.RegNo = Num;
    return Op;
  }

  static std::unique_ptr<M88kOperand> createImm(const MCExpr *Expr,
                                                SMLoc StartLoc, SMLoc EndLoc) {
    auto Op = std::make_unique<M88kOperand>(OpKind_Imm, StartLoc, EndLoc);
    Op->Imm = Expr;
    return Op;
  }

  // Used by the TableGen code to add particular types of operand
  // to an instruction.
  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    Inst.addOperand(MCOperand::createReg(getReg()));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    addExpr(Inst, getImm());
  }

  void addBFWidthOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    addExpr(Inst, getImm());
  }

  void addBFOffsetOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    addExpr(Inst, getImm());
  }

  void addPixelRotOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    addExpr(Inst, getImm());
  }

  void addConditionCodeOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    addExpr(Inst, getImm());
  }

  bool isU5Imm() const { return isImm(0, 31); }
  bool isU16Imm() const { return isImm(0, 65535); }
  bool isS16Imm() const { return isImm(-32768, 32767); }
  bool isVec9() const { return isImm(0, 511); }

  bool isBFWidth() const { return isImm(0, 31); }
  bool isBFOffset() const { return isImm(0, 31); }
  bool isPixelRot() const { return isImm(0, 60); }
  bool isCCode() const { return isImm(0, 31); }

  void print(raw_ostream &OS) const override {
    switch (Kind) {
    case OpKind_Imm:
      OS << "Imm: " << getImm() << "\n";
      break;
    case OpKind_Token:
      OS << "Token: " << getToken() << "\n";
      break;
    case OpKind_Reg:
      OS << "Reg: " << M88kInstPrinter::getRegisterName(getReg()) << "\n";
      break;
    }
  }
};

class M88kAsmParser : public MCTargetAsmParser {
// Auto-generated instruction matching functions
#define GET_ASSEMBLER_HEADER
#include "M88kGenAsmMatcher.inc"

  M88kTargetStreamer &getTargetStreamer() {
    assert(getParser().getStreamer().getTargetStreamer() &&
           "m88k - asm parser does not have a target streamer");
    MCTargetStreamer &TS = *getParser().getStreamer().getTargetStreamer();
    return static_cast<M88kTargetStreamer &>(TS);
  }

  bool ParseDirective(AsmToken DirectiveID) override;
  bool ParseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;
  bool parseRegister(MCRegister &RegNo, SMLoc &StartLoc,
                     SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &RegNo, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;
  unsigned validateTargetOperandClass(MCParsedAsmOperand &Op,
                                      unsigned Kind) override;

  bool parseRegister(MCRegister &RegNo, SMLoc &StartLoc, SMLoc &EndLoc,
                     bool RestoreOnFailure);
  bool parseOperand(OperandVector &Operands, StringRef Mnemonic);
  bool parseScaledRegister(OperandVector &Operands);

  ParseStatus parseBFWidth(OperandVector &Operands);
  ParseStatus parseBFOffset(OperandVector &Operands);
  ParseStatus parsePixelRot(OperandVector &Operands);
  ParseStatus parseConditionCode(OperandVector &Operands);

  ParseStatus parsePCRel(OperandVector &Operands, unsigned Bits);

  ParseStatus parsePCRel16(OperandVector &Operands) {
    return parsePCRel(Operands, 18);
  }

  ParseStatus parsePCRel26(OperandVector &Operands) {
    return parsePCRel(Operands, 28);
  }

  bool MatchAndEmitInstruction(SMLoc IdLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

public:
  enum M88kMatchResultTy {
    Match_Dummy = FIRST_TARGET_MATCH_RESULT_TY,
#define GET_OPERAND_DIAGNOSTIC_TYPES
#include "M88kGenAsmMatcher.inc"
  };

  M88kAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(Parser),
        Lexer(Parser.getLexer()), SubtargetInfo(STI) {
    setAvailableFeatures(
        ComputeAvailableFeatures(SubtargetInfo.getFeatureBits()));
  }

private:
  MCAsmParser &Parser;
  MCAsmLexer &Lexer;

  const MCSubtargetInfo &SubtargetInfo;
};

} // end anonymous namespace

#define GET_REGISTER_MATCHER
#define GET_SUBTARGET_FEATURE_NAME
#define GET_MATCHER_IMPLEMENTATION
#define GET_MNEMONIC_SPELL_CHECKER
#include "M88kGenAsmMatcher.inc"

unsigned M88kAsmParser::validateTargetOperandClass(MCParsedAsmOperand &AsmOp,
                                                   unsigned Kind) {
  if (Kind == MCK_GPR64 && AsmOp.isReg()) {
    switch (AsmOp.getReg()) {
    case M88k::R0:
    case M88k::R2:
    case M88k::R4:
    case M88k::R6:
    case M88k::R8:
    case M88k::R10:
    case M88k::R12:
    case M88k::R14:
    case M88k::R16:
    case M88k::R18:
    case M88k::R20:
    case M88k::R22:
    case M88k::R24:
    case M88k::R26:
    case M88k::R28:
    case M88k::R30:
      return Match_Success;
    case M88k::R1:
    case M88k::R3:
    case M88k::R5:
    case M88k::R7:
    case M88k::R9:
    case M88k::R11:
    case M88k::R13:
    case M88k::R15:
    case M88k::R17:
    case M88k::R19:
    case M88k::R21:
    case M88k::R23:
    case M88k::R25:
    case M88k::R27:
    case M88k::R29:
    case M88k::R31:
      // TODO Add option to flag odd registers.
      return Match_Success;
    default:
      return Match_InvalidOperand;
    }
  }

  return Match_InvalidOperand;
}

bool M88kAsmParser::ParseDirective(AsmToken DirectiveID) {
  StringRef IDVal = DirectiveID.getIdentifier();

  if (IDVal == ".requires_88110") {
    MCSubtargetInfo &STI = copySTI();
    STI.setDefaultFeatures(/*CPU*/ "mc88110", /*TuneCPU*/ "mc88110", "");
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
    getTargetStreamer().emitDirectiveRequires881100();
    return false;
  }

  return true;
}

bool M88kAsmParser::ParseInstruction(ParseInstructionInfo &Info, StringRef Name,
                                     SMLoc NameLoc, OperandVector &Operands) {
  // First operand in MCInst is instruction mnemonic.
  Operands.push_back(M88kOperand::createToken(Name, NameLoc));

  // Read the remaining operands.
  if (getLexer().isNot(AsmToken::EndOfStatement)) {

    // Read the first operand.
    if (parseOperand(Operands, Name)) {
      return Error(getLexer().getLoc(), "expected operand");
    }

    // Read the second operand.
    if (getLexer().is(AsmToken::Comma)) {
      Parser.Lex();
      if (parseOperand(Operands, Name)) {
        return Error(getLexer().getLoc(), "expected operand");
      }

      // Read the third operand or a scaled register.
      if (getLexer().is(AsmToken::Comma)) {
        Parser.Lex();
        if (getLexer().is(AsmToken::Less) && Name == "rot")
          Operands.push_back(
              M88kOperand::createToken("<", Parser.getTok().getLoc()));

        if (parseOperand(Operands, Name)) {
          return Error(getLexer().getLoc(), "expected register or immediate");
        }
        // Parse bitfield width
        if (getLexer().is(AsmToken::Less)) {
          Operands.push_back(
              M88kOperand::createToken("<", Parser.getTok().getLoc()));
          if (parseOperand(Operands, Name)) {
            return Error(getLexer().getLoc(), "expected bitfield offset");
          }
        }
      } else if (getLexer().is(AsmToken::LBrac)) {
        if (parseScaledRegister(Operands))
          return Error(getLexer().getLoc(), "expected scaled register operand");
      }
    }

    if (getLexer().isNot(AsmToken::EndOfStatement))
      return Error(getLexer().getLoc(), "unexpected token in argument list");
  }

  // Consume the EndOfStatement.
  Parser.Lex();
  return false;
}

bool M88kAsmParser::parseOperand(OperandVector &Operands, StringRef Mnemonic) {
  // Invoke a custom associated parser.
  ParseStatus Result = MatchOperandParserImpl(Operands, Mnemonic);

  if (Result.isSuccess()) {
    return false;
  }
  if (Result.isFailure()) {
    Parser.eatToEndOfStatement();
    return true;
  }
  assert(Result.isNoMatch() && "Unexpected match result");

  // Check if it is a register.
  if (Lexer.is(AsmToken::Percent)) {
    MCRegister RegNo;
    SMLoc StartLoc, EndLoc;
    if (parseRegister(RegNo, StartLoc, EndLoc, /*RestoreOnFailure=*/false))
      return true;
    Operands.push_back(M88kOperand::createReg(RegNo, StartLoc, EndLoc));
    return false;
  }

  // Could be immediate or address.
  if (Lexer.is(AsmToken::Integer)) {
    SMLoc StartLoc = Parser.getTok().getLoc();
    const MCExpr *Expr;
    if (Parser.parseExpression(Expr))
      return true;
    SMLoc EndLoc = Parser.getTok().getLoc();
    Operands.push_back(M88kOperand::createImm(Expr, StartLoc, EndLoc));
    return false;
  }

  // Failure.
  return true;
}

ParseStatus M88kAsmParser::parseBFWidth(OperandVector &Operands) {
  // Parses the width of a bitfield. If empty and followed by <O>, then it is 0.
  // If not followed by <O>, then it is the offset, and the width is 0.
  MCContext &Ctx = getContext();
  SMLoc StartLoc = Parser.getTok().getLoc();
  bool HasWidth = false;
  int64_t Width = 0;
  bool IsReallyOffset = false;
  if (Lexer.is(AsmToken::Integer)) {
    Width = Parser.getTok().getIntVal();
    HasWidth = true;
    Parser.Lex();
  }
  if (Lexer.isNot(AsmToken::Less)) {
    if (!HasWidth)
      return ParseStatus::NoMatch;
    IsReallyOffset = true;
  }

  const MCExpr *Expr = MCConstantExpr::create(Width, Ctx);
  SMLoc EndLoc =
      SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);
  if (IsReallyOffset) {
    Operands.push_back(M88kOperand::createImm(MCConstantExpr::create(0, Ctx),
                                              StartLoc, EndLoc));
    Operands.push_back(M88kOperand::createToken("<", Parser.getTok().getLoc()));
    Operands.push_back(M88kOperand::createImm(Expr, StartLoc, EndLoc));
    Operands.push_back(M88kOperand::createToken(">", Parser.getTok().getLoc()));
  } else
    Operands.push_back(M88kOperand::createImm(Expr, StartLoc, EndLoc));

  // Announce match.
  return ParseStatus::Success;
}

ParseStatus M88kAsmParser::parseBFOffset(OperandVector &Operands) {
  // Parses operands like <7>.
  MCContext &Ctx = getContext();
  SMLoc StartLoc = Parser.getTok().getLoc();

  Parser.Lex();
  if (Lexer.isNot(AsmToken::Integer))
    return ParseStatus::Failure;
  int64_t Offset = Parser.getTok().getIntVal();
  Parser.Lex();
  if (Lexer.isNot(AsmToken::Greater))
    return ParseStatus::Failure;
  Parser.Lex();

  const MCExpr *Expr = MCConstantExpr::create(Offset, Ctx);
  SMLoc EndLoc =
      SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);
  Operands.push_back(M88kOperand::createImm(Expr, StartLoc, EndLoc));
  Operands.push_back(M88kOperand::createToken(">", Parser.getTok().getLoc()));

  // Announce match.
  return ParseStatus::Success;
}

ParseStatus M88kAsmParser::parsePixelRot(OperandVector &Operands) {
  // Parses operands like <7>.
  MCContext &Ctx = getContext();
  SMLoc StartLoc = Parser.getTok().getLoc();

  if (Lexer.isNot(AsmToken::Less)) {
    return ParseStatus::NoMatch;
  }
  Parser.Lex();
  if (Lexer.isNot(AsmToken::Integer))
    return ParseStatus::Failure;
  int64_t RotateSize = Parser.getTok().getIntVal();
  Parser.Lex();
  if (Lexer.isNot(AsmToken::Greater))
    return ParseStatus::Failure;
  Parser.Lex();

  if (RotateSize & 0x3) {
    Warning(StartLoc, "Removed lower 2 bits of expression");
    RotateSize &= ~0x3;
  }
  const MCExpr *Expr = MCConstantExpr::create(RotateSize, Ctx);
  SMLoc EndLoc =
      SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);
  Operands.push_back(M88kOperand::createImm(Expr, StartLoc, EndLoc));

  // Announce match.
  return ParseStatus::Success;
}

ParseStatus M88kAsmParser::parseConditionCode(OperandVector &Operands) {
  // Parses condition codes for brcond/tcond.
  SMLoc StartLoc = getLexer().getLoc();
  unsigned CC;
  if (Lexer.is(AsmToken::Integer)) {
    int64_t CCVal = Lexer.getTok().getIntVal();
    if (isUInt<5>(CCVal))
      return ParseStatus::NoMatch;
    CC = static_cast<unsigned>(CCVal);
  } else {
    CC = StringSwitch<unsigned>(Parser.getTok().getString())
             .Case("eq0", 0x2)
             .Case("ne0", 0xd)
             .Case("gt0", 0x1)
             .Case("lt0", 0xc)
             .Case("ge0", 0x3)
             .Case("le0", 0xe)
             .Default(0);
    if (CC == 0)
      return ParseStatus::NoMatch;
  }
  Parser.Lex();

  // Create expression.
  SMLoc EndLoc =
      SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);
  const MCExpr *CCExpr = MCConstantExpr::create(CC, getContext());
  Operands.push_back(M88kOperand::createImm(CCExpr, StartLoc, EndLoc));

  return ParseStatus::Success;
}

ParseStatus M88kAsmParser::parsePCRel(OperandVector &Operands, unsigned Bits) {
  const MCExpr *Expr;
  SMLoc StartLoc = Parser.getTok().getLoc();
  if (getParser().parseExpression(Expr))
    return ParseStatus::NoMatch;

  const int64_t MinVal = -(1LL << Bits);
  const int64_t MaxVal = (1LL << Bits) - 1;
  auto IsOutOfRangeConstant = [&](const MCExpr *E) -> bool {
    if (auto *CE = dyn_cast<MCConstantExpr>(E)) {
      int64_t Value = CE->getValue();
      if ((Value & 1) || Value < MinVal || Value > MaxVal)
        return true;
    }
    return false;
  };

  // For consistency with the GNU assembler, treat immediates as absolute
  // values. In this case, check only the range.
  if (auto *CE = dyn_cast<MCConstantExpr>(Expr)) {
    if (IsOutOfRangeConstant(CE)) {
      Error(StartLoc, "offset out of range");
      return ParseStatus::Failure;
    }
  }

  // For consistency with the GNU assembler, conservatively assume that a
  // constant offset must by itself be within the given size range.
  if (const auto *BE = dyn_cast<MCBinaryExpr>(Expr))
    if (IsOutOfRangeConstant(BE->getLHS()) ||
        IsOutOfRangeConstant(BE->getRHS())) {
      Error(StartLoc, "offset out of range");
      return ParseStatus::Failure;
    }

  SMLoc EndLoc =
      SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);

  Operands.push_back(M88kOperand::createImm(Expr, StartLoc, EndLoc));

  return ParseStatus::Success;
}

// Matches the normal register name, or the alternative register name.
static unsigned matchRegisterName(StringRef Name) {
  unsigned RegNo = MatchRegisterName(Name);
  if (RegNo == 0)
    RegNo = MatchRegisterAltName(Name);
  return RegNo;
}

// Parses register of form %(r|x|cr|fcr)<No>.
bool M88kAsmParser::parseRegister(MCRegister &RegNo, SMLoc &StartLoc,
                                  SMLoc &EndLoc, bool RestoreOnFailure) {
  StartLoc = Parser.getTok().getLoc();

  // Eat the '%' prefix.
  if (Parser.getTok().isNot(AsmToken::Percent))
    return true;
  const AsmToken &PercentTok = Parser.getTok();
  Parser.Lex();

  // Match the register.
  if (Lexer.getKind() != AsmToken::Identifier ||
      (RegNo = matchRegisterName(Lexer.getTok().getIdentifier())) == 0) {
    if (RestoreOnFailure)
      Lexer.UnLex(PercentTok);
    return Error(StartLoc, "invalid register");
  }

  Parser.Lex(); // Eat identifier token.
  EndLoc = Parser.getTok().getLoc();
  return false;
}

bool M88kAsmParser::parseScaledRegister(OperandVector &Operands) {
  SMLoc LBracketLoc = Parser.getTok().getLoc();

  // Eat the '[' bracket.
  if (Lexer.isNot(AsmToken::LBrac))
    return true;
  Parser.Lex();

  MCRegister RegNo;
  SMLoc StartLoc, EndLoc;
  if (parseRegister(RegNo, StartLoc, EndLoc, /*RestoreOnFailure=*/false))
    return true;

  // Eat the ']' bracket.
  if (Lexer.isNot(AsmToken::RBrac))
    return true;

  SMLoc RBracLoc = Parser.getTok().getLoc();
  Parser.Lex();

  Operands.push_back(M88kOperand::createToken("[", LBracketLoc));
  Operands.push_back(M88kOperand::createReg(RegNo, StartLoc, EndLoc));
  Operands.push_back(M88kOperand::createToken("]", RBracLoc));

  return false;
}

bool M88kAsmParser::parseRegister(MCRegister &RegNo, SMLoc &StartLoc,
                                  SMLoc &EndLoc) {
  return parseRegister(RegNo, StartLoc, EndLoc,
                       /*RestoreOnFailure=*/false);
}

ParseStatus M88kAsmParser::tryParseRegister(MCRegister &RegNo, SMLoc &StartLoc,
                                            SMLoc &EndLoc) {
  bool Result = parseRegister(RegNo, StartLoc, EndLoc,
                              /*RestoreOnFailure=*/true);
  bool PendingErrors = getParser().hasPendingError();
  getParser().clearPendingErrors();
  if (PendingErrors)
    return ParseStatus::Failure;
  if (Result)
    return ParseStatus::NoMatch;
  return ParseStatus::Success;
}

bool M88kAsmParser::MatchAndEmitInstruction(SMLoc IdLoc, unsigned &Opcode,
                                            OperandVector &Operands,
                                            MCStreamer &Out,
                                            uint64_t &ErrorInfo,
                                            bool MatchingInlineAsm) {
  MCInst Inst;
  FeatureBitset MissingFeatures;
  unsigned MatchResult = MatchInstructionImpl(
      Operands, Inst, ErrorInfo, MissingFeatures, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    Inst.setLoc(IdLoc);
    Out.emitInstruction(Inst, getSTI());
    return false;
  case Match_MissingFeature: {
    assert(MissingFeatures.any() && "Unknown missing features!");
    bool FirstFeature = true;
    std::string Msg = "instruction requires the following:";
    for (unsigned I = 0, E = MissingFeatures.size(); I != E; ++I) {
      if (MissingFeatures[I]) {
        Msg += FirstFeature ? " " : ", ";
        Msg += getSubtargetFeatureName(I);
        FirstFeature = false;
      }
    }
    return Error(IdLoc, Msg);
  }
  case Match_InvalidOperand: {
    SMLoc ErrorLoc = IdLoc;
    if (ErrorInfo != ~0U) {
      if (ErrorInfo >= Operands.size())
        return Error(IdLoc, "Too few operands for instruction");
      ErrorLoc = ((M88kOperand &)*Operands[ErrorInfo]).getStartLoc();
    }
    return Error(ErrorLoc, "Invalid operand for instruction");
  }
  case Match_MnemonicFail: {
    FeatureBitset FBS = ComputeAvailableFeatures(getSTI().getFeatureBits());
    M88kOperand &Op = static_cast<M88kOperand &>(*Operands[0]);
    std::string Suggestion = M88kMnemonicSpellCheck(Op.getToken(), FBS, 0);
    return Error(IdLoc, "invalid instruction" + Suggestion/*,
                 Op.getLocRange()*/);
  }
  }

  // Handle the case when the error message is of specific type other than the
  // generic Match_InvalidOperand, and the corresponding operand is missing.
  if (MatchResult > FIRST_TARGET_MATCH_RESULT_TY) {
    SMLoc ErrorLoc = IdLoc;
    if (ErrorInfo != ~0ULL && ErrorInfo >= Operands.size())
      return Error(ErrorLoc, "too few operands for instruction");
  }

  switch (MatchResult) {
  case Match_InvalidBitfieldWidth:
  case Match_InvalidBitfieldOffset:
  case Match_InvalidPixelRotationSize: {
    SMLoc ErrorLoc = ((M88kOperand &)*Operands[ErrorInfo]).getStartLoc();
    return Error(ErrorLoc, getMatchKindDiag((M88kMatchResultTy)MatchResult));
  }
  }

  llvm_unreachable("Unexpected match type");
}

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeM88kAsmParser() {
  RegisterMCAsmParser<M88kAsmParser> X(getTheM88kTarget());
}
