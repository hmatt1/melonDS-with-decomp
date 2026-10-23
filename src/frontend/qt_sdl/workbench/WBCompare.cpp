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

#include <algorithm>
#include <map>
#include <set>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include "WBCompare.h"
#include "KnowledgeStore.h"
#include "Disasm.h"

namespace WBCmp
{

using namespace melonDS::WB;

// ---------------------------------------------------------------- compare.json

static bool ParseAddr(const QJsonValue& v, u32& out)
{
    if (v.isDouble()) { out = (u32)v.toDouble(); return true; }
    bool ok = false;
    QString s = v.toString().trimmed();
    out = s.startsWith("0x", Qt::CaseInsensitive) ? s.mid(2).toUInt(&ok, 16) : s.toUInt(&ok, 10);
    return ok;
}

static bool ParseRule(const QJsonObject& o, CompareConfig::Rule& r, const QString& where, QString& error)
{
    r.Module = o.value("module").toString();
    r.Compiler = o.value("compiler").toString();
    r.Flags = o.value("flags").toString();
    if (o.contains("context"))
    {
        if (!o.value("context").isBool()) { error = where + ": `context` must be true or false"; return false; }
        r.Context = o.value("context").toBool() ? 1 : 0;
    }
    for (const QJsonValue& rv : o.value("ranges").toArray())
    {
        QJsonArray pair = rv.toArray();
        u32 lo, hi;
        if (pair.size() != 2 || !ParseAddr(pair[0], lo) || !ParseAddr(pair[1], hi) || hi <= lo)
        {
            error = where + ": each range must be [start, end] with end after start";
            return false;
        }
        r.Ranges.push_back({lo, hi});
    }
    return true;
}

bool CompareConfig::Parse(const QByteArray& json, QString& error)
{
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError)
    {
        error = QString("compare.json is not valid JSON at offset %1: %2").arg(pe.offset).arg(pe.errorString());
        return false;
    }
    if (!doc.isObject()) { error = "compare.json must hold one object"; return false; }
    QJsonObject root = doc.object();
    *this = CompareConfig();
    if (!ParseRule(root.value("default").toObject(), Default, "default", error)) return false;
    if (Default.Context < 0) Default.Context = 1;
    QJsonArray mods = root.value("modules").toArray();
    for (int i = 0; i < mods.size(); i++)
    {
        Rule r;
        QString where = QString("modules[%1]").arg(i);
        if (!mods[i].isObject()) { error = where + " must be an object"; return false; }
        if (!ParseRule(mods[i].toObject(), r, where, error)) return false;
        if (r.Module.isEmpty() && r.Ranges.empty()) { error = where + " needs a `module` or `ranges`"; return false; }
        Rules.push_back(r);
    }
    return true;
}

static QJsonObject RuleJson(const CompareConfig::Rule& r, bool isDefault)
{
    QJsonObject o;
    if (!isDefault && !r.Module.isEmpty()) o["module"] = r.Module;
    if (!r.Ranges.empty())
    {
        QJsonArray ranges;
        for (auto& p : r.Ranges)
            ranges.append(QJsonArray{QString("0x%1").arg(p.first, 8, 16, QChar('0')), QString("0x%1").arg(p.second, 8, 16, QChar('0'))});
        o["ranges"] = ranges;
    }
    o["compiler"] = r.Compiler;
    o["flags"] = r.Flags;
    if (r.Context >= 0) o["context"] = r.Context != 0;
    return o;
}

QByteArray CompareConfig::ToJson() const
{
    QJsonObject root;
    root["version"] = 1;
    root["default"] = RuleJson(Default, true);
    QJsonArray mods;
    for (const Rule& r : Rules) mods.append(RuleJson(r, false));
    root["modules"] = mods;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

CompilerChoice CompareConfig::Resolve(const QString& module, u32 addr) const
{
    auto choose = [this](const Rule& r, const char* from)
    {
        CompilerChoice c;
        c.Label = r.Compiler.isEmpty() ? Default.Compiler : r.Compiler;
        c.Flags = r.Flags.isEmpty() && r.Compiler.isEmpty() ? Default.Flags : r.Flags;
        c.Context = (r.Context >= 0 ? r.Context : Default.Context) != 0;
        c.From = from;
        return c;
    };
    for (const Rule& r : Rules)
        for (auto& p : r.Ranges)
            if (addr >= p.first && addr < p.second && (r.Module.isEmpty() || r.Module == module))
                return choose(r, "range");
    for (const Rule& r : Rules)
        if (r.Ranges.empty() && !module.isEmpty() && r.Module == module) return choose(r, "module");
    if (!Default.Compiler.isEmpty()) return choose(Default, "default");
    CompilerChoice none;
    none.Context = Default.Context != 0;
    return none;
}

QByteArray CompareConfig::Template(const QString& firstModule)
{
    CompareConfig c;
    c.Default.Context = 1;
    Rule r;
    r.Module = firstModule.isEmpty() ? QString("arm9") : firstModule;
    r.Compiler = "mwcc-2.0-sp1p5";
    r.Flags = "-O4,p -proc arm946e -interworking -enum int -char signed -str noreuse -nostdinc";
    c.Rules.push_back(r);
    return c.ToJson();
}

// ---------------------------------------------------------------- the command's answer

static QJsonObject FindObject(const QByteArray& out)
{
    QByteArray t = out.trimmed();
    QJsonObject o = QJsonDocument::fromJson(t).object();
    if (!o.isEmpty()) return o;
    // the last line that is a whole object (tools that log before answering)
    QList<QByteArray> lines = t.split('\n');
    for (int i = lines.size() - 1; i >= 0; i--)
    {
        QByteArray l = lines[i].trimmed();
        if (!l.startsWith('{')) continue;
        o = QJsonDocument::fromJson(l).object();
        if (!o.isEmpty()) return o;
    }
    int brace = t.indexOf('{');
    return brace >= 0 ? QJsonDocument::fromJson(t.mid(brace)).object() : QJsonObject();
}

CompareOutput ParseCompareOutput(const QByteArray& stdoutBytes)
{
    CompareOutput r;
    r.Obj = FindObject(stdoutBytes);
    r.Found = !r.Obj.isEmpty();
    if (!r.Found)
    {
        r.Errors << "no JSON object in the output";
        return r;
    }
    const QJsonObject& o = r.Obj;
    bool failed = o.contains("error");
    if (o.contains("score") && !o.value("score").isDouble()) r.Errors << "`score` must be a number";
    else if (!o.contains("score") && !failed) r.Errors << "`score` is missing";
    else if (o.contains("score") && (o.value("score").toDouble() < 0 || o.value("score").toDouble() > 100))
        r.Errors << "`score` must be between 0 and 100";
    if (o.contains("matched") && !o.value("matched").isBool()) r.Errors << "`matched` must be true or false";
    if (o.contains("recorded") && !o.value("recorded").isBool()) r.Errors << "`recorded` must be true or false";
    for (const char* k : {"compiler", "flags", "stage", "log", "error", "engine", "source_file", "commit"})
        if (o.contains(k) && !o.value(k).isString()) r.Errors << QString("`%1` must be a string").arg(k);
    if (o.contains("stage") && o.value("stage").isString())
    {
        static const QStringList stages = {"compile", "extract", "diff", "done"};
        if (!stages.contains(o.value("stage").toString()))
            r.Errors << "`stage` must be one of compile, extract, diff, done";
    }
    if (o.contains("diff"))
    {
        if (!o.value("diff").isArray()) r.Errors << "`diff` must be a list";
        else
        {
            QJsonArray rows = o.value("diff").toArray();
            for (int i = 0; i < rows.size(); i++)
            {
                QJsonObject row = rows[i].toObject();
                bool ok = rows[i].isObject();
                for (const char* k : {"orig", "compiled", "status"})
                    if (ok && row.contains(k) && !row.value(k).isString()) ok = false;
                if (ok && !row.contains("status")) ok = false;
                if (!ok)
                {
                    r.Errors << QString("`diff[%1]` must be an object with orig, compiled and status strings").arg(i);
                    break;
                }
            }
        }
    }
    else if (!failed) r.Errors << "`diff` is missing";
    return r;
}

QString ExplainCompareFailure(const QString& stage, const QString& error, const QString& log)
{
    QString e = error.toLower(), l = log.toLower();
    if (stage == "run")
        return "The compare command could not start. Check the Python path under Build > Compare setup > Settings.";
    if (e.contains("timed out"))
        return "The compare took longer than its timeout. Raise the timeout under Build > Compare setup > Settings, "
               "or check that the compiler isn't waiting for input.";
    if (e.contains("too old"))
        return "objdiff-cli is too old: version 3.8 or newer is needed. Point Build > Compare setup at a newer one.";
    if (stage == "diff" && (e.contains("not found") || e.contains("no objdiff")))
        return "objdiff-cli could not be found. Set its path under Build > Compare setup > Settings.";
    if (e.contains("no compiler is configured"))
        return "No compiler is set for this function's module. Add one under Build > Compare setup > Settings.";
    if (stage == "compile" && e.contains("not found"))
        return "The compiler could not be found. Check the path set for this compiler label under Build > Compare setup > Settings.";
    if (e.contains("no source file"))
        return "This function has no C file yet. Use Open in editor to create one.";
    if (l.contains("mwcincludes") || l.contains("mwlibraries") || l.contains("mwciincludes"))
        return "mwccarm needs its include paths: set the MWCIncludes environment variable, "
               "or add -nostdinc and -I flags for this module in compare.json.";
    if (l.contains("license"))
        return "mwccarm could not find its license file. Set LM_LICENSE_FILE to the license.dat next to the compiler.";
    if (e.contains("is not in the compiled object"))
        return "The compiled object has no function with this name. Make the function's name in the C file "
               "match the knowledge store, or rename the function.";
    if (e.contains("no target object"))
        return "The target object is missing. Run Compare from the workbench, which writes it for each run.";
    if (stage == "compile" && e.contains("compiler failed"))
        return "The C file did not compile. The compiler's messages are below.";
    if (stage == "diff")
        return "objdiff-cli could not diff the two objects. Its output is below.";
    return QString();
}

QString CommandFingerprint(const QString& commandTemplate, const QByteArray& script)
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(commandTemplate.toUtf8());
    h.addData(QByteArray(1, '\0'));
    h.addData(script);
    return QString::fromLatin1(h.result().toHex());
}

std::vector<std::pair<char, QString>> LineDiff(const QString& a, const QString& b)
{
    QStringList x = a.split('\n'), y = b.split('\n');
    // trim the common head and tail so the table stays small
    int head = 0;
    while (head < x.size() && head < y.size() && x[head] == y[head]) head++;
    int tail = 0;
    while (tail < x.size() - head && tail < y.size() - head && x[x.size() - 1 - tail] == y[y.size() - 1 - tail]) tail++;
    int n = x.size() - head - tail, m = y.size() - head - tail;
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (int i = n - 1; i >= 0; i--)
        for (int j = m - 1; j >= 0; j--)
            lcs[i][j] = x[head + i] == y[head + j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    std::vector<std::pair<char, QString>> out;
    for (int i = 0; i < head; i++) out.push_back({' ', x[i]});
    int i = 0, j = 0;
    while (i < n || j < m)
    {
        if (i < n && j < m && x[head + i] == y[head + j]) { out.push_back({' ', x[head + i]}); i++; j++; }
        else if (j < m && (i >= n || lcs[i][j + 1] >= lcs[i + 1][j])) { out.push_back({'+', y[head + j]}); j++; }
        else { out.push_back({'-', x[head + i]}); i++; }
    }
    for (int k = x.size() - tail; k < x.size(); k++) out.push_back({' ', x[k]});
    return out;
}

// ---------------------------------------------------------------- target object

static u32 Rd16(const std::vector<u8>& b, u32 o) { return o + 2 <= b.size() ? (u32)b[o] | ((u32)b[o + 1] << 8) : 0; }
static u32 Rd32(const std::vector<u8>& b, u32 o) { return Rd16(b, o) | (Rd16(b, o + 2) << 16); }

CodeAnalysis AnalyzeCode(const std::vector<u8>& bytes, u32 base, bool thumb, const SymbolResolver& resolve)
{
    CodeAnalysis r;
    const u32 size = (u32)bytes.size();
    std::set<u32> pool;         // offsets of literal-pool words
    auto addSpan = [&r](u32 off, u32 len, bool data)
    {
        if (!r.Spans.empty() && r.Spans.back().Data == data && r.Spans.back().Offset + r.Spans.back().Size == off)
            r.Spans.back().Size += len;
        else r.Spans.push_back({off, len, data});
    };
    u32 off = 0;
    while (off < size)
    {
        if (pool.count(off) && off + 4 <= size)
        {
            addSpan(off, 4, true);
            u32 v = Rd32(bytes, off);
            SymbolRef s;
            if (v >= base && v < base + size)
            {
                CodeReloc rel;
                rel.Offset = off;
                rel.Type = CodeReloc::Abs32;
                rel.Local = true;
                rel.Sym.Addend = v - base;
                r.Relocs.push_back(rel);
            }
            else if (resolve(v, false, s))
            {
                CodeReloc rel;
                rel.Offset = off;
                rel.Type = CodeReloc::Abs32;
                rel.Sym = s;
                r.Relocs.push_back(rel);
            }
            off += 4;
            continue;
        }
        u32 step = thumb ? 2 : 4;
        if (off + step > size) { addSpan(off, size - off, true); break; }
        u32 word = thumb ? (Rd16(bytes, off) | (off + 4 <= size ? Rd16(bytes, off + 2) << 16 : 0)) : Rd32(bytes, off);
        DisasmInsn in;
        Disassemble(base + off, word, thumb, in);
        u32 len = in.Size >= 2 ? in.Size : step;
        if (off + len > size) len = step;
        if (in.LiteralLoad && in.HasRef && in.Ref >= base + off && in.Ref + 4 <= base + size)
            pool.insert((in.Ref - base) & ~3u);
        bool external = in.HasTarget && (in.Target < base || in.Target >= base + size || in.Target == base);
        if (in.Valid && external && (in.IsCall || (in.IsBranch && !thumb && !in.Conditional && !in.IsReturn)))
        {
            SymbolRef s;
            if (resolve(in.Target | (in.TargetThumb ? 1u : 0u), true, s) && s.Function)
            {
                CodeReloc rel;
                rel.Offset = off;
                rel.Type = in.IsCall ? CodeReloc::Call : CodeReloc::Jump;
                rel.Thumb = thumb;
                rel.Sym = s;
                r.Relocs.push_back(rel);
            }
        }
        addSpan(off, len, false);
        off += len;
    }
    return r;
}

namespace
{

struct Buf
{
    QByteArray B;
    void U8(u32 v) { B.append((char)v); }
    void U16(u32 v) { U8(v & 0xFF); U8(v >> 8); }
    void U32(u32 v) { U16(v & 0xFFFF); U16(v >> 16); }
    void Align(int a) { while (B.size() % a) U8(0); }
};

struct StrTab
{
    QByteArray B = QByteArray(1, '\0');
    std::map<QByteArray, u32> At;
    u32 Add(const QString& s)
    {
        QByteArray k = s.toUtf8();
        auto it = At.find(k);
        if (it != At.end()) return it->second;
        u32 o = (u32)B.size();
        B.append(k);
        B.append('\0');
        At[k] = o;
        return o;
    }
};

enum : u32
{
    R_ARM_ABS32 = 2, R_ARM_THM_CALL = 10, R_ARM_CALL = 28, R_ARM_JUMP24 = 29,
    STB_LOCAL = 0, STB_GLOBAL = 1, STT_NOTYPE = 0, STT_FUNC = 2, STT_SECTION = 3,
};

struct Sym { u32 Name, Value, Size; u8 Info; u16 Shndx; };

}

QByteArray WriteTargetObject(const QString& symbol, const std::vector<u8>& bytes, bool thumb, const CodeAnalysis& code)
{
    std::vector<u8> text = bytes;
    StrTab str;
    std::vector<Sym> syms;
    syms.push_back({0, 0, 0, 0, 0});
    syms.push_back({0, 0, 0, (u8)((STB_LOCAL << 4) | STT_SECTION), 1});    // .text, for jump-table words
    for (const CodeAnalysis::Span& s : code.Spans)
        syms.push_back({str.Add(s.Data ? "$d" : (thumb ? "$t" : "$a")), s.Offset, 0, (u8)((STB_LOCAL << 4) | STT_NOTYPE), 1});
    if (code.Spans.empty()) syms.push_back({str.Add(thumb ? "$t" : "$a"), 0, 0, (u8)((STB_LOCAL << 4) | STT_NOTYPE), 1});
    u32 firstGlobal = (u32)syms.size();
    u32 selfIdx = firstGlobal;
    syms.push_back({str.Add(symbol), thumb ? 1u : 0u, (u32)bytes.size(), (u8)((STB_GLOBAL << 4) | STT_FUNC), 1});

    std::map<QString, u32> undef;
    struct Rel { u32 Offset, Info; };
    std::vector<Rel> rels;
    for (const CodeReloc& c : code.Relocs)
    {
        u32 idx;
        if (c.Local) idx = 1;
        else if (c.Sym.Name == symbol) idx = selfIdx;
        else
        {
            auto it = undef.find(c.Sym.Name);
            if (it == undef.end())
            {
                idx = (u32)syms.size();
                syms.push_back({str.Add(c.Sym.Name), 0, 0, (u8)((STB_GLOBAL << 4) | STT_NOTYPE), 0});
                undef[c.Sym.Name] = idx;
            }
            else idx = it->second;
        }
        u32 o = c.Offset;
        u32 type = R_ARM_ABS32;
        // the in-place field holds the addend (REL), as an assembler leaves it before linking
        if (c.Type == CodeReloc::Abs32)
        {
            u32 v = c.Sym.Addend;
            text[o] = v & 0xFF; text[o + 1] = (v >> 8) & 0xFF; text[o + 2] = (v >> 16) & 0xFF; text[o + 3] = v >> 24;
        }
        else if (c.Thumb)
        {
            type = R_ARM_THM_CALL;
            // bl (a blx to ARM code is still emitted as bl and turned into blx by the linker)
            text[o] = 0xFF; text[o + 1] = 0xF7; text[o + 2] = 0xFE; text[o + 3] = 0xFF;
        }
        else
        {
            u32 w = Rd32(text, o);
            if ((w >> 28) == 0xF) w = 0xEB000000;     // blx imm -> bl
            w = (w & 0xFF000000) | 0x00FFFFFE;
            type = c.Type == CodeReloc::Call ? R_ARM_CALL : R_ARM_JUMP24;
            text[o] = w & 0xFF; text[o + 1] = (w >> 8) & 0xFF; text[o + 2] = (w >> 16) & 0xFF; text[o + 3] = w >> 24;
        }
        rels.push_back({o, (idx << 8) | type});
    }

    StrTab shstr;
    u32 nText = shstr.Add(".text"), nRel = shstr.Add(".rel.text"), nSym = shstr.Add(".symtab"),
        nStr = shstr.Add(".strtab"), nShstr = shstr.Add(".shstrtab");

    Buf f;
    f.B.resize(52);
    u32 textOff = (u32)f.B.size();
    f.B.append((const char*)text.data(), (int)text.size());
    f.Align(4);
    u32 relOff = (u32)f.B.size();
    for (const Rel& r : rels) { f.U32(r.Offset); f.U32(r.Info); }
    u32 symOff = (u32)f.B.size();
    for (const Sym& s : syms) { f.U32(s.Name); f.U32(s.Value); f.U32(s.Size); f.U8(s.Info); f.U8(0); f.U16(s.Shndx); }
    u32 strOff = (u32)f.B.size();
    f.B.append(str.B);
    u32 shstrOff = (u32)f.B.size();
    f.B.append(shstr.B);
    f.Align(4);
    u32 shOff = (u32)f.B.size();
    auto sh = [&f](u32 name, u32 type, u32 flags, u32 off, u32 size, u32 link, u32 info, u32 align, u32 entsize)
    {
        f.U32(name); f.U32(type); f.U32(flags); f.U32(0); f.U32(off); f.U32(size);
        f.U32(link); f.U32(info); f.U32(align); f.U32(entsize);
    };
    sh(0, 0, 0, 0, 0, 0, 0, 0, 0);
    sh(nText, 1, 0x6, textOff, (u32)text.size(), 0, 0, 4, 0);                        // 1 .text: PROGBITS, AX
    sh(nRel, 9, 0x40, relOff, (u32)rels.size() * 8, 3, 1, 4, 8);                     // 2 .rel.text: REL, INFO_LINK
    sh(nSym, 2, 0, symOff, (u32)syms.size() * 16, 4, firstGlobal, 4, 16);            // 3 .symtab
    sh(nStr, 3, 0, strOff, (u32)str.B.size(), 0, 0, 1, 0);                           // 4 .strtab
    sh(nShstr, 3, 0, shstrOff, (u32)shstr.B.size(), 0, 0, 1, 0);                     // 5 .shstrtab

    Buf h;
    h.B.append("\x7f" "ELF", 4);
    h.U8(1); h.U8(1); h.U8(1); h.U8(0);              // ELFCLASS32, little-endian, version 1, SYSV
    while (h.B.size() < 16) h.U8(0);
    h.U16(1);                                        // ET_REL
    h.U16(40);                                       // EM_ARM
    h.U32(1);
    h.U32(0);                                        // entry
    h.U32(0);                                        // phoff
    h.U32(shOff);
    h.U32(0x05000000);                               // EABI version 5
    h.U16(52); h.U16(0); h.U16(0);                   // ehsize, phentsize, phnum
    h.U16(40); h.U16(6); h.U16(5);                   // shentsize, shnum, shstrndx
    f.B.replace(0, 52, h.B);
    return f.B;
}

// ---------------------------------------------------------------- context header

static QSet<QString> Identifiers(const QString& text)
{
    static const QRegularExpression re("\\b[A-Za-z_][A-Za-z0-9_]*\\b");
    QSet<QString> out;
    auto it = re.globalMatch(text);
    while (it.hasNext()) out.insert(it.next().captured(0));
    return out;
}

static QString Terminated(QString s)
{
    s = s.trimmed();
    if (!s.endsWith(';')) s += ';';
    return s;
}

QString GenerateContext(const WBKB::KnowledgeStore& kb, const std::vector<const WBKB::Function*>& own, const QString& title,
                        const CodeAnalysis& code)
{
    const std::map<QString, WBKB::Type>& types = kb.Types();
    std::vector<const WBKB::Function*> funcs;
    std::vector<const WBKB::DataObject*> data;
    std::set<qint64> seenF, seenD;
    for (const WBKB::Function* f : own) seenF.insert(f->Id);
    for (const CodeReloc& r : code.Relocs)
    {
        if (r.Sym.FunctionId && seenF.insert(r.Sym.FunctionId).second)
            if (const WBKB::Function* f = kb.FunctionById(r.Sym.FunctionId)) funcs.push_back(f);
        if (r.Sym.DataObjectId && seenD.insert(r.Sym.DataObjectId).second)
            for (const WBKB::DataObject& d : kb.DataObjects())
                if (d.Id == r.Sym.DataObjectId) { data.push_back(&d); break; }
    }

    // the types those declarations name, and the types those types name
    QSet<QString> want;
    auto addFrom = [&](const QString& text)
    {
        for (const QString& id : Identifiers(text))
            if (types.count(id)) want.insert(id);
    };
    for (const WBKB::Function* f : own) addFrom(f->Signature);
    for (const WBKB::Function* f : funcs) addFrom(f->Signature);
    for (const WBKB::DataObject* d : data) addFrom(d->TypeName);
    std::map<QString, QStringList> deps;
    QStringList queue(want.begin(), want.end());
    while (!queue.isEmpty())
    {
        QString t = queue.takeLast();
        if (deps.count(t)) continue;
        QStringList d;
        for (const QString& id : Identifiers(types.at(t).Definition))
            if (id != t && types.count(id)) { d << id; if (!want.contains(id)) { want.insert(id); queue << id; } }
        d.sort();
        deps[t] = d;
    }
    // dependency order; a cycle (through pointers) is broken where it closes
    QStringList order;
    std::map<QString, int> state;
    std::function<void(const QString&)> visit = [&](const QString& t)
    {
        if (state[t]) return;
        state[t] = 1;
        for (const QString& d : deps[t]) visit(d);
        state[t] = 2;
        order << t;
    };
    QStringList names(want.begin(), want.end());
    names.sort();
    for (const QString& t : names) visit(t);

    static const char* basic[][2] = {
        {"u8", "unsigned char"}, {"s8", "signed char"}, {"u16", "unsigned short"}, {"s16", "signed short"},
        {"u32", "unsigned long"}, {"s32", "signed long"}, {"u64", "unsigned long long"}, {"s64", "signed long long"},
        {"vu8", "volatile u8"}, {"vu16", "volatile u16"}, {"vu32", "volatile u32"},
        {"vs8", "volatile s8"}, {"vs16", "volatile s16"}, {"vs32", "volatile s32"},
        {"f32", "float"}, {"BOOL", "int"}, {"fx16", "s16"}, {"fx32", "s32"},
    };
    QString out;
    out += QString("/* Context for %1, generated by the DS Decomp Workbench from the knowledge store:\n"
                   "   the types, prototypes and globals its C file references. Rewritten on every Compare. */\n").arg(title);
    out += "#ifndef DSWB_CONTEXT_H\n#define DSWB_CONTEXT_H\n\n";
    out += "#ifndef DSWB_NO_BASIC_TYPES\n";
    for (auto& b : basic)
        if (!types.count(b[0])) out += QString("typedef %1 %2;\n").arg(b[1], b[0]);
    out += "#endif\n";
    static const QRegularExpression plainStruct("^\\s*(struct|union)\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\{");
    QString fwd;
    for (const QString& t : order)
    {
        QRegularExpressionMatch m = plainStruct.match(types.at(t).Definition);
        // a plain struct definition: its tag is a type name too, as the knowledge store uses it
        if (m.hasMatch()) fwd += QString("typedef %1 %2 %2;\n").arg(m.captured(1), m.captured(2));
    }
    if (!fwd.isEmpty()) out += "\n" + fwd;
    if (!order.isEmpty())
    {
        out += "\n";
        for (const QString& t : order)
        {
            const WBKB::Type& ty = types.at(t);
            out += Terminated(ty.Definition.isEmpty() ? QString("/* %1: no definition */").arg(t) : ty.Definition) + "\n";
        }
    }
    if (!data.empty())
    {
        out += "\n";
        for (const WBKB::DataObject* d : data)
        {
            const WBKB::Type* ty = d->TypeName.isEmpty() ? nullptr : kb.TypeByName(d->TypeName);
            if (d->TypeName.isEmpty())
                out += QString("extern unsigned char %1[%2];\n").arg(d->Name).arg(std::max<u32>(d->Size, 1));
            else if (ty && ty->Size > 0 && d->Size > (u32)ty->Size && d->Size % ty->Size == 0)
                out += QString("extern %1 %2[%3];\n").arg(d->TypeName, d->Name).arg(d->Size / ty->Size);
            else out += QString("extern %1 %2;\n").arg(d->TypeName, d->Name);
        }
    }
    out += "\n";
    for (const WBKB::Function* f : funcs)
    {
        QString name = f->Name.isEmpty() ? QString("sub_%1").arg(f->Addr, 8, 16, QChar('0')) : f->Name;
        if (f->Signature.trimmed().isEmpty()) out += QString("/* %1: no signature in the knowledge store */\n").arg(name);
        else out += Terminated(f->Signature) + "\n";
    }
    // the file's own functions last, so they can call each other in any order
    for (const WBKB::Function* f : own)
        if (!f->Signature.trimmed().isEmpty()) out += Terminated(f->Signature) + "\n";
    out += "\n#endif\n";
    return out;
}

}
