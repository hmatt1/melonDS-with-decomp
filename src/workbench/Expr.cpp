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

// Breakpoint condition expressions.
//
//   r0 == 0x10 && [sp+4].w > 3
//   u16[r4 + 0x20] != 0 || value == 0xFFFF
//
// Operands: numbers (decimal, 0x hex, $hex), registers r0-r15 sp lr pc cpsr,
// value / addr / size (the access that hit a watchpoint), hits, frame, cpu.
// Memory: [expr] (32-bit), [expr].b .h .w .sb .sh, or u8[..] u16[..] u32[..] s8[..] s16[..] s32[..].
// Operators follow C precedence: ! ~ - * / % + - << >> < <= > >= == != & ^ | && ||

#include <cctype>
#include <cstring>
#include "Workbench.h"

namespace melonDS::WB
{

struct Expr
{
    enum Op { Num, Reg, Var, Mem, Neg, Not, BitNot, Bin } Kind;
    s64 Value = 0;
    int Width = 4;       // Mem: bytes
    bool Signed = false; // Mem
    std::string BinOp;
    std::shared_ptr<Expr> A, B;
};

namespace
{

struct Parser
{
    const char* p;
    std::string err;

    void ws() { while (*p && isspace((unsigned char)*p)) p++; }
    bool accept(const char* tok)
    {
        ws();
        size_t n = strlen(tok);
        if (strncmp(p, tok, n) == 0)
        {
            // do not split "<=" into "<" etc.
            if (n == 1 && (tok[0] == '<' || tok[0] == '>' || tok[0] == '=' || tok[0] == '!') && p[1] == '=') return false;
            if (n == 1 && tok[0] == '<' && p[1] == '<') return false;
            if (n == 1 && tok[0] == '>' && p[1] == '>') return false;
            if (n == 1 && tok[0] == '&' && p[1] == '&') return false;
            if (n == 1 && tok[0] == '|' && p[1] == '|') return false;
            p += n;
            return true;
        }
        return false;
    }

    std::shared_ptr<Expr> bin(const char* op, std::shared_ptr<Expr> a, std::shared_ptr<Expr> b)
    {
        auto e = std::make_shared<Expr>();
        e->Kind = Expr::Bin;
        e->BinOp = op;
        e->A = a;
        e->B = b;
        return e;
    }

    typedef std::shared_ptr<Expr> (Parser::*Level)();

    std::shared_ptr<Expr> chain(Level next, std::initializer_list<const char*> ops)
    {
        auto a = (this->*next)();
        if (!a) return nullptr;
        for (;;)
        {
            const char* found = nullptr;
            for (const char* o : ops)
                if (accept(o)) { found = o; break; }
            if (!found) return a;
            auto b = (this->*next)();
            if (!b) return nullptr;
            a = bin(found, a, b);
        }
    }

    std::shared_ptr<Expr> lor() { return chain(&Parser::land, {"||"}); }
    std::shared_ptr<Expr> land() { return chain(&Parser::bor, {"&&"}); }
    std::shared_ptr<Expr> bor() { return chain(&Parser::bxor, {"|"}); }
    std::shared_ptr<Expr> bxor() { return chain(&Parser::band, {"^"}); }
    std::shared_ptr<Expr> band() { return chain(&Parser::eq, {"&"}); }
    std::shared_ptr<Expr> eq() { return chain(&Parser::rel, {"==", "!="}); }
    std::shared_ptr<Expr> rel() { return chain(&Parser::shift, {"<=", ">=", "<", ">"}); }
    std::shared_ptr<Expr> shift() { return chain(&Parser::add, {"<<", ">>"}); }
    std::shared_ptr<Expr> add() { return chain(&Parser::mul, {"+", "-"}); }
    std::shared_ptr<Expr> mul() { return chain(&Parser::unary, {"*", "/", "%"}); }

    std::shared_ptr<Expr> unary()
    {
        ws();
        if (accept("-")) { auto e = std::make_shared<Expr>(); e->Kind = Expr::Neg; e->A = unary(); return e->A ? e : nullptr; }
        if (accept("!")) { auto e = std::make_shared<Expr>(); e->Kind = Expr::Not; e->A = unary(); return e->A ? e : nullptr; }
        if (accept("~")) { auto e = std::make_shared<Expr>(); e->Kind = Expr::BitNot; e->A = unary(); return e->A ? e : nullptr; }
        return postfix();
    }

    std::shared_ptr<Expr> postfix()
    {
        auto e = primary();
        if (!e) return nullptr;
        if (e->Kind == Expr::Mem)
        {
            ws();
            if (*p == '.')
            {
                const char* save = p;
                p++;
                if (!strncmp(p, "sb", 2)) { e->Width = 1; e->Signed = true; p += 2; }
                else if (!strncmp(p, "sh", 2)) { e->Width = 2; e->Signed = true; p += 2; }
                else if (*p == 'b') { e->Width = 1; p++; }
                else if (*p == 'h') { e->Width = 2; p++; }
                else if (*p == 'w') { e->Width = 4; p++; }
                else p = save;
            }
        }
        return e;
    }

    std::shared_ptr<Expr> primary()
    {
        ws();
        if (accept("("))
        {
            auto e = lor();
            if (!e) return nullptr;
            if (!accept(")")) { err = "expected ')'"; return nullptr; }
            return e;
        }
        if (accept("["))
        {
            auto e = std::make_shared<Expr>();
            e->Kind = Expr::Mem;
            e->A = lor();
            if (!e->A) return nullptr;
            if (!accept("]")) { err = "expected ']'"; return nullptr; }
            return e;
        }
        if (*p == '$' || isdigit((unsigned char)*p))
        {
            auto e = std::make_shared<Expr>();
            e->Kind = Expr::Num;
            char* end;
            // strtoull would also take a sign or spaces, or no digits at all
            const char* hex = *p == '$' ? p + 1 : (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? p + 2 : nullptr;
            if (hex && !isxdigit((unsigned char)*hex)) { err = "expected hex digits after '" + std::string(p, hex) + "'"; return nullptr; }
            if (hex) e->Value = (s64)strtoull(hex, &end, 16);
            else e->Value = (s64)strtoull(p, &end, 10);
            p = end;
            return e;
        }
        if (isalpha((unsigned char)*p) || *p == '_')
        {
            const char* s = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            std::string id(s, p - s);
            for (auto& c : id) c = (char)tolower(c);

            struct { const char* name; int width; bool sign; } types[] = {
                {"u8", 1, false}, {"u16", 2, false}, {"u32", 4, false}, {"s8", 1, true}, {"s16", 2, true}, {"s32", 4, true},
                {"byte", 1, false}, {"half", 2, false}, {"word", 4, false},
            };
            for (auto& t : types)
            {
                if (id == t.name)
                {
                    ws();
                    if (!accept("[")) { err = "expected '[' after " + id; return nullptr; }
                    auto e = std::make_shared<Expr>();
                    e->Kind = Expr::Mem;
                    e->Width = t.width;
                    e->Signed = t.sign;
                    e->A = lor();
                    if (!e->A) return nullptr;
                    if (!accept("]")) { err = "expected ']'"; return nullptr; }
                    return e;
                }
            }

            auto e = std::make_shared<Expr>();
            if (id.size() >= 2 && id[0] == 'r' && id.find_first_not_of("0123456789", 1) == std::string::npos)
            {
                int n = atoi(id.c_str() + 1);
                if (n < 0 || n > 15) { err = "bad register " + id; return nullptr; }
                e->Kind = Expr::Reg; e->Value = n; return e;
            }
            if (id == "sp") { e->Kind = Expr::Reg; e->Value = 13; return e; }
            if (id == "lr") { e->Kind = Expr::Reg; e->Value = 14; return e; }
            if (id == "pc") { e->Kind = Expr::Reg; e->Value = 15; return e; }
            if (id == "cpsr") { e->Kind = Expr::Reg; e->Value = 16; return e; }
            static const char* vars[] = {"value", "addr", "size", "hits", "frame", "cpu"};
            for (int i = 0; i < 6; i++)
                if (id == vars[i]) { e->Kind = Expr::Var; e->Value = i; return e; }
            err = "unknown identifier '" + id + "'";
            return nullptr;
        }
        err = *p ? std::string("unexpected '") + *p + "'" : "unexpected end of expression";
        return nullptr;
    }
};

}

std::shared_ptr<Expr> CompileExpr(const std::string& text, std::string& error)
{
    Parser ps;
    ps.p = text.c_str();
    auto e = ps.lor();
    if (e)
    {
        ps.ws();
        if (*ps.p) { ps.err = std::string("unexpected '") + *ps.p + "'"; e = nullptr; }
    }
    if (!e) error = ps.err.empty() ? "syntax error" : ps.err;
    return e;
}

s64 EvalExpr(const Expr& e, const EvalContext& c)
{
    switch (e.Kind)
    {
    case Expr::Num: return e.Value;
    case Expr::Reg: return (s64)c.WB->ReadReg(c.Cpu, (u32)e.Value);
    case Expr::Var:
        switch (e.Value)
        {
        case 0: return c.AccessValue;
        case 1: return c.AccessAddr;
        case 2: return c.AccessSize;
        case 3: return c.Hits;
        case 4: return c.WB->Frame();
        default: return c.Cpu;
        }
    case Expr::Mem:
        {
            u32 addr = (u32)EvalExpr(*e.A, c);
            u32 v = 0;
            c.WB->Peek(c.Cpu & 1, addr, (u32)e.Width, v);
            if (e.Signed)
            {
                if (e.Width == 1) return (s8)v;
                if (e.Width == 2) return (s16)v;
                return (s32)v;
            }
            return v;
        }
    case Expr::Neg: return (s64)(0 - (u64)EvalExpr(*e.A, c));    // wraps for the most negative value
    case Expr::Not: return !EvalExpr(*e.A, c);
    case Expr::BitNot: return (s64)(~(u32)EvalExpr(*e.A, c));
    case Expr::Bin:
        {
            const std::string& o = e.BinOp;
            if (o == "&&") return EvalExpr(*e.A, c) && EvalExpr(*e.B, c);
            if (o == "||") return EvalExpr(*e.A, c) || EvalExpr(*e.B, c);
            s64 a = EvalExpr(*e.A, c), b = EvalExpr(*e.B, c);
            if (o == "+") return a + b;
            if (o == "-") return a - b;
            if (o == "*") return a * b;
            // the most negative value divided by -1 overflows (a CPU fault): it wraps, like the other operators
            if (o == "/") return b == -1 ? (s64)(0 - (u64)a) : b ? a / b : 0;
            if (o == "%") return b == -1 ? 0 : b ? a % b : 0;
            if (o == "<<") return (s64)((u64)a << (b & 63));
            if (o == ">>") return (s64)((u64)a >> (b & 63));
            if (o == "&") return a & b;
            if (o == "|") return a | b;
            if (o == "^") return a ^ b;
            if (o == "==") return a == b;
            if (o == "!=") return a != b;
            if (o == "<") return a < b;
            if (o == ">") return a > b;
            if (o == "<=") return a <= b;
            if (o == ">=") return a >= b;
            return 0;
        }
    }
    return 0;
}

}
