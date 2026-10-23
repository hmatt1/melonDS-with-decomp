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

#include "InstrOps.h"

namespace melonDS::WB
{

static inline void alu(InstrOps& o, int rd, u16 src)
{
    o.kind = InstrOps::Alu;
    o.srcMask = src & 0x7FFF;   // PC never carries a label
    if (rd >= 0 && rd < 15) o.dstMask = 1 << rd;
    o.constResult = (o.srcMask == 0);
}

static inline void load(InstrOps& o, int rd)
{
    o.kind = InstrOps::Load;
    o.loadRegs[o.nLoad++] = (u8)rd;
    if (rd < 15) o.dstMask |= 1 << rd;
}

static inline void store(InstrOps& o, int rd)
{
    o.kind = InstrOps::Store;
    o.storeRegs[o.nStore++] = (u8)rd;
}

static void DecodeARM(u32 i, InstrOps& o)
{
    u32 op = (i >> 25) & 0x7;
    int rd = (i >> 12) & 0xF;
    int rn = (i >> 16) & 0xF;
    int rm = i & 0xF;
    int rs = (i >> 8) & 0xF;

    if ((i & 0xF0000000) == 0xF0000000)
    {
        // unconditional space: BLX imm, PLD
        if ((i & 0x0E000000) == 0x0A000000) { o.kind = InstrOps::Branch; o.writesLR = true; o.dstMask = 1 << 14; o.constResult = true; }
        return;
    }

    switch (op)
    {
    case 0:
        if ((i & 0x0FFFFFD0) == 0x012FFF10) // BX / BLX reg
        {
            o.kind = InstrOps::Branch;
            if (i & 0x20) { o.writesLR = true; o.dstMask = 1 << 14; o.constResult = true; }
            return;
        }
        if ((i & 0x0FFF0FF0) == 0x016F0F10) { alu(o, rd, 1 << rm); return; } // CLZ
        if ((i & 0x0F900FF0) == 0x01000050) { alu(o, rd, (1 << rn) | (1 << rm)); return; } // QADD/QSUB/QDADD/QDSUB
        if ((i & 0x0FBF0FFF) == 0x010F0000) { alu(o, rd, 0); return; } // MRS
        if ((i & 0x0FB0FFF0) == 0x0120F000) { o.kind = InstrOps::None; return; } // MSR reg
        if ((i & 0x0F900090) == 0x01000080) // SMLAxy / SMLAWy / SMULWy / SMLALxy / SMULxy
        {
            u32 sub = (i >> 21) & 0x3;
            int rdh = (i >> 16) & 0xF, rdl = (i >> 12) & 0xF;
            if (sub == 2) // SMLALxy
            {
                alu(o, rdh, (1 << rm) | (1 << rs) | (1 << rdh) | (1 << rdl));
                if (rdl < 15) o.dstMask |= 1 << rdl;
            }
            else if (sub == 3) alu(o, rdh, (1 << rm) | (1 << rs)); // SMULxy
            else if (sub == 1 && (i & 0x20)) alu(o, rdh, (1 << rm) | (1 << rs)); // SMULWy
            else alu(o, rdh, (1 << rm) | (1 << rs) | (1 << rdl)); // SMLAxy / SMLAWy
            return;
        }
        if ((i & 0x0FC000F0) == 0x00000090) // MUL / MLA
        {
            int d = (i >> 16) & 0xF;
            u16 src = (1 << rm) | (1 << rs);
            if (i & (1 << 21)) src |= 1 << rd;
            alu(o, d, src);
            return;
        }
        if ((i & 0x0F8000F0) == 0x00800090) // long multiply
        {
            int rdh = (i >> 16) & 0xF, rdl = (i >> 12) & 0xF;
            u16 src = (1 << rm) | (1 << rs);
            if (i & (1 << 21)) src |= (1 << rdh) | (1 << rdl);
            alu(o, rdh, src);
            if (rdl < 15) o.dstMask |= 1 << rdl;
            return;
        }
        if ((i & 0x0FB00FF0) == 0x01000090) // SWP / SWPB
        {
            o.kind = InstrOps::Swap;
            o.loadRegs[o.nLoad++] = (u8)rd;
            if (rd < 15) o.dstMask |= 1 << rd;
            o.storeRegs[o.nStore++] = (u8)rm;
            return;
        }
        if ((i & 0x00000090) == 0x00000090 && (i & 0x60)) // halfword / signed / doubleword transfers
        {
            u32 sh = (i >> 5) & 0x3;
            bool l = i & (1 << 20);
            if (l)
            {
                load(o, rd);
                if (rn == 15) o.pcRelLoad = true;
            }
            else if (sh == 1) store(o, rd); // STRH
            else if (sh == 2) // LDRD
            {
                load(o, rd); load(o, rd + 1);
                if (rn == 15) o.pcRelLoad = true;
            }
            else { store(o, rd); store(o, rd + 1); } // STRD
            return;
        }
        // data processing, register operand
        {
            u32 opc = (i >> 21) & 0xF;
            u16 src = 1 << rm;
            if (i & 0x10) src |= 1 << rs;
            if (opc != 0xD && opc != 0xF) src |= 1 << rn;
            if (opc >= 0x8 && opc <= 0xB) { o.kind = InstrOps::None; return; } // TST/TEQ/CMP/CMN
            alu(o, rd, src);
            if (rd == 15) o.kind = InstrOps::Branch;
        }
        return;

    case 1:
        {
            u32 opc = (i >> 21) & 0xF;
            if ((i & 0x0FB00000) == 0x03200000) { o.kind = InstrOps::None; return; } // MSR imm
            if (opc >= 0x8 && opc <= 0xB) { o.kind = InstrOps::None; return; }
            u16 src = 0;
            if (opc != 0xD && opc != 0xF) src |= 1 << rn;
            alu(o, rd, src);
            if (rd == 15) o.kind = InstrOps::Branch;
        }
        return;

    case 2:
    case 3:
        if (op == 3 && (i & 0x10)) return; // undefined / media
        if (i & (1 << 20))
        {
            load(o, rd);
            if (rn == 15 && op == 2) o.pcRelLoad = true;
        }
        else store(o, rd);
        return;

    case 4:
        {
            u16 list = i & 0xFFFF;
            bool l = i & (1 << 20);
            for (int r = 0; r < 16; r++)
            {
                if (!(list & (1 << r))) continue;
                if (l) load(o, r);
                else store(o, r);
            }
            if (!list) o.kind = InstrOps::None;
        }
        return;

    case 5:
        o.kind = InstrOps::Branch;
        if (i & (1 << 24)) { o.writesLR = true; o.dstMask = 1 << 14; o.constResult = true; }
        return;

    case 6:
        return; // LDC/STC

    case 7:
        if (i & (1 << 24)) { o.kind = InstrOps::Swi; o.swiNum = (i >> 16) & 0xFF; return; }
        if ((i & 0x00100010) == 0x00100010 && rd != 15) { alu(o, rd, 0); return; } // MRC
        return;
    }
}

static void DecodeThumb(u16 i, InstrOps& o)
{
    int rd3 = i & 0x7;
    int rs3 = (i >> 3) & 0x7;

    switch (i >> 11)
    {
    case 0x00: case 0x01: case 0x02: // shift by immediate
        alu(o, rd3, 1 << rs3);
        return;
    case 0x03: // add/sub
        if (i & (1 << 10)) alu(o, rd3, 1 << rs3);
        else alu(o, rd3, (1 << rs3) | (1 << ((i >> 6) & 0x7)));
        return;
    case 0x04: alu(o, (i >> 8) & 0x7, 0); return; // MOV imm
    case 0x05: return;                            // CMP imm
    case 0x06: case 0x07: { int r = (i >> 8) & 0x7; alu(o, r, 1 << r); return; } // ADD/SUB imm8
    case 0x08:
        if (!(i & 0x0400)) // ALU
        {
            u32 op = (i >> 6) & 0xF;
            switch (op)
            {
            case 0x8: case 0xA: case 0xB: return;               // TST CMP CMN
            case 0x9: case 0xF: alu(o, rd3, 1 << rs3); return;  // NEG MVN
            default: alu(o, rd3, (1 << rd3) | (1 << rs3)); return;
            }
        }
        else // hi register ops / BX
        {
            u32 op = (i >> 8) & 0x3;
            int rd = (i & 0x7) | ((i >> 4) & 0x8);
            int rs = (i >> 3) & 0xF;
            if (op == 0) { alu(o, rd, (1 << rd) | (1 << rs)); if (rd == 15) o.kind = InstrOps::Branch; return; }
            if (op == 1) return;
            if (op == 2) { alu(o, rd, 1 << rs); if (rd == 15) o.kind = InstrOps::Branch; return; }
            o.kind = InstrOps::Branch;
            if (i & 0x80) { o.writesLR = true; o.dstMask = 1 << 14; o.constResult = true; }
            return;
        }
    case 0x09: load(o, (i >> 8) & 0x7); o.pcRelLoad = true; return; // LDR PC-relative
    case 0x0A: case 0x0B: // register offset
        {
            u32 op = (i >> 9) & 0x7;
            if (op <= 2) store(o, rd3);
            else load(o, rd3);
        }
        return;
    case 0x0C: case 0x0E: store(o, rd3); return;    // STR/STRB imm
    case 0x0D: case 0x0F: load(o, rd3); return;     // LDR/LDRB imm
    case 0x10: store(o, rd3); return;               // STRH imm
    case 0x11: load(o, rd3); return;                // LDRH imm
    case 0x12: store(o, (i >> 8) & 0x7); return;    // STR SP-relative
    case 0x13: load(o, (i >> 8) & 0x7); return;     // LDR SP-relative
    case 0x14: case 0x15: alu(o, (i >> 8) & 0x7, 0); return; // ADD Rd, PC/SP, imm (addresses)
    case 0x16: case 0x17:
        if ((i & 0x0600) == 0x0400) // PUSH / POP
        {
            bool l = i & 0x0800;
            for (int r = 0; r < 8; r++)
                if (i & (1 << r)) { if (l) load(o, r); else store(o, r); }
            if (i & 0x0100) { if (l) load(o, 15); else store(o, 14); }
            if (!o.nLoad && !o.nStore) o.kind = InstrOps::None;
        }
        return; // ADD SP / BKPT: nothing
    case 0x18: case 0x19: // STMIA / LDMIA
        {
            bool l = i & 0x0800;
            for (int r = 0; r < 8; r++)
                if (i & (1 << r)) { if (l) load(o, r); else store(o, r); }
            if (!o.nLoad && !o.nStore) o.kind = InstrOps::None;
        }
        return;
    case 0x1A: case 0x1B:
        if ((i & 0xFF00) == 0xDF00) { o.kind = InstrOps::Swi; o.swiNum = i & 0xFF; return; }
        o.kind = InstrOps::Branch;
        return;
    case 0x1C: o.kind = InstrOps::Branch; return;
    case 0x1D: case 0x1F: o.kind = InstrOps::Branch; o.writesLR = true; o.dstMask = 1 << 14; o.constResult = true; return;
    case 0x1E: alu(o, 14, 0); return; // BL prefix: LR = PC + offset
    }
}

static u16 AddressRegs(u32 i, bool thumb)
{
    if (thumb)
    {
        switch (i >> 11)
        {
        case 0x0A: case 0x0B: return (u16)((1 << ((i >> 3) & 7)) | (1 << ((i >> 6) & 7)));   // [Rb, Ro]
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: return (u16)(1 << ((i >> 3) & 7));
        case 0x12: case 0x13: case 0x16: case 0x17: return 1 << 13;
        case 0x18: case 0x19: return (u16)(1 << ((i >> 8) & 7));
        default: return 0;
        }
    }
    u16 m = (u16)(1 << ((i >> 16) & 0xF));
    u32 op = (i >> 25) & 7;
    if (op == 3) m |= (u16)(1 << (i & 0xF));                                  // register offset
    else if (op == 0 && !(i & (1 << 22)) && (i & 0x90) == 0x90 && (i & 0x60)) m |= (u16)(1 << (i & 0xF)); // halfword register offset
    return m & 0x7FFF;
}

void DecodeOps(u32 instr, bool thumb, InstrOps& ops)
{
    ops = InstrOps();
    if (thumb) DecodeThumb((u16)instr, ops);
    else DecodeARM(instr, ops);
    if (ops.kind == InstrOps::Load || ops.kind == InstrOps::Store || ops.kind == InstrOps::Swap)
        ops.addrMask = AddressRegs(instr, thumb);
}

}
