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
#include <QRegularExpression>
#include "WBClassify.h"
#include "KnowledgeStore.h"

namespace WBQ
{

using namespace melonDS;
using namespace melonDS::WB;

QString KindName(CodeKind k)
{
    switch (k)
    {
    case CodeKind::Arm: return "arm";
    case CodeKind::Thumb: return "thumb";
    case CodeKind::Mixed: return "mixed";
    case CodeKind::Data: return "data";
    default: return "unknown";
    }
}

QString EvidenceName(Evidence e)
{
    switch (e)
    {
    case Evidence::Recorded: return "confirmed";
    case Evidence::Override: return "override";
    case Evidence::Defined: return "defined";
    case Evidence::Function: return "inferred";
    case Evidence::Analysis: return "from analysis";
    default: return "none";
    }
}

QChar BadgeLetter(CodeKind k)
{
    switch (k)
    {
    case CodeKind::Arm: return 'A';
    case CodeKind::Thumb: return 'T';
    case CodeKind::Mixed: return 'M';
    case CodeKind::Data: return 'D';
    default: return '?';
    }
}

// "ARM", "Thumb", "Mixed", "Data", "Unknown": the start of a tooltip
static QString Title(CodeKind k)
{
    switch (k)
    {
    case CodeKind::Arm: return "ARM";
    case CodeKind::Thumb: return "Thumb";
    case CodeKind::Mixed: return "Mixed";
    case CodeKind::Data: return "Data";
    default: return "Unknown";
    }
}

KbHints KbHints::Collect(const WBKB::KnowledgeStore& kb, int cpu, u32 start, u32 end, int overlay)
{
    KbHints h;
    // a module belongs if it is on this CPU, and is the loaded overlay when it is one
    auto moduleOk = [&](int moduleId)
    {
        if (moduleId < 0) return true;
        const WBKB::Module* m = kb.ModuleById(moduleId);
        if (!m || m->CpuNum() != cpu) return false;
        return m->Kind != "overlay" || overlay < 0 || m->OverlayId == overlay;
    };
    for (const WBKB::Function& f : kb.Functions())
    {
        u32 fe = f.Addr + std::max<u32>(f.Size, f.Thumb() ? 2 : 4);
        if (fe <= start || f.Addr >= end || !moduleOk(f.ModuleId)) continue;
        h.Functions.push_back({f.Addr, fe, f.Thumb() ? CodeKind::Thumb : CodeKind::Arm, f.Name});
    }
    for (const WBKB::DataObject& d : kb.DataObjects())
    {
        u32 de = d.Addr + std::max<u32>(d.Size, 1);
        if (de <= start || d.Addr >= end || !moduleOk(d.ModuleId)) continue;
        h.Data.push_back({d.Addr, de, CodeKind::Data, d.Name.isEmpty() ? d.TypeName : d.Name});
    }
    for (const WBKB::DecodeOverride& o : kb.DecodeOverrides())
    {
        if (o.Cpu != cpu || o.End <= start || o.Start >= end || !moduleOk(o.ModuleId)) continue;
        CodeKind k = o.Mode == "thumb" ? CodeKind::Thumb : o.Mode == "data" ? CodeKind::Data : CodeKind::Arm;
        h.Overrides.push_back({o.Start, o.End, k, o.Author});
    }
    return h;
}

const KbHints::Range* KbHints::Find(const std::vector<Range>& v, u32 addr)
{
    // the innermost (smallest) range wins: a function nested in a larger one, a field in a struct
    const Range* best = nullptr;
    for (const Range& r : v)
        if (addr >= r.Start && addr < r.End && (!best || r.End - r.Start < best->End - best->Start)) best = &r;
    return best;
}

std::vector<DataUnit> DataUnits(const WBKB::KnowledgeStore& kb, int cpu, u32 start, u32 end, int overlay)
{
    std::vector<DataUnit> out;
    auto scalar = [](const QString& ctype) -> u32
    {
        QString t = ctype.toLower();
        if (t.contains("64") || t == "double" || t == "f64") return 8;
        if (t == "wchar_t" || t.contains("char16")) return 2;
        if (t.contains("char32")) return 4;
        if (t.contains("char") || t.contains("u8") || t.contains("s8") || t == "bool") return 1;
        if (t.contains("16") || t.contains("short")) return 2;
        return 4;
    };
    for (const WBKB::DataObject& d : kb.DataObjects())
    {
        // "T[N]": N elements of T; the object's size, when not given, comes from the type
        QString tn = d.TypeName.trimmed();
        u32 count = 1;
        QRegularExpressionMatch am = QRegularExpression("^(.*)\\[(\\d+)\\]$").match(tn);
        if (am.hasMatch()) { tn = am.captured(1).trimmed(); count = std::max(am.captured(2).toUInt(), 1u); }
        const WBKB::Type* t = kb.TypeByName(tn);
        bool str = !t && tn.contains("char");
        u32 esz = t && t->Size > 0 ? (u32)t->Size : str ? 1 : scalar(tn);
        u32 dsize = d.Size ? d.Size : esz * count;
        if (d.Addr >= end || d.Addr + dsize <= start) continue;
        if (kb.DataObjectAt(cpu, d.Addr, overlay) != &d) continue;     // another CPU, or an overlay not loaded
        QString base = d.Name.isEmpty() ? QString("data_%1").arg(d.Addr, 8, 16, QChar('0')) : d.Name;
        if (t && !t->Fields.empty())
        {
            u32 n = t->Size > 0 ? std::min<u32>(std::max<u32>(dsize / (u32)t->Size, 1), 64) : 1;
            for (u32 k = 0; k < n; k++)
            {
                u32 eaddr = d.Addr + k * (t->Size > 0 ? (u32)t->Size : 0);
                QString ebase = n > 1 ? base + QString("[%1]").arg(k) : base;
                for (const WBKB::TypeField& f : t->Fields)
                {
                    bool fstr = f.CType.contains("char") && f.Count > 1;
                    u32 fsz = std::max<u32>(f.Size, 1);
                    if (fstr) { out.push_back({eaddr + f.Offset, fsz * f.Count, true, ebase + "." + f.Name}); continue; }
                    if (fsz > 4) continue;           // a nested struct: its bytes show as words
                    for (u32 i = 0; i < std::min<u32>(f.Count, 256); i++)
                        out.push_back({eaddr + f.Offset + i * fsz, fsz, false,
                                       ebase + "." + f.Name + (f.Count > 1 ? QString("[%1]").arg(i) : QString())});
                }
            }
        }
        else if (str) out.push_back({d.Addr, dsize, true, base});
        else
            for (u32 o = 0, i = 0; o < dsize && i < 1024; o += esz, i++)
                out.push_back({d.Addr + o, std::min(esz, dsize - o), false, dsize > esz ? base + QString("[%1]").arg(i) : base});
    }
    std::sort(out.begin(), out.end(), [](const DataUnit& x, const DataUnit& y) { return x.Addr < y.Addr; });
    return out;
}

Classification Classify(const ByteRecord* rec, const KbHints& kb, u32 addr, const AnalysisHints* analysis)
{
    Classification c;
    u32 exec = rec ? rec->exec_count : 0;
    u16 fl = rec ? rec->flags : 0;
    bool thumb = exec && (fl & ByteFlag::Thumb), arm = exec && (fl & ByteFlag::Arm);
    bool pool = rec && !exec && (fl & ByteFlag::LiteralPool);
    bool read = rec && !exec && rec->read_count > 0;
    const KbHints::Range* ov = KbHints::Find(kb.Overrides, addr);
    const KbHints::Range* fn = KbHints::Find(kb.Functions, addr);
    const KbHints::Range* data = KbHints::Find(kb.Data, addr);

    // what the recording says, if anything
    CodeKind rk = CodeKind::Unknown;
    QString rwhy;
    if (thumb && arm) { rk = CodeKind::Mixed; rwhy = QString("executed %1 times in both ARM and Thumb mode").arg(exec); }
    else if (thumb) { rk = CodeKind::Thumb; rwhy = QString("executed %1 times as Thumb").arg(exec); }
    else if (arm) { rk = CodeKind::Arm; rwhy = QString("executed %1 times as ARM").arg(exec); }
    else if (pool) { rk = CodeKind::Data; rwhy = "a literal pool: loaded by PC-relative instructions, never executed"; }
    else if (read && !fn) { rk = CodeKind::Data; rwhy = QString("read %1 times, never executed").arg(rec->read_count); }

    if (ov)
    {
        // a person's or an agent's word decides how it is decoded; disagreeing evidence is flagged
        c.Kind = ov->Kind;
        c.From = Evidence::Override;
        c.Why = QString("%1, set by %2").arg(Title(ov->Kind), ov->Name.isEmpty() ? QString("an override") : ov->Name);
        if (rk != CodeKind::Unknown && rk != ov->Kind && !(rk == CodeKind::Mixed && ov->Kind != CodeKind::Data))
        {
            c.Conflict = true;
            c.Why += "; but the recording says " + rwhy;
        }
        return c;
    }
    if (rk != CodeKind::Unknown)
    {
        c.Kind = rk;
        c.From = Evidence::Recorded;
        c.Why = Title(rk) + ", confirmed: " + rwhy;
        return c;
    }
    if (data)
    {
        c.Kind = CodeKind::Data;
        c.From = Evidence::Defined;
        c.Why = "Data, defined: the data object " + (data->Name.isEmpty() ? QString("here") : data->Name);
        return c;
    }
    if (fn)
    {
        c.Kind = fn->Kind;
        c.From = Evidence::Function;
        c.Why = QString("%1, inferred from the function %2; never executed").arg(Title(fn->Kind), fn->Name.isEmpty() ? QString("here") : fn->Name);
        if (read) c.Why += QString(", but read %1 times").arg(rec->read_count);
        return c;
    }
    if (analysis)
        if (const AnalysisHints::Range* g = AnalysisHints::Find(analysis->Ranges, addr))
        {
            c.Kind = g->Kind;
            c.From = Evidence::Analysis;
            c.Why = g->Why;
            return c;
        }
    c.Why = rec ? "Unknown: recorded, but never executed or read" : "Unknown: nothing recorded here";
    return c;
}

const AnalysisHints::Range* AnalysisHints::Find(const std::vector<Range>& v, u32 addr)
{
    auto it = std::lower_bound(v.begin(), v.end(), addr, [](const Range& r, u32 a) { return r.Start < a; });
    // lower_bound stops on the range that starts here, so check it before stepping back
    if (it != v.end() && addr >= it->Start && addr < it->End) return &*it;
    if (it == v.begin()) return nullptr;
    --it;
    if (addr >= it->Start && addr < it->End) return &*it;
    return nullptr;
}

}
