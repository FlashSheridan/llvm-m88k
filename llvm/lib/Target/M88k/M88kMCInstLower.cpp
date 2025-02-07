//===-- M88kMCInstLower.cpp - Lower MachineInstr to MCInst ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "M88kMCInstLower.h"
#include "M88kRegisterInfo.h"
#include "MCTargetDesc/M88kBaseInfo.h"
#include "MCTargetDesc/M88kMCExpr.h"
#include "MCTargetDesc/M88kMCTargetDesc.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Mangler.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"

using namespace llvm;

M88kMCInstLower::M88kMCInstLower(MCContext &Ctx, AsmPrinter &Printer)
    : Ctx(Ctx), Printer(Printer) {}

MCOperand M88kMCInstLower::lowerSymbolOperand(const MachineOperand &MO) const {
  MCSymbolRefExpr::VariantKind Kind = MCSymbolRefExpr::VK_None;
  M88kMCExpr::VariantKind TargetKind = M88kMCExpr::VK_None;
  const MCSymbol *Symbol;
  bool HasOffset = true;

  switch (MO.getTargetFlags()) {
  default:
    llvm_unreachable("Invalid target flag!");
  case M88kII::MO_NO_FLAG:
    break;
  case M88kII::MO_ABS_HI:
    TargetKind = M88kMCExpr::VK_ABS_HI;
    break;
  case M88kII::MO_ABS_LO:
    TargetKind = M88kMCExpr::VK_ABS_LO;
    break;
  }

  switch (MO.getType()) {
  case MachineOperand::MO_MachineBasicBlock:
    Symbol = MO.getMBB()->getSymbol();
    HasOffset = false;
    break;

  case MachineOperand::MO_GlobalAddress:
    Symbol = Printer.getSymbol(MO.getGlobal());
    break;

  case MachineOperand::MO_ExternalSymbol:
    Symbol = Printer.GetExternalSymbolSymbol(MO.getSymbolName());
    break;

  case MachineOperand::MO_JumpTableIndex:
    Symbol = Printer.GetJTISymbol(MO.getIndex());
    HasOffset = false;
    break;

  case MachineOperand::MO_ConstantPoolIndex:
    Symbol = Printer.GetCPISymbol(MO.getIndex());
    break;

  case MachineOperand::MO_BlockAddress:
    Symbol = Printer.GetBlockAddressSymbol(MO.getBlockAddress());
    break;

  default:
    llvm_unreachable("unknown operand type");
  }
  const MCExpr *Expr = MCSymbolRefExpr::create(Symbol, Kind, Ctx);
  if (HasOffset) {
    if (int64_t Offset = MO.getOffset()) {
      const MCExpr *OffsetExpr = MCConstantExpr::create(Offset, Ctx);
      Expr = MCBinaryExpr::createAdd(Expr, OffsetExpr, Ctx);
    }
  }
  if (TargetKind)
    Expr = M88kMCExpr::create(TargetKind, Expr, Ctx);
  return MCOperand::createExpr(Expr);
}

MCOperand M88kMCInstLower::lowerOperand(const MachineOperand &MO, const TargetRegisterInfo *TRI) const {
  switch (MO.getType()) {
  case MachineOperand::MO_Register: {
    // HACK: If a register pair is used then replace register with the hi part.
    Register Reg = MO.getReg();
    assert(Register::isPhysicalRegister(Reg));
    assert(!MO.getSubReg() && "Subregs should be eliminated!");
    if(M88k::GPR64RegClass.contains(Reg))
      Reg = TRI->getSubReg(Reg, M88k::sub_hi);
    return MCOperand::createReg(Reg);
  }

  case MachineOperand::MO_Immediate:
    return MCOperand::createImm(MO.getImm());

  case MachineOperand::MO_MachineBasicBlock:
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_ExternalSymbol:
  case MachineOperand::MO_MCSymbol:
  case MachineOperand::MO_JumpTableIndex:
  case MachineOperand::MO_ConstantPoolIndex:
  case MachineOperand::MO_BlockAddress:
    return lowerSymbolOperand(MO);
  default:
    report_fatal_error("Unexpected MachineOperand type.");
  }
}

void M88kMCInstLower::lower(const MachineInstr *MI, MCInst &OutMI) const {
  const MachineFunction &MF = *MI->getParent()->getParent();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  OutMI.setOpcode(MI->getOpcode());
  for (const auto &MO : MI->operands()) {
    // Ignore all implicit register operands, and register masks.
    if ((!MO.isReg() || !MO.isImplicit()) && !MO.isRegMask())
      OutMI.addOperand(lowerOperand(MO, TRI));
  }
}
