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

#include "Disasm.h"

#include <sstream>
#include <vector>

namespace melonDS::WB
{

static std::vector<std::string> SplitOps(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '[') depth++;
        else if (c == ']' && depth) depth--;
        if (depth == 0 && c == ',' && i + 1 < s.size() && s[i + 1] == ' ')
        {
            out.push_back(cur);
            cur.clear();
            i++;
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string JoinFrom(const std::vector<std::string>& p, size_t i)
{
    std::string s;
    for (; i < p.size(); i++)
    {
        if (!s.empty()) s += ", ";
        s += p[i];
    }
    return s;
}

static std::string CondPhrase(const std::string& c)
{
    if (c == "eq") return "equal (Z is set)";
    if (c == "ne") return "not equal (Z is clear)";
    if (c == "cs") return "carry set (unsigned higher or same)";
    if (c == "cc") return "carry clear (unsigned lower)";
    if (c == "mi") return "negative (N is set)";
    if (c == "pl") return "positive or zero (N is clear)";
    if (c == "vs") return "overflow set";
    if (c == "vc") return "overflow clear";
    if (c == "hi") return "unsigned higher";
    if (c == "ls") return "unsigned lower or same";
    if (c == "ge") return "signed greater than or equal";
    if (c == "lt") return "signed less than";
    if (c == "gt") return "signed greater than";
    if (c == "le") return "signed less than or equal";
    if (c == "nv") return "the never condition";
    return c;
}

static bool StripCond(const DisasmInsn& in, std::string& base, std::string& cond)
{
    base = in.Mnemonic;
    cond.clear();
    if (in.Conditional && base.size() > 2)
    {
        cond = base.substr(base.size() - 2);
        base.resize(base.size() - 2);
    }
    return true;
}

static bool Starts(const std::string& s, const char* p)
{
    return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

static std::string WidthOf(const std::string& suf)
{
    if (suf == "b" || suf == "bt") return "byte";
    if (suf == "h") return "halfword";
    if (suf == "sb") return "signed byte";
    if (suf == "sh") return "signed halfword";
    if (suf == "d") return "doubleword";
    return "word";
}

static std::string Describe(const std::string& base, const std::vector<std::string>& p, const DisasmInsn& in)
{
    auto need = [&](size_t n) { return p.size() >= n; };
    bool flags = false;
    std::string op = base;
    if (!op.empty() && op.back() == 's' && op != "mrs" && op != "msr" && op != "rsbs" &&
        (Starts(op, "add") || Starts(op, "sub") || Starts(op, "and") || Starts(op, "orr") || Starts(op, "eor") ||
         Starts(op, "mov") || Starts(op, "mvn") || Starts(op, "bic") || Starts(op, "adc") || Starts(op, "sbc") ||
         Starts(op, "rsc") || Starts(op, "rsb") || Starts(op, "lsl") || Starts(op, "lsr") || Starts(op, "asr") ||
         Starts(op, "ror") || Starts(op, "mul") || Starts(op, "neg")))
    {
        // "adds" / "lsls": the trailing s sets flags. "rsbs" keeps the s on a 4-letter base handled below.
        if (op.size() > 3 && op.substr(op.size() - 1) == "s")
        {
            flags = true;
            op.pop_back();
        }
    }
    if (op == "rsbs") { flags = true; op = "rsb"; }

    auto flagNote = [&](std::string s) {
        if (flags) s += " The flags are updated.";
        return s;
    };

    if (op == "add" || op == "adc" || op == "sub" || op == "sbc" || op == "rsb" || op == "rsc" ||
        op == "and" || op == "orr" || op == "eor" || op == "bic")
    {
        const char* verb = "computes";
        const char* how = "with";
        if (op == "add") { verb = "adds"; how = "and"; }
        else if (op == "adc") { verb = "adds with carry"; how = "and"; }
        else if (op == "sub") { verb = "subtracts"; how = "minus"; }
        else if (op == "sbc") { verb = "subtracts with carry"; how = "minus"; }
        else if (op == "rsb") { verb = "reverse-subtracts"; how = "from"; }
        else if (op == "rsc") { verb = "reverse-subtracts with carry"; how = "from"; }
        else if (op == "and") { verb = "ANDs"; how = "with"; }
        else if (op == "orr") { verb = "ORs"; how = "with"; }
        else if (op == "eor") { verb = "exclusive-ORs"; how = "with"; }
        else if (op == "bic") { verb = "clears the bits of"; how = "using"; }
        if (need(3))
            return flagNote(std::string(verb) + " " + p[1] + " " + how + " " + JoinFrom(p, 2) + ", and writes the result to " + p[0] + ".");
        if (need(2))
            return flagNote(std::string(verb) + " " + p[1] + " into " + p[0] + ".");
    }
    if (op == "mov" || op == "mvn")
    {
        if (need(2))
            return flagNote(std::string(op == "mvn" ? "Moves the bitwise NOT of " : "Copies ") + JoinFrom(p, 1) + " into " + p[0] + ".");
    }
    if (op == "lsl" || op == "lsr" || op == "asr" || op == "ror" || op == "rrx")
    {
        const char* name = op == "lsl" ? "logical left" : op == "lsr" ? "logical right" : op == "asr" ? "arithmetic right" : op == "ror" ? "right rotate" : "right rotate with carry";
        if (need(3))
            return flagNote(std::string("Shifts ") + p[1] + " " + name + " by " + JoinFrom(p, 2) + ", and writes the result to " + p[0] + ".");
        if (need(2))
            return flagNote(std::string("Shifts ") + p[0] + " " + name + " by " + JoinFrom(p, 1) + ".");
    }
    if (op == "cmp" || op == "cmn" || op == "tst" || op == "teq")
    {
        const char* what = op == "cmp" ? "Compares" : op == "cmn" ? "Compares (negative)" : op == "tst" ? "Tests (AND)" : "Tests (exclusive OR)";
        if (need(2))
            return std::string(what) + " " + p[0] + " with " + JoinFrom(p, 1) + ", and updates the flags. No register is written.";
    }
    if (op == "mul" || op == "mla")
    {
        if (op == "mul" && need(3))
            return flagNote("Multiplies " + p[1] + " by " + JoinFrom(p, 2) + ", and writes the low 32 bits to " + p[0] + ".");
        if (op == "mla" && need(4))
            return flagNote("Multiplies " + p[1] + " by " + p[2] + ", adds " + p[3] + ", and writes the low 32 bits to " + p[0] + ".");
    }
    if (op == "umull" || op == "smull" || op == "umlal" || op == "smlal")
    {
        const char* sign = (op[0] == 'u') ? "unsigned" : "signed";
        bool acc = op.find("mlal") != std::string::npos || op == "umlal" || op == "smlal";
        if (need(4))
            return std::string("64-bit ") + sign + " multiply of " + p[2] + " by " + p[3] +
                   (acc ? ", added to the previous 64-bit value, " : ", ") +
                   "written to " + p[0] + " (low) and " + p[1] + " (high).";
    }
    if (op == "ldrd" || op == "strd")
    {
        if (need(3))
        {
            if (op == "strd")
                return "Stores the doubleword in " + p[0] + " and " + p[1] + " to " + p.back() + ".";
            return "Loads a doubleword from " + p.back() + " into " + p[0] + " and " + p[1] + ".";
        }
    }
    if (Starts(op, "ldr") || Starts(op, "str"))
    {
        bool store = Starts(op, "str");
        std::string suf = op.substr(3);
        std::string width = WidthOf(suf);
        if (store && need(2))
            return "Stores the " + width + " in " + p[0] + " to " + JoinFrom(p, 1) + ".";
        if (!store && need(2))
            return "Loads a " + width + " from " + JoinFrom(p, 1) + " into " + p[0] + ".";
    }
    if (op == "ldm" || op == "stm" || op == "push" || op == "pop")
    {
        if (op == "push" && need(1))
            return "Pushes " + p[0] + " onto the stack (the address is sp, updated as the registers are stored).";
        if (op == "pop" && need(1))
            return "Pops " + p[0] + " off the stack (the address is sp, updated as the registers are loaded).";
        if (op == "ldm" && need(2))
            return "Loads several registers " + JoinFrom(p, 1) + " from memory at " + p[0] + ".";
        if (op == "stm" && need(2))
            return "Stores several registers " + JoinFrom(p, 1) + " to memory at " + p[0] + ".";
    }
    if (op == "b" || op == "bl" || op == "bx" || op == "blx")
    {
        std::string dest = in.HasTarget ? Hex(in.Target) : (need(1) ? p[0] : std::string("the target"));
        if (in.HasTarget && in.TargetThumb && (op == "blx" || op == "bx")) dest += " (Thumb)";
        if (op == "b") return "Branches to " + dest + ".";
        if (op == "bl") return "Calls " + dest + ", saving the return address in lr.";
        if (op == "bx") return std::string("Branches to the address in ") + (need(1) ? p[0] : dest) + ", and switches to ARM or Thumb according to bit 0.";
        if (op == "blx") return "Calls " + dest + ", saving the return address in lr, and switches instruction set.";
    }
    if (op == "swi" || op == "svc")
    {
        if (need(1)) return "Software interrupt " + p[0] + ". The CPU enters supervisor mode at the SWI vector.";
        return "Software interrupt. The CPU enters supervisor mode at the SWI vector.";
    }
    if (op == "mrs" && need(2)) return "Copies the status register " + p[1] + " into " + p[0] + ".";
    if (op == "msr" && need(2)) return "Writes " + JoinFrom(p, 1) + " into the status-register fields " + p[0] + ".";
    if (op == "clz" && need(2)) return "Counts the leading zero bits of " + p[1] + " and writes the count to " + p[0] + ".";
    if (op == "bkpt") return "Breakpoint. The CPU takes the prefetch-abort vector.";
    if (op == "nop") return "No operation. The instruction does not change registers or memory.";
    if (op == "pld" && need(1)) return "Prefetch hint for the address " + p[0] + ". It does not load a register.";
    if (Starts(op, "mcr") || Starts(op, "mrc") || Starts(op, "cdp") || Starts(op, "ldc") || Starts(op, "stc") || Starts(op, "mcrr") || Starts(op, "mrrc"))
        return "Coprocessor operation " + in.Text() + ".";
    if (Starts(op, "qadd") || Starts(op, "qsub") || Starts(op, "qdadd") || Starts(op, "qdsub") || Starts(op, "sml") || Starts(op, "smu") || Starts(op, "smla") || Starts(op, "smlal"))
    {
        if (need(1)) return "Saturating or signed-multiply operation: " + in.Text() + ".";
    }
    if (op == "neg" && need(2)) return flagNote("Negates " + p[1] + " and writes the result to " + p[0] + ".");
    return {};
}

std::string ExplainInsn(const DisasmInsn& in)
{
    if (!in.Valid || in.Mnemonic.empty() || in.Mnemonic[0] == '.')
        return "This is not a recognised ARM or Thumb instruction.";
    std::string base, cond;
    StripCond(in, base, cond);
    std::string body = Describe(base, SplitOps(in.Operands), in);
    if (body.empty())
        return "This instruction (" + in.Text() + ") is not one the workbench can explain in plain English.";
    if (!cond.empty())
        body += " This runs only when the condition is " + CondPhrase(cond) + ".";
    return body;
}

static std::string CReg(const std::string& r)
{
    if (r == "sp") return "sp";
    if (r == "lr") return "lr";
    if (r == "pc") return "pc";
    return r;
}

std::string InsnAsC(const DisasmInsn& in)
{
    if (!in.Valid || in.Mnemonic.empty() || in.Mnemonic[0] == '.')
        return "/* data */ " + in.Text() + ";";
    std::string base, cond;
    StripCond(in, base, cond);
    if (!base.empty() && base.back() == 's' && base.size() > 3) base.pop_back();
    auto p = SplitOps(in.Operands);
    std::string c;
    auto bin = [&](const char* sym) -> bool {
        if (p.size() >= 3) { c = CReg(p[0]) + " = " + CReg(p[1]) + " " + sym + " " + JoinFrom(p, 2) + ";"; return true; }
        if (p.size() == 2) { c = CReg(p[0]) + " " + sym + "= " + p[1] + ";"; return true; }
        return false;
    };
    if (base == "add" || base == "adc") bin(base == "adc" ? "+ /*carry*/ " : "+");
    else if (base == "sub" || base == "sbc") bin("-");
    else if (base == "and") bin("&");
    else if (base == "orr") bin("|");
    else if (base == "eor") bin("^");
    else if (base == "mov" && p.size() >= 2) c = CReg(p[0]) + " = " + JoinFrom(p, 1) + ";";
    else if (base == "mvn" && p.size() >= 2) c = CReg(p[0]) + " = ~(" + JoinFrom(p, 1) + ");";
    else if (base == "cmp" && p.size() >= 2) c = "/* compare " + p[0] + " with " + JoinFrom(p, 1) + " */";
    else if ((base == "ldrd" || base == "strd") && p.size() >= 3)
    {
        std::string addr = p.back();
        if (base == "strd")
            c = "*(uint64_t*)(" + addr + ") = ((uint64_t)" + CReg(p[1]) + " << 32) | " + CReg(p[0]) + ";";
        else
            c = CReg(p[0]) + " = (uint32_t)*(uint64_t*)(" + addr + "); " + CReg(p[1]) + " = *(uint64_t*)(" + addr + ") >> 32;";
    }
    else if ((Starts(base, "ldr") || Starts(base, "str")) && p.size() >= 2)
    {
        bool store = Starts(base, "str");
        std::string width = WidthOf(base.substr(3));
        std::string cast = width == "byte" ? "uint8_t" : width == "signed byte" ? "int8_t"
            : width == "halfword" ? "uint16_t" : width == "signed halfword" ? "int16_t"
            : width == "doubleword" ? "uint64_t" : "uint32_t";
        if (store) c = "*(" + cast + "*)(" + JoinFrom(p, 1) + ") = " + CReg(p[0]) + ";";
        else c = CReg(p[0]) + " = *(" + cast + "*)(" + JoinFrom(p, 1) + ");";
    }
    else if (base == "b" && in.HasTarget) c = "goto loc_" + Hex(in.Target) + ";";
    else if ((base == "bl" || base == "blx") && in.HasTarget) c = "sub_" + Hex(in.Target) + "();";
    else if (base == "bx") c = "return;";
    else if (base == "push" && !p.empty()) c = "/* push " + p[0] + " */";
    else if (base == "pop" && !p.empty()) c = "/* pop " + p[0] + " */";
    else if (base == "swi" || base == "svc") c = "swi(" + (p.empty() ? std::string("0") : p[0]) + ");";
    if (c.empty()) c = "/* " + in.Text() + " */";
    if (!cond.empty()) c = "if (" + cond + ") " + c;
    return c;
}

}
