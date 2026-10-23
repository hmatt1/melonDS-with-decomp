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

#ifndef WB_INSTROPS_H
#define WB_INSTROPS_H

#include "types.h"

namespace melonDS::WB
{

// Data-flow summary of one instruction, used by data origin propagation.
// Base and offset registers of memory accesses are addresses, not data, and are not listed as sources.
struct InstrOps
{
    enum Kind : u8 { None, Alu, Load, Store, Swap, Branch, Swi };

    Kind kind = None;
    u16 srcMask = 0;        // data source registers of an ALU result
    u16 dstMask = 0;        // registers whose value is replaced (ALU result or loaded data)
    u16 addrMask = 0;       // base and offset registers that form the address of a memory access
    bool constResult = false; // ALU result is built only from immediates / PC / CPSR
    bool pcRelLoad = false;   // load from a literal pool (LDR Rd, [PC, #x])
    bool writesLR = false;
    u8 swiNum = 0;
    u8 nLoad = 0, nStore = 0;
    u8 loadRegs[17];        // destination register of each memory read, in access order
    u8 storeRegs[17];       // source register of each memory write, in access order
};

void DecodeOps(u32 instr, bool thumb, InstrOps& ops);

// Returns true for the second half of a Thumb BL/BLX pair.
inline bool IsThumbBLSuffix(u16 instr) { return (instr & 0xF800) == 0xF800 || (instr & 0xF800) == 0xE800; }

// SWI number of an instruction, or -1 if it is not a SWI.
inline int SwiNumber(u32 instr, bool thumb)
{
    if (thumb) return ((instr & 0xFF00) == 0xDF00) ? (int)(instr & 0xFF) : -1;
    return ((instr & 0x0F000000) == 0x0F000000) ? (int)((instr >> 16) & 0xFF) : -1;
}

}

#endif
