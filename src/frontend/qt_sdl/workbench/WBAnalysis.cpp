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

// Mark unreached code: the recursive walk in FlowDisasm, seeded from instructions
// the recording has already executed. Bytes it reaches that nothing else has
// classified are kept as marks ("from analysis") until they are cleared or the
// command is run again. A finished snapshot stays across Resume.

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "WBHost.h"
#include "FlowDisasm.h"
#include "WBQueries.h"
#include "Disasm.h"

using namespace melonDS;
using namespace melonDS::WB;

namespace
{

bool RegionForCpu(u32 cpu, u16 region)
{
    if (region == (u16)Region::MainRAM || region == (u16)Region::SharedWRAM || region == (u16)Region::VRAM)
        return true;
    if (cpu == 0)
        return region == (u16)Region::ITCM || region == (u16)Region::BIOS9
            || (region >= (u16)Region::OverlayARM9Base && region < (u16)Region::OverlayARM7Base);
    return region == (u16)Region::ARM7WRAM || region == (u16)Region::BIOS7
        || (region >= (u16)Region::OverlayARM7Base && region < (u16)Region::RegionMax);
}

void MergeRanges(std::vector<std::pair<u32, u32>>& v)
{
    std::sort(v.begin(), v.end());
    std::vector<std::pair<u32, u32>> out;
    for (const auto& r : v)
    {
        if (r.second <= r.first) continue;
        if (!out.empty() && r.first <= out.back().second) out.back().second = std::max(out.back().second, r.second);
        else out.push_back(r);
    }
    v.swap(out);
}

bool InRanges(const std::vector<std::pair<u32, u32>>& v, u32 addr)
{
    auto it = std::lower_bound(v.begin(), v.end(), addr, [](const std::pair<u32, u32>& r, u32 a) { return r.first < a; });
    if (it == v.begin()) return false;
    --it;
    return addr < it->second;
}

QString Hex8(u32 a) { return QString("%1").arg(a, 8, 16, QChar('0')); }

QString ReachWhy(bool thumb, FlowReach reach, u32 from)
{
    QString how;
    switch (reach)
    {
    case FlowReach::Branch: how = "a branch from " + Hex8(from); break;
    case FlowReach::Call: how = "a call from " + Hex8(from); break;
    case FlowReach::AfterCall: how = "the return of the call at " + Hex8(from); break;
    case FlowReach::Continue: how = from ? "falling through from " + Hex8(from) : "continuing from an entry"; break;
    default: how = "an entry that has not executed"; break;
    }
    return QString("%1, from analysis: %2").arg(thumb ? "Thumb" : "ARM", how);
}

// The program the walk adds to. Known instructions and defined data are loaded first,
// so a block that lands on them is skipped or reported instead of marked.
struct Image : FlowImage
{
    Workbench& WB;
    u32 Cpu;
    std::map<u32, FlowInsn> Insns;
    std::vector<std::pair<u32, u32>> Data;
    const std::vector<std::pair<u32, u32>>* Funcs = nullptr;
    int Marked = 0;
    std::function<void(int)> OnProgress;

    Image(Workbench& wb, u32 cpu) : WB(wb), Cpu(cpu) {}

    bool Read(u32 addr, u32 size, u32& out) const override
    {
        if (WB.Peek(Cpu, addr, size, out)) return true;
        // The coverage map shows ITCM at 01FF8000. The CPU runs that memory at the low mirror.
        if (Cpu == 0 && addr >= 0x01FF8000 && addr + size <= 0x02000000)
            return WB.Peek(0, addr - 0x01FF8000, size, out);
        return false;
    }
    bool Restricted(u32 addr) const override
    {
        if (Cpu == 0 && addr >= 0x01FF8000 && addr < 0x02000000) return true;
        PhysAddr pa = WB.Translate(Cpu, addr);
        return pa.Region && RegionForCpu(Cpu, pa.Region);
    }
    bool Initialized(u32 addr) const override
    {
        u32 b = 0;
        return Read(addr, 1, b);
    }
    bool NoReturn(u32) const override { return false; }
    bool InstructionAt(u32 addr, FlowInsn& out) const override
    {
        auto it = Insns.find(addr);
        if (it == Insns.end()) return false;
        out = it->second;
        return true;
    }
    bool Offcut(u32 addr) const override
    {
        auto it = Insns.upper_bound(addr);
        if (it == Insns.begin()) return false;
        --it;
        return addr > it->second.Addr && addr < it->second.Addr + it->second.Size;
    }
    bool DefinedData(u32 addr) const override
    {
        auto it = std::lower_bound(Data.begin(), Data.end(), addr, [](const std::pair<u32, u32>& r, u32 a) { return r.first < a; });
        if (it == Data.begin()) return false;
        --it;
        return addr < it->second;
    }
    void Commit(const std::vector<FlowInsn>& added) override
    {
        for (const FlowInsn& in : added)
        {
            Insns[in.Addr] = in;
            if (!Funcs || !InRanges(*Funcs, in.Addr)) Marked++;
        }
        if (OnProgress) OnProgress(Marked);
    }
};

struct CpuIn
{
    std::vector<std::pair<u32, u32>> Data;
    std::vector<std::pair<u32, u32>> Funcs;
    std::vector<FlowSeed> Entries;     // knowledge-store functions, after the recording's own edges
};

struct CpuOut
{
    std::vector<WBQ::AnalysisHints::Range> Ranges;
    int Instructions = 0;
    int Literals = 0;
    int Notes = 0;
    QString First;
    u32 FirstAddr = 0;
    bool HasFirst = false;
    u32 FirstInsn = 0;
    bool Incomplete = false;
};

CpuOut AnalyzeCpu(Workbench& wb, u32 cpu, const CpuIn& in, std::atomic<bool>* cancel, const std::function<void(int)>& progress)
{
    CpuOut out;
    if (progress) progress(0);
    if (!wb.GetNDS()) return out;
    Image image{wb, cpu};
    image.Data = in.Data;
    image.Funcs = &in.Funcs;
    image.OnProgress = progress;

    std::vector<FlowInsn> runtime;
    wb.Rec.Store().ForEach([&](u16 region, u32 page, const RecordPage* p)
    {
        if (!RegionForCpu(cpu, region)) return;
        u32 base = WBQ::CanonicalAddr(wb, region, page << PageShift);
        u16 execFlag = cpu ? (u16)ByteFlag::Exec7 : (u16)ByteFlag::Exec9;
        for (u32 i = 0; i < PageSize; i++)
        {
            const ByteRecord& rec = p->Rec[i];
            if (!rec.exec_count || !(rec.flags & ByteFlag::InstrStart) || !(rec.flags & execFlag)) continue;
            u32 addr = base + i;
            bool thumb = (rec.flags & ByteFlag::Thumb) && !(rec.flags & ByteFlag::Arm);
            u32 raw = 0, lo = 0;
            if (!image.Read(addr, thumb ? 2u : 4u, lo)) continue;
            raw = lo;
            if (thumb)
            {
                u32 hi = 0;
                if (image.Read(addr + 2, 2, hi)) raw |= (hi & 0xFFFF) << 16;
            }
            DisasmInsn decoded;
            Disassemble(addr, raw, thumb, decoded);
            FlowInsn known;
            known.Addr = addr;
            known.Size = decoded.Size >= 2 ? decoded.Size : (thumb ? 2u : 4u);
            known.Thumb = thumb;
            known.Raw = raw;
            image.Insns[addr] = known;
            runtime.push_back(known);
        }
    });

    // Reads and literal pools the recording already called data, outside any known function.
    // Interior bytes of an executed instruction are that instruction, not data.
    {
        u32 runS = 0, runE = 0;
        auto flush = [&]() { if (runE > runS) image.Data.push_back({runS, runE}); runS = runE = 0; };
        auto add = [&](u32 a)
        {
            FlowInsn tmp;
            if (image.InstructionAt(a, tmp) || image.Offcut(a) || InRanges(in.Funcs, a)) return;
            if (a == runE) { runE = a + 1; return; }
            flush();
            runS = a;
            runE = a + 1;
        };
        wb.Rec.Store().ForEach([&](u16 region, u32 page, const RecordPage* p)
        {
            if (!RegionForCpu(cpu, region)) return;
            u32 base = WBQ::CanonicalAddr(wb, region, page << PageShift);
            u16 readFlag = cpu ? (u16)ByteFlag::Read7 : (u16)ByteFlag::Read9;
            for (u32 i = 0; i < PageSize; i++)
            {
                const ByteRecord& rec = p->Rec[i];
                if (rec.exec_count) continue;
                bool pool = rec.flags & ByteFlag::LiteralPool;
                bool read = rec.read_count && (rec.flags & readFlag);
                if (pool || read) add(base + i);
            }
        });
        flush();
        MergeRanges(image.Data);
    }

    std::vector<FlowSeed> seeds;
    std::unordered_set<u32> seen;
    auto addSeed = [&](FlowSeed s)
    {
        if (!s.Addr || s.Addr < 0x1000) return;
        FlowInsn tmp;
        if (image.InstructionAt(s.Addr, tmp) || image.Offcut(s.Addr) || image.DefinedData(s.Addr)) return;
        if (!image.Restricted(s.Addr)) return;
        if (!seen.insert(s.Addr).second) return;
        seeds.push_back(s);
    };

    // Edges of instructions that actually ran, before any entry point. Their mode wins.
    for (const FlowInsn& known : runtime)
    {
        DisasmInsn decoded;
        Disassemble(known.Addr, known.Raw, known.Thumb, decoded);
        InsnEdges edges = EdgesOf(known.Addr, decoded, false);
        for (const InsnEdges::Target& t : edges.Targets)
            addSeed({t.Addr, t.Thumb, known.Addr, t.Call ? FlowReach::Call : FlowReach::Branch});
        if (edges.Falls)
            addSeed({edges.Next, known.Thumb, known.Addr, FlowReach::Continue});
    }
    u32 pc = wb.CurrentPC(cpu);
    addSeed({pc, (wb.CPSR(cpu) & 0x20) != 0, 0, FlowReach::Entry});
    for (const FlowSeed& s : in.Entries) addSeed(s);
    u32 entry = cpu ? wb.Rom.ARM7Entry : wb.Rom.ARM9Entry;
    addSeed({entry & ~1u, (entry & 1) != 0, 0, FlowReach::Entry});

    FlowDisasmResult walked = DisassembleSet(image, seeds, {}, cancel);
    out.Notes = (int)walked.NoteCount;
    out.Incomplete = walked.Incomplete;
    if (!walked.Notes.empty())
    {
        out.HasFirst = true;
        out.FirstAddr = walked.Notes[0].Addr;
        out.First = Hex8(walked.Notes[0].Addr) + ", " + QString::fromStdString(walked.Notes[0].Text);
    }

    std::unordered_set<u32> literals;
    for (const FlowInsn& inInsn : walked.Added)
    {
        if (!InRanges(in.Funcs, inInsn.Addr))
        {
            WBQ::AnalysisHints::Range r;
            r.Start = inInsn.Addr;
            r.End = inInsn.Addr + (inInsn.Size ? inInsn.Size : 2);
            r.Kind = inInsn.Thumb ? WBQ::CodeKind::Thumb : WBQ::CodeKind::Arm;
            r.Why = ReachWhy(inInsn.Thumb, inInsn.Reach, inInsn.From);
            out.Ranges.push_back(r);
            out.Instructions++;
        }
        if (!inInsn.Literal) continue;
        u32 a = inInsn.LiteralAddr;
        if (!literals.insert(a).second) continue;
        bool free = true;
        for (u32 i = 0; i < 4; i++)
        {
            FlowInsn tmp;
            if (image.InstructionAt(a + i, tmp) || image.Offcut(a + i) || image.DefinedData(a + i) || InRanges(in.Funcs, a + i))
                free = false;
        }
        // also not on top of a mark we just added
        for (const WBQ::AnalysisHints::Range& g : out.Ranges)
            if (a < g.End && a + 4 > g.Start) free = false;
        if (!free || !image.Restricted(a)) continue;
        WBQ::AnalysisHints::Range r;
        r.Start = a;
        r.End = a + 4;
        r.Kind = WBQ::CodeKind::Data;
        r.Why = "Data, from analysis: a literal pool of " + Hex8(inInsn.Addr);
        out.Ranges.push_back(r);
        out.Literals++;
    }
    std::sort(out.Ranges.begin(), out.Ranges.end(), [](const WBQ::AnalysisHints::Range& a, const WBQ::AnalysisHints::Range& b) { return a.Start < b.Start; });
    for (const WBQ::AnalysisHints::Range& r : out.Ranges)
        if (r.Kind == WBQ::CodeKind::Arm || r.Kind == WBQ::CodeKind::Thumb)
        {
            out.FirstInsn = r.Start;
            break;
        }
    return out;
}

QString Summary(const CpuOut& arm9, const CpuOut& arm7)
{
    auto piece = [](const char* name, const CpuOut& o) -> QString
    {
        if (!o.Instructions && !o.Literals) return QString();
        QString s = QString("%1 instruction%2").arg(o.Instructions).arg(o.Instructions == 1 ? "" : "s");
        if (o.Literals) s += QString(" and %1 literal word%2").arg(o.Literals).arg(o.Literals == 1 ? "" : "s");
        return s + " on " + name;
    };
    QString a = piece("ARM9", arm9), b = piece("ARM7", arm7);
    QString text;
    if (a.isEmpty() && b.isEmpty()) text = "Static analysis found no unreached code to mark.";
    else text = "Static analysis marked " + (a.isEmpty() ? b : b.isEmpty() ? a : a + ", and " + b) + " that the recording had not reached.";
    int notes = arm9.Notes + arm7.Notes;
    if (notes)
    {
        QString first = !arm9.First.isEmpty() ? arm9.First : arm7.First;
        text += QString(" %1 place%2 left unmarked").arg(notes).arg(notes == 1 ? " was" : "s were");
        if (!first.isEmpty()) text += "; the first is " + first;
        text += ".";
    }
    if (arm9.Incomplete || arm7.Incomplete) text += " The walk stopped early.";
    return text;
}

}

void WBHost::ReportAnalysisProgress(u32 gen, int cpu, int instructions)
{
    QMetaObject::invokeMethod(this, [this, gen, cpu, instructions]()
    {
        if (gen != AnalysisGen.load() || !AnalysisBusy) return;
        emit analysisProgress(cpu, instructions);
    }, Qt::QueuedConnection);
}

void WBHost::RunStaticAnalysis()
{
    if (AnalysisBusy) return;
    WBStatus st = Status();
    if (st.EmuState != WBStatus::Paused && st.EmuState != WBStatus::Break)
    {
        emit notice("Pause the game first (F5).");
        return;
    }
    if (!WB.GetNDS()) return;
    AnalysisBusy = true;
    AnalysisCancel.store(false);
    u32 gen = ++AnalysisGen;
    emit analysisProgress(0, 0);
    emit statusChanged();

    struct Inputs { CpuIn Cpu[2]; } in;
    for (int cpu = 0; cpu < 2; cpu++)
    {
        for (const WBKB::Function& f : Kb.Functions())
        {
            const WBKB::Module* m = Kb.ModuleById(f.ModuleId);
            int mc = m ? m->CpuNum() : 0;
            if (mc != cpu) continue;
            if (m && m->Kind == "overlay" && m->OverlayId != OverlayLoadedAt(f.Addr)) continue;
            u32 n = std::max<u32>(f.Size, f.Thumb() ? 2u : 4u);
            in.Cpu[cpu].Funcs.push_back({f.Addr, f.Addr + n});
            in.Cpu[cpu].Entries.push_back({f.Addr, f.Thumb(), 0, FlowReach::Entry});
        }
        for (const WBKB::DataObject& d : Kb.DataObjects())
        {
            if (!d.Size) continue;
            const WBKB::Module* m = d.ModuleId >= 0 ? Kb.ModuleById(d.ModuleId) : nullptr;
            if (m && m->CpuNum() != cpu) continue;
            if (!m && cpu != 0) continue;
            if (m && m->Kind == "overlay" && m->OverlayId != OverlayLoadedAt(d.Addr)) continue;
            in.Cpu[cpu].Data.push_back({d.Addr, d.Addr + d.Size});
        }
        for (const WBKB::DecodeOverride& o : Kb.DecodeOverrides())
        {
            if (o.Cpu != cpu || o.Mode != "data") continue;
            in.Cpu[cpu].Data.push_back({o.Start, o.End});
        }
        MergeRanges(in.Cpu[cpu].Data);
        MergeRanges(in.Cpu[cpu].Funcs);
    }

    struct Out { CpuOut Cpu[2]; };
    Query<Out>([this, in, gen]()
    {
        Out r;
        if (gen != AnalysisGen.load() || AnalysisCancel.load() || !WB.GetNDS()) return r;
        for (int cpu = 0; cpu < 2; cpu++)
        {
            if (gen != AnalysisGen.load() || AnalysisCancel.load()) break;
            r.Cpu[cpu] = AnalyzeCpu(WB, (u32)cpu, in.Cpu[cpu], &AnalysisCancel, [this, gen, cpu](int n)
            {
                ReportAnalysisProgress(gen, cpu, n);
            });
        }
        return r;
    }, this, [this, gen](Out r)
    {
        AnalysisBusy = false;
        if (gen != AnalysisGen.load()) { emit statusChanged(); return; }
        WBStatus s = Status();
        if (s.EmuState != WBStatus::Paused && s.EmuState != WBStatus::Break)
        {
            emit notice("Static analysis was discarded because the game resumed.");
            emit statusChanged();
            return;
        }
        Analysis[0] = std::move(r.Cpu[0].Ranges);
        Analysis[1] = std::move(r.Cpu[1].Ranges);
        int noteCpu = -1;
        u32 noteAddr = 0;
        if (r.Cpu[0].HasFirst) { noteCpu = 0; noteAddr = r.Cpu[0].FirstAddr; }
        else if (r.Cpu[1].HasFirst) { noteCpu = 1; noteAddr = r.Cpu[1].FirstAddr; }
        emit analysisChanged();
        emit analysisFinished(Summary(r.Cpu[0], r.Cpu[1]), noteCpu, noteAddr, r.Cpu[0].FirstInsn, r.Cpu[1].FirstInsn);
        emit statusChanged();
    });
}

void WBHost::CancelStaticAnalysis()
{
    if (!AnalysisBusy) return;
    AnalysisCancel.store(true);
    AnalysisGen++;
}

void WBHost::AbandonStaticAnalysis()
{
    if (!AnalysisBusy) return;
    AnalysisCancel.store(true);
    AnalysisGen++;
    emit notice("Static analysis was discarded because the game resumed.");
    emit statusChanged();
}

void WBHost::ClearStaticAnalysis()
{
    AnalysisGen++;
    AnalysisCancel.store(true);
    bool any = HasAnalysis();
    Analysis[0].clear();
    Analysis[1].clear();
    if (any) emit analysisChanged();
    if (AnalysisBusy) emit statusChanged();
}

WBQ::AnalysisHints WBHost::AnalysisNear(int cpu, u32 start, u32 end) const
{
    WBQ::AnalysisHints h;
    if (cpu < 0 || cpu > 1 || start >= end) return h;
    const auto& v = Analysis[cpu];
    auto it = std::lower_bound(v.begin(), v.end(), start, [](const WBQ::AnalysisHints::Range& r, u32 a) { return r.Start < a; });
    if (it != v.begin()) --it;
    for (; it != v.end() && it->Start < end; ++it)
        if (it->End > start) h.Ranges.push_back(*it);
    return h;
}

const std::vector<WBQ::AnalysisHints::Range>& WBHost::AnalysisAll(int cpu) const
{
    static const std::vector<WBQ::AnalysisHints::Range> empty;
    if (cpu < 0 || cpu > 1) return empty;
    return Analysis[cpu];
}
