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

// ARMv5TE / Thumb disassembler (UAL-flavoured, lower case, as used by
// objdump and most DS decomp projects).

#include <cstdio>
#include "Disasm.h"

namespace melonDS::WB
{

static const char* const CondNames[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "", "nv"};
static const char* const DPNames[16] = {"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};
static const char* const ShiftNames[4] = {"lsl", "lsr", "asr", "ror"};

std::string RegName(int r)
{
    static const char* const names[16] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
    return names[r & 15];
}

std::string Hex(u32 v)
{
    char buf[16];
    if (v < 10) snprintf(buf, sizeof(buf), "%u", v);
    else snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

static std::string Imm(u32 v) { return "#" + Hex(v); }

static std::string SImm(s32 v)
{
    if (v < 0) return "#-" + Hex((u32)-v);
    return "#" + Hex((u32)v);
}

static std::string RegList(u32 list, DisasmInsn& o)
{
    std::string s = "{";
    bool first = true;
    for (int r = 0; r < 16; r++)
    {
        if (!(list & (1 << r))) continue;
        int e = r;
        while (e < 15 && (list & (1 << (e + 1))) && e + 1 <= 12) e++;
        if (!first) s += ", ";
        first = false;
        o.RegMask |= (u16)(1 << r);
        if (e > r + 1 && r < 13)
        {
            s += RegName(r) + "-" + RegName(e);
            for (int k = r; k <= e; k++) o.RegMask |= (u16)(1 << k);
            r = e;
        }
        else if (e == r + 1 && r < 13)
        {
            s += RegName(r) + ", " + RegName(e);
            o.RegMask |= (u16)(1 << e);
            r = e;
        }
        else
            s += RegName(r);
    }
    return s + "}";
}

static std::string R(int r, DisasmInsn& o)
{
    o.RegMask |= (u16)(1 << (r & 15));
    return RegName(r);
}

static std::string ShiftOperand(u32 i, DisasmInsn& o)
{
    int rm = i & 0xF;
    u32 type = (i >> 5) & 3;
    std::string s = R(rm, o);
    if (i & 0x10)
    {
        s += ", " + std::string(ShiftNames[type]) + " " + R((i >> 8) & 0xF, o);
        return s;
    }
    u32 amt = (i >> 7) & 0x1F;
    if (amt == 0)
    {
        if (type == 0) return s;
        if (type == 3) return s + ", rrx";
        amt = 32;
    }
    return s + ", " + ShiftNames[type] + " #" + std::to_string(amt);
}

static void Undefined(u32 i, DisasmInsn& o, bool thumb)
{
    o.Valid = false;
    o.Mnemonic = thumb ? ".hword" : ".word";
    o.Operands = Hex(thumb ? (i & 0xFFFF) : i);
}

static void DisARM(u32 addr, u32 i, DisasmInsn& o)
{
    o.Size = 4;
    u32 condv = i >> 28;
    std::string cond = CondNames[condv];
    o.Conditional = condv != 0xE && condv != 0xF;
    u32 pc = addr + 8;
    int rd = (i >> 12) & 0xF, rn = (i >> 16) & 0xF, rm = i & 0xF, rs = (i >> 8) & 0xF;

    if (condv == 0xF)
    {
        if ((i & 0x0E000000) == 0x0A000000) // BLX imm
        {
            s32 off = ((s32)(i << 8) >> 6) | ((i >> 23) & 2);
            o.Mnemonic = "blx";
            o.HasTarget = true;
            o.Target = pc + off;
            o.TargetThumb = true;
            o.IsCall = true;
            o.IsBranch = true;
            o.Operands = Hex(o.Target);
            return;
        }
        if ((i & 0x0D70F000) == 0x0550F000) // PLD
        {
            u32 imm = i & 0xFFF;
            bool up = i & (1 << 23);
            o.Mnemonic = "pld";
            if (i & (1 << 25)) o.Operands = "[" + R(rn, o) + ", " + (up ? "" : "-") + ShiftOperand(i & ~0x10u, o) + "]";
            else o.Operands = "[" + R(rn, o) + ", " + SImm(up ? (s32)imm : -(s32)imm) + "]";
            return;
        }
        return Undefined(i, o, false);
    }

    switch ((i >> 25) & 7)
    {
    case 0:
        if ((i & 0x0FFFFFF0) == 0x012FFF10) { o.Mnemonic = "bx" + cond; o.Operands = R(rm, o); o.IsBranch = true; o.IsReturn = (rm == 14); return; }
        if ((i & 0x0FFFFFF0) == 0x012FFF30) { o.Mnemonic = "blx" + cond; o.Operands = R(rm, o); o.IsBranch = true; o.IsCall = true; return; }
        if ((i & 0x0FFF0FF0) == 0x016F0F10) { o.Mnemonic = "clz" + cond; o.Operands = R(rd, o) + ", " + R(rm, o); return; }
        if ((i & 0xFFF000F0) == 0xE1200070) { o.Mnemonic = "bkpt"; o.Operands = Imm(((i >> 4) & 0xFFF0) | (i & 0xF)); return; }
        if ((i & 0x0F900FF0) == 0x01000050)
        {
            static const char* const q[4] = {"qadd", "qsub", "qdadd", "qdsub"};
            o.Mnemonic = q[(i >> 21) & 3] + cond;
            o.Operands = R(rd, o) + ", " + R(rm, o) + ", " + R(rn, o);
            return;
        }
        if ((i & 0x0FBF0FFF) == 0x010F0000) { o.Mnemonic = "mrs" + cond; o.Operands = R(rd, o) + ", " + ((i & (1 << 22)) ? "spsr" : "cpsr"); return; }
        if ((i & 0x0FB0FFF0) == 0x0120F000)
        {
            std::string f = (i & (1 << 22)) ? "spsr_" : "cpsr_";
            if (i & (1 << 16)) f += "c";
            if (i & (1 << 17)) f += "x";
            if (i & (1 << 18)) f += "s";
            if (i & (1 << 19)) f += "f";
            o.Mnemonic = "msr" + cond;
            o.Operands = f + ", " + R(rm, o);
            return;
        }
        if ((i & 0x0F900090) == 0x01000080)
        {
            u32 op = (i >> 21) & 3;
            const char* xy[4] = {"bb", "tb", "bt", "tt"};
            int sel = ((i >> 5) & 1) | (((i >> 6) & 1) << 1);
            int d = (i >> 16) & 0xF, n = (i >> 12) & 0xF;
            switch (op)
            {
            case 0: o.Mnemonic = std::string("smla") + xy[sel] + cond; o.Operands = R(d, o) + ", " + R(rm, o) + ", " + R(rs, o) + ", " + R(n, o); return;
            case 1:
                if (i & 0x20) { o.Mnemonic = std::string("smulw") + ((i & 0x40) ? "t" : "b") + cond; o.Operands = R(d, o) + ", " + R(rm, o) + ", " + R(rs, o); }
                else { o.Mnemonic = std::string("smlaw") + ((i & 0x40) ? "t" : "b") + cond; o.Operands = R(d, o) + ", " + R(rm, o) + ", " + R(rs, o) + ", " + R(n, o); }
                return;
            case 2: o.Mnemonic = std::string("smlal") + xy[sel] + cond; o.Operands = R(n, o) + ", " + R(d, o) + ", " + R(rm, o) + ", " + R(rs, o); return;
            case 3: o.Mnemonic = std::string("smul") + xy[sel] + cond; o.Operands = R(d, o) + ", " + R(rm, o) + ", " + R(rs, o); return;
            }
        }
        if ((i & 0x0FC000F0) == 0x00000090)
        {
            int d = (i >> 16) & 0xF;
            std::string s = (i & (1 << 20)) ? "s" : "";
            if (i & (1 << 21)) { o.Mnemonic = "mla" + s + cond; o.Operands = R(d, o) + ", " + R(rm, o) + ", " + R(rs, o) + ", " + R(rd, o); }
            else { o.Mnemonic = "mul" + s + cond; o.Operands = R(d, o) + ", " + R(rm, o) + ", " + R(rs, o); }
            return;
        }
        if ((i & 0x0F8000F0) == 0x00800090)
        {
            static const char* const names[4] = {"umull", "umlal", "smull", "smlal"};
            std::string s = (i & (1 << 20)) ? "s" : "";
            o.Mnemonic = names[(i >> 21) & 3] + s + cond;
            o.Operands = R(rd, o) + ", " + R(rn, o) + ", " + R(rm, o) + ", " + R(rs, o);
            return;
        }
        if ((i & 0x0FB00FF0) == 0x01000090)
        {
            o.Mnemonic = std::string("swp") + cond + ((i & (1 << 22)) ? "b" : "");
            o.Operands = R(rd, o) + ", " + R(rm, o) + ", [" + R(rn, o) + "]";
            return;
        }
        if ((i & 0x90) == 0x90 && (i & 0x60))
        {
            u32 sh = (i >> 5) & 3;
            bool l = i & (1 << 20);
            const char* name;
            if (l) name = sh == 1 ? "ldrh" : sh == 2 ? "ldrsb" : "ldrsh";
            else name = sh == 1 ? "strh" : sh == 2 ? "ldrd" : "strd";
            o.Mnemonic = std::string(name) + cond;
            bool p = i & (1 << 24), u = i & (1 << 23), imm = i & (1 << 22), w = i & (1 << 21);
            std::string off;
            if (imm)
            {
                u32 v = ((i >> 4) & 0xF0) | (i & 0xF);
                off = v ? ", " + SImm(u ? (s32)v : -(s32)v) : "";
                if (rn == 15 && p)
                {
                    o.HasRef = true;
                    o.Ref = u ? pc + v : pc - v;
                    // LDRD is a load with the L bit clear: the opcode (sh == 2) says so
                    o.LiteralLoad = l || sh == 2;
                }
            }
            else off = std::string(", ") + (u ? "" : "-") + R(rm, o);
            std::string dst = R(rd, o);
            if (sh >= 2 && !l) dst += ", " + R(rd + 1, o);
            if (p) o.Operands = dst + ", [" + R(rn, o) + off + "]" + (w ? "!" : "");
            else o.Operands = dst + ", [" + R(rn, o) + "]" + off;
            if (l && rd == 15) o.IsBranch = true;
            return;
        }
        // fallthrough: data processing register
        [[fallthrough]];
    case 1:
        {
            u32 opc = (i >> 21) & 0xF;
            bool s = i & (1 << 20);
            if ((i & 0x0FB00000) == 0x03200000) // MSR imm
            {
                std::string f = (i & (1 << 22)) ? "spsr_" : "cpsr_";
                if (i & (1 << 16)) f += "c";
                if (i & (1 << 17)) f += "x";
                if (i & (1 << 18)) f += "s";
                if (i & (1 << 19)) f += "f";
                u32 rot = ((i >> 8) & 0xF) * 2;
                u32 v = (i & 0xFF);
                v = (v >> rot) | (v << ((32 - rot) & 31));
                o.Mnemonic = "msr" + cond;
                o.Operands = f + ", " + Imm(v);
                return;
            }
            std::string op2;
            u32 immv = 0;
            bool isimm = i & (1 << 25);
            if (isimm)
            {
                u32 rot = ((i >> 8) & 0xF) * 2;
                immv = i & 0xFF;
                immv = (immv >> rot) | (immv << ((32 - rot) & 31));
                op2 = Imm(immv);
            }
            else op2 = ShiftOperand(i, o);

            bool test = opc >= 8 && opc <= 11;
            std::string mn = DPNames[opc];
            if (!test && s) mn += "s";
            mn += cond;
            if (opc == 13 || opc == 15)
            {
                // mov/mvn; show shifts by immediate as their own mnemonics like objdump does for plain movs
                if (opc == 13 && !isimm && !(i & 0xFF0) && rd == 15 && rm == 14) o.IsReturn = true;
                o.Mnemonic = mn;
                o.Operands = R(rd, o) + ", " + op2;
                if (opc == 13 && !isimm && rd == 14 && rm == 15) o.Mnemonic = mn;   // mov lr, pc (call idiom)
            }
            else if (test)
            {
                o.Mnemonic = mn;
                o.Operands = R(rn, o) + ", " + op2;
            }
            else
            {
                o.Mnemonic = mn;
                o.Operands = R(rd, o) + ", " + R(rn, o) + ", " + op2;
                if (rn == 15 && isimm && (opc == 4 || opc == 2))
                {
                    o.HasRef = true;
                    o.Ref = opc == 4 ? pc + immv : pc - immv;
                }
            }
            if (rd == 15 && !test) { o.IsBranch = true; if (opc == 2 && rn == 14 && s) o.IsReturn = true; }
        }
        return;

    case 2:
    case 3:
        {
            if ((i & (1 << 25)) && (i & 0x10)) return Undefined(i, o, false);
            bool p = i & (1 << 24), u = i & (1 << 23), b = i & (1 << 22), w = i & (1 << 21), l = i & (1 << 20);
            std::string mn = l ? "ldr" : "str";
            mn += cond;
            if (b) mn += "b";
            if (!p && w) mn += "t";
            o.Mnemonic = mn;
            std::string off;
            if (!(i & (1 << 25)))
            {
                u32 v = i & 0xFFF;
                off = v ? ", " + SImm(u ? (s32)v : -(s32)v) : "";
                if (rn == 15 && p)
                {
                    o.HasRef = true;
                    o.Ref = u ? pc + v : pc - v;
                    o.LiteralLoad = l && !b;
                }
            }
            else off = std::string(", ") + (u ? "" : "-") + ShiftOperand(i, o);
            if (p) o.Operands = R(rd, o) + ", [" + R(rn, o) + off + "]" + (w ? "!" : "");
            else o.Operands = R(rd, o) + ", [" + R(rn, o) + "]" + off;
            if (l && rd == 15)
            {
                o.IsBranch = true;
                if (rn == 13 && !p && u && (i & 0xFFF) == 4) { o.IsReturn = true; o.Mnemonic = "pop" + cond; o.Operands = "{pc}"; }
            }
            else if (!l && rn == 13 && p && !u && w && (i & 0xFFF) == 4) { o.Mnemonic = "push" + cond; o.Operands = "{" + R(rd, o) + "}"; }
            else if (l && rn == 13 && !p && u && (i & 0xFFF) == 4 && !b) { o.Mnemonic = "pop" + cond; o.Operands = "{" + R(rd, o) + "}"; }
        }
        return;

    case 4:
        {
            bool p = i & (1 << 24), u = i & (1 << 23), s = i & (1 << 22), w = i & (1 << 21), l = i & (1 << 20);
            u32 list = i & 0xFFFF;
            static const char* const modes[4] = {"da", "ia", "db", "ib"};
            std::string mode = modes[(p ? 2 : 0) | (u ? 1 : 0)];
            if (rn == 13 && w && !s)
            {
                if (l && mode == "ia") { o.Mnemonic = "pop" + cond; o.Operands = RegList(list, o); if (list & 0x8000) { o.IsBranch = true; o.IsReturn = true; } return; }
                if (!l && mode == "db") { o.Mnemonic = "push" + cond; o.Operands = RegList(list, o); return; }
            }
            o.Mnemonic = std::string(l ? "ldm" : "stm") + (mode == "ia" ? "" : mode) + cond;
            o.Operands = R(rn, o) + (w ? "!" : "") + ", " + RegList(list, o) + (s ? "^" : "");
            if (l && (list & 0x8000)) { o.IsBranch = true; o.IsReturn = true; }
        }
        return;

    case 5:
        {
            s32 off = (s32)(i << 8) >> 6;
            bool link = i & (1 << 24);
            o.Mnemonic = std::string(link ? "bl" : "b") + cond;
            o.HasTarget = true;
            o.Target = pc + off;
            o.IsBranch = true;
            o.IsCall = link;
            o.Operands = Hex(o.Target);
        }
        return;

    case 6:
        {
            bool p = i & (1 << 24), u = i & (1 << 23), n = i & (1 << 22), w = i & (1 << 21), l = i & (1 << 20);
            u32 v = (i & 0xFF) * 4;
            o.Mnemonic = std::string(l ? "ldc" : "stc") + cond + (n ? "l" : "");
            std::string off = v ? ", " + SImm(u ? (s32)v : -(s32)v) : "";
            o.Operands = "p" + std::to_string((i >> 8) & 0xF) + ", c" + std::to_string(rd) + ", [" + R(rn, o) +
                (p ? off + "]" + (w ? "!" : "") : "]" + off);
        }
        return;

    case 7:
        if (i & (1 << 24))
        {
            o.Mnemonic = "swi" + cond;
            o.Operands = Hex((i >> 16) & 0xFF);
            return;
        }
        if (i & 0x10)
        {
            bool l = i & (1 << 20);
            o.Mnemonic = std::string(l ? "mrc" : "mcr") + cond;
            o.Operands = "p" + std::to_string((i >> 8) & 0xF) + ", " + std::to_string((i >> 21) & 7) + ", " + R(rd, o) +
                ", c" + std::to_string(rn) + ", c" + std::to_string(rm) + ", " + std::to_string((i >> 5) & 7);
        }
        else
        {
            o.Mnemonic = "cdp" + cond;
            o.Operands = "p" + std::to_string((i >> 8) & 0xF) + ", " + std::to_string((i >> 20) & 0xF) + ", c" + std::to_string(rd) +
                ", c" + std::to_string(rn) + ", c" + std::to_string(rm) + ", " + std::to_string((i >> 5) & 7);
        }
        return;
    }
}

static void DisThumb(u32 addr, u32 word, DisasmInsn& o)
{
    u16 i = (u16)word;
    u16 next = (u16)(word >> 16);
    o.Size = 2;
    o.Thumb = true;
    u32 pc = addr + 4;
    int rd = i & 7, rs = (i >> 3) & 7;

    switch (i >> 11)
    {
    case 0x00: case 0x01: case 0x02:
        {
            u32 op = (i >> 11) & 3;
            u32 amt = (i >> 6) & 0x1F;
            if (op == 0 && amt == 0) { o.Mnemonic = "movs"; o.Operands = R(rd, o) + ", " + R(rs, o); return; }
            if (op != 0 && amt == 0) amt = 32;
            o.Mnemonic = std::string(ShiftNames[op]) + "s";
            o.Operands = R(rd, o) + ", " + R(rs, o) + ", #" + std::to_string(amt);
        }
        return;
    case 0x03:
        {
            bool sub = i & (1 << 9);
            std::string third = (i & (1 << 10)) ? Imm((i >> 6) & 7) : R((i >> 6) & 7, o);
            if ((i & (1 << 10)) && ((i >> 6) & 7) == 0 && !sub) { o.Mnemonic = "movs"; o.Operands = R(rd, o) + ", " + R(rs, o); return; }
            o.Mnemonic = sub ? "subs" : "adds";
            o.Operands = R(rd, o) + ", " + R(rs, o) + ", " + third;
        }
        return;
    case 0x04: o.Mnemonic = "movs"; o.Operands = R((i >> 8) & 7, o) + ", " + Imm(i & 0xFF); return;
    case 0x05: o.Mnemonic = "cmp"; o.Operands = R((i >> 8) & 7, o) + ", " + Imm(i & 0xFF); return;
    case 0x06: o.Mnemonic = "adds"; o.Operands = R((i >> 8) & 7, o) + ", " + Imm(i & 0xFF); return;
    case 0x07: o.Mnemonic = "subs"; o.Operands = R((i >> 8) & 7, o) + ", " + Imm(i & 0xFF); return;
    case 0x08:
        if (!(i & 0x400))
        {
            static const char* const ops[16] = {"ands", "eors", "lsls", "lsrs", "asrs", "adcs", "sbcs", "rors", "tst", "negs", "cmp", "cmn", "orrs", "muls", "bics", "mvns"};
            u32 op = (i >> 6) & 0xF;
            o.Mnemonic = ops[op];
            o.Operands = R(rd, o) + ", " + R(rs, o);
            return;
        }
        else
        {
            u32 op = (i >> 8) & 3;
            int d = (i & 7) | ((i >> 4) & 8);
            int m = (i >> 3) & 0xF;
            switch (op)
            {
            case 0: o.Mnemonic = "add"; o.Operands = R(d, o) + ", " + R(m, o); if (d == 15) o.IsBranch = true; return;
            case 1: o.Mnemonic = "cmp"; o.Operands = R(d, o) + ", " + R(m, o); return;
            case 2:
                o.Mnemonic = "mov"; o.Operands = R(d, o) + ", " + R(m, o);
                if (d == 15) { o.IsBranch = true; o.IsReturn = (m == 14); }
                return;
            case 3:
                if (i & 0x80) { o.Mnemonic = "blx"; o.IsCall = true; }
                else { o.Mnemonic = "bx"; o.IsReturn = (m == 14); }
                o.IsBranch = true;
                o.Operands = R(m, o);
                return;
            }
        }
        return;
    case 0x09:
        {
            u32 v = (i & 0xFF) * 4;
            o.Mnemonic = "ldr";
            o.Operands = R((i >> 8) & 7, o) + ", [pc, " + Imm(v) + "]";
            o.HasRef = true;
            o.Ref = (pc & ~3u) + v;
            o.LiteralLoad = true;
        }
        return;
    case 0x0A: case 0x0B:
        {
            static const char* const ops[8] = {"str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh"};
            o.Mnemonic = ops[(i >> 9) & 7];
            o.Operands = R(rd, o) + ", [" + R(rs, o) + ", " + R((i >> 6) & 7, o) + "]";
        }
        return;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        {
            bool b = i & (1 << 12), l = i & (1 << 11);
            u32 v = ((i >> 6) & 0x1F) * (b ? 1 : 4);
            o.Mnemonic = std::string(l ? "ldr" : "str") + (b ? "b" : "");
            o.Operands = R(rd, o) + ", [" + R(rs, o) + (v ? ", " + Imm(v) : "") + "]";
        }
        return;
    case 0x10: case 0x11:
        {
            u32 v = ((i >> 6) & 0x1F) * 2;
            o.Mnemonic = (i & (1 << 11)) ? "ldrh" : "strh";
            o.Operands = R(rd, o) + ", [" + R(rs, o) + (v ? ", " + Imm(v) : "") + "]";
        }
        return;
    case 0x12: case 0x13:
        {
            u32 v = (i & 0xFF) * 4;
            o.Mnemonic = (i & (1 << 11)) ? "ldr" : "str";
            o.Operands = R((i >> 8) & 7, o) + ", [sp" + (v ? ", " + Imm(v) : "") + "]";
        }
        return;
    case 0x14:
        o.Mnemonic = "add";
        o.Operands = R((i >> 8) & 7, o) + ", pc, " + Imm((i & 0xFF) * 4);
        o.HasRef = true;
        o.Ref = (pc & ~3u) + (i & 0xFF) * 4;
        return;
    case 0x15: o.Mnemonic = "add"; o.Operands = R((i >> 8) & 7, o) + ", sp, " + Imm((i & 0xFF) * 4); return;
    case 0x16: case 0x17:
        if ((i & 0xFF00) == 0xB000)
        {
            u32 v = (i & 0x7F) * 4;
            o.Mnemonic = (i & 0x80) ? "sub" : "add";
            o.Operands = "sp, " + Imm(v);
            return;
        }
        if ((i & 0x0600) == 0x0400)
        {
            bool l = i & (1 << 11);
            u32 list = i & 0xFF;
            if (i & 0x100) list |= l ? 0x8000 : 0x4000;
            o.Mnemonic = l ? "pop" : "push";
            o.Operands = RegList(list, o);
            if (l && (list & 0x8000)) { o.IsBranch = true; o.IsReturn = true; }
            return;
        }
        if ((i & 0xFF00) == 0xBE00) { o.Mnemonic = "bkpt"; o.Operands = Imm(i & 0xFF); return; }
        return Undefined(i, o, true);
    case 0x18: case 0x19:
        {
            bool l = i & (1 << 11);
            int b = (i >> 8) & 7;
            o.Mnemonic = l ? "ldmia" : "stmia";
            bool wb = !l || !(i & (1 << b));
            o.Operands = R(b, o) + (wb ? "!" : "") + ", " + RegList(i & 0xFF, o);
        }
        return;
    case 0x1A: case 0x1B:
        {
            u32 c = (i >> 8) & 0xF;
            if (c == 0xF) { o.Mnemonic = "swi"; o.Operands = Hex(i & 0xFF); return; }
            if (c == 0xE) return Undefined(i, o, true);
            s32 off = (s32)(s8)(i & 0xFF) * 2;
            o.Mnemonic = std::string("b") + CondNames[c];
            o.HasTarget = true;
            o.Target = pc + off;
            o.TargetThumb = true;
            o.IsBranch = true;
            o.Conditional = true;
            o.Operands = Hex(o.Target);
        }
        return;
    case 0x1C:
        {
            s32 off = ((s32)(i << 21) >> 20);
            o.Mnemonic = "b";
            o.HasTarget = true;
            o.Target = pc + off;
            o.TargetThumb = true;
            o.IsBranch = true;
            o.Operands = Hex(o.Target);
        }
        return;
    case 0x1E:
        {
            // BL/BLX prefix: decode the pair when the suffix follows
            s32 hi = ((s32)(i << 21) >> 9);
            if ((next & 0xF800) == 0xF800 || (next & 0xF800) == 0xE800)
            {
                u32 lo = (next & 0x7FF) << 1;
                u32 target = pc + hi + lo;
                bool blx = (next & 0xF800) == 0xE800;
                if (blx) target &= ~3u;
                o.Size = 4;
                o.Mnemonic = blx ? "blx" : "bl";
                o.HasTarget = true;
                o.Target = target;
                o.TargetThumb = !blx;
                o.IsCall = true;
                o.IsBranch = true;
                o.Operands = Hex(target);
                return;
            }
            o.Mnemonic = "bl.hi";
            o.Operands = "lr, pc, " + SImm(hi);
        }
        return;
    case 0x1D: case 0x1F:
        o.Mnemonic = (i >> 11) == 0x1D ? "blx.lo" : "bl.lo";
        o.Operands = "lr, " + Imm((i & 0x7FF) << 1);
        return;
    }
    Undefined(i, o, true);
}

bool LoadsLiteralPool(u32 instr, bool thumb)
{
    if (thumb) return (instr & 0xF800) == 0x4800;
    // LDR rd, [pc, #±imm]: pre-indexed, no writeback, a word. The up/down bit is outside the mask.
    if ((instr & 0x0F7F0000) == 0x051F0000) return true;
    // LDRH / LDRSB / LDRD / LDRSH from pc, immediate, pre-indexed, no writeback.
    u32 half = instr & 0x0F7F00F0;
    return half == 0x015F00B0 || half == 0x015F00D0 || half == 0x014F00D0 || half == 0x015F00F0;
}

void Disassemble(u32 addr, u32 instr, bool thumb, DisasmInsn& out)
{
    out = DisasmInsn();
    out.Addr = addr;
    out.Thumb = thumb;
    if (thumb) DisThumb(addr, instr, out);
    else DisARM(addr, instr, out);
}

bool IsFunctionPrologue(u32 instr, bool thumb)
{
    if (thumb)
        return (instr & 0xFF00) == 0xB500;   // push {..., lr}
    if ((instr >> 28) == 0xF) return false;
    // stmfd sp!, {..., lr}  (P=1 U=0 W=1, base sp, lr in the list)
    if ((instr & 0x0FFF0000) == 0x092D0000 && (instr & (1u << 14))) return true;
    // str lr, [sp, #-4]!
    if ((instr & 0x0FFFFFFF) == 0x052DE004) return true;
    return false;
}

}
