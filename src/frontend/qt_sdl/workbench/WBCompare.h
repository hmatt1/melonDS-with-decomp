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

#ifndef WB_COMPARE_H
#define WB_COMPARE_H

#include <functional>
#include <vector>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include "types.h"

// Function compare (Component 5 of the vision): what the workbench prepares before it runs the
// configured compare command, and how it reads the answer. The workbench never compiles anything
// itself; the command (tools/workbench/compare/compare.py, copied into the project) does.
//
// compare.json  per game, in the project folder, safe to share: which compiler version label and
//               flags each module (or address range) uses. The label's executable path is local to
//               the machine (Workbench.Compilers in melonDS.toml).
// target object the original function as an ELF relocatable: its bytes, its symbol, mapping
//               symbols, and the relocations that can be inferred (calls to known functions,
//               literal-pool words that point at known functions, labels or data), so the diff
//               engine compares symbols, not addresses.
// context       a header with the types, prototypes and globals the function references, from the
//               knowledge store, so the C file compiles without hand-written declarations.

namespace WBKB { class KnowledgeStore; struct Function; struct DataObject; }

namespace WBCmp
{

using namespace melonDS;

// ---------------------------------------------------------------- compare.json

struct CompilerChoice
{
    QString Label;          // compiler version label ("mwcc-2.0-sp1p5"); its path is a local setting
    QString Flags;
    bool Context = true;    // prefix the generated context header
    QString From;           // which rule chose it: "range", "module", "default", or "" when none did
};

struct CompareConfig
{
    struct Rule
    {
        QString Module;     // module name (arm9, ov003, ...); empty for a range-only rule
        std::vector<std::pair<u32, u32>> Ranges;    // absolute [start, end)
        QString Compiler, Flags;
        int Context = -1;   // -1: inherit the default
    };
    Rule Default;
    std::vector<Rule> Rules;

    bool Parse(const QByteArray& json, QString& error);
    QByteArray ToJson() const;
    // range rules first, then module rules, then the default
    CompilerChoice Resolve(const QString& module, u32 addr) const;
    // the starting file for a new project
    static QByteArray Template(const QString& firstModule);
};

// ---------------------------------------------------------------- the command's answer

struct CompareOutput
{
    bool Found = false;         // stdout held a JSON object
    QJsonObject Obj;
    QStringList Errors;         // contract errors ("`diff` must be a list")
};

// Finds the JSON object in a compare command's stdout and checks it against the contract:
// {score, matched, compiler, flags, diff: [{orig, compiled, status}], recorded}, plus the optional
// stage, log, error, engine and source_file.
CompareOutput ParseCompareOutput(const QByteArray& stdoutBytes);

// One sentence and a next step for a known failure, or empty. stage is the script's (compile,
// extract, diff, done) or "run" when the command itself could not run.
QString ExplainCompareFailure(const QString& stage, const QString& error, const QString& log);

// sha1 of the command template and the script it runs: Compare asks before the first run of a
// command it hasn't seen
QString CommandFingerprint(const QString& commandTemplate, const QByteArray& script);

// line diff of two texts (Show changes from template): ' ' kept, '-' only in a, '+' only in b
std::vector<std::pair<char, QString>> LineDiff(const QString& a, const QString& b);

// ---------------------------------------------------------------- target object

struct SymbolRef
{
    QString Name;
    u32 Addend = 0;
    bool Function = false;
    qint64 FunctionId = 0;      // the referenced function or data object, for the context header
    qint64 DataObjectId = 0;
};

// what an address is called in the knowledge store; false for an unknown address
using SymbolResolver = std::function<bool(u32 addr, bool call, SymbolRef& out)>;

struct CodeReloc
{
    enum Kind { Call, Jump, Abs32 };
    u32 Offset = 0;             // from the function's start
    Kind Type = Abs32;
    bool Thumb = false;         // Call/Jump: a Thumb instruction
    SymbolRef Sym;
    bool Local = false;         // Abs32 into the function itself (a jump table): .text-relative
};

struct CodeAnalysis
{
    struct Span { u32 Offset, Size; bool Data; };
    std::vector<Span> Spans;    // code and literal-pool runs, in order
    std::vector<CodeReloc> Relocs;
};

// Walks the function's instructions and literal pools and infers its relocations.
CodeAnalysis AnalyzeCode(const std::vector<u8>& bytes, u32 base, bool thumb, const SymbolResolver& resolve);

// A 32-bit little-endian ARM ELF relocatable holding the function as `symbol`.
QByteArray WriteTargetObject(const QString& symbol, const std::vector<u8>& bytes, bool thumb, const CodeAnalysis& code);

// ---------------------------------------------------------------- context header

// own: the functions defined in the C file (the compared one first); code: what they reference
QString GenerateContext(const WBKB::KnowledgeStore& kb, const std::vector<const WBKB::Function*>& own, const QString& title,
                        const CodeAnalysis& code);

}

#endif
