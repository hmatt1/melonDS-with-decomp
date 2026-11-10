/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef WB_DISASM_H
#define WB_DISASM_H

#include <string>
#include "types.h"

namespace melonDS::WB
{

struct DisasmInsn
{
    u32 Addr = 0;
    u32 Size = 4;           // 2 or 4 (a Thumb BL pair is 4)
    bool Thumb = false;
    std::string Mnemonic;
    std::string Operands;
    bool HasTarget = false; // branch target
    u32 Target = 0;
    bool TargetThumb = false;
    bool HasRef = false;    // referenced address (literal pool slot, ADR)
    u32 Ref = 0;
    bool LiteralLoad = false;   // Ref is a literal pool word that holds the loaded value
    bool IsCall = false;
    bool IsBranch = false;
    bool IsReturn = false;
    bool Conditional = false;
    bool Valid = true;
    u16 RegMask = 0;        // registers named in the operands

    std::string Text() const { return Operands.empty() ? Mnemonic : Mnemonic + " " + Operands; }
};

// instr: ARM: the 32-bit word. Thumb: halfword at addr in the low 16 bits and the following halfword in the high 16 bits.
void Disassemble(u32 addr, u32 instr, bool thumb, DisasmInsn& out);

// ARM: stmfd/push of lr, or str lr, [sp, #-4]!. Thumb: push including lr. The instruction must have executed.
bool IsFunctionPrologue(u32 instr, bool thumb);

// True when this instruction loads a constant from a literal pool (the recorder's fast path).
bool LoadsLiteralPool(u32 instr, bool thumb);

// One sentence for this decoded instruction, using the registers, immediates and addressing it actually uses.
std::string ExplainInsn(const DisasmInsn& in);

// A C starting point for one instruction. Unknown encodings stay as a comment.
std::string InsnAsC(const DisasmInsn& in);

std::string RegName(int r);
std::string Hex(u32 v);

}

#endif
