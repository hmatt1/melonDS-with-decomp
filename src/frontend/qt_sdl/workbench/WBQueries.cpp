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
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <QStringDecoder>
#include <QStringEncoder>
#include "WBQueries.h"
#include "NDS.h"
#include "ARM.h"
#include "GPU.h"
#ifdef OGLRENDERER_ENABLED
#include "OpenGLSupport.h"
#endif

using namespace melonDS;
using namespace melonDS::WB;

namespace WBQ
{

bool ParseCpu(const QString& s, u32& cpu, QString& err)
{
    QString l = s.toLower();
    if (l.isEmpty() || l == "arm9" || l == "9") { cpu = 0; return true; }
    if (l == "arm7" || l == "7") { cpu = 1; return true; }
    err = "cpu must be arm9 or arm7";
    return false;
}

QString CpuName(u32 cpu)
{
    switch (cpu & 0xF)
    {
    case 0: return "arm9";
    case 1: return "arm7";
    case 2: return "dma9";
    case 3: return "dma7";
    case 4: return "card";
    default: return "boot";
    }
}

QString ModeName(u32 cpsr)
{
    switch (cpsr & 0x1F)
    {
    case 0x10: return "usr";
    case 0x11: return "fiq";
    case 0x12: return "irq";
    case 0x13: return "svc";
    case 0x17: return "abt";
    case 0x1B: return "und";
    case 0x1F: return "sys";
    default: return "???";
    }
}

QString FlagNames(u16 flags, QStringList* list)
{
    QStringList l;
    for (int i = 0; i < ByteFlag::Count; i++)
        if (flags & ByteFlag::Values[i]) l << ByteFlag::Names[i];
    if (list) *list = l;
    return l.join(",");
}

u32 CanonicalAddr(Workbench& wb, u16 region, u32 off)
{
    if (IsOverlayRegion(region)) return 0x02000000 + off;
    switch ((Region)region)
    {
    case Region::MainRAM: return 0x02000000 + off;
    case Region::ITCM: return 0x01FF8000 + off;
    case Region::DTCM: return (wb.GetNDS() ? wb.GetNDS()->ARM9.DTCMBase : 0x027E0000) + off;
    case Region::SharedWRAM: return 0x03000000 + off;
    case Region::ARM7WRAM: return 0x03800000 + off;
    case Region::VRAM: return 0x06800000 + off;
    case Region::Palette: return 0x05000000 + off;
    case Region::OAM: return 0x07000000 + off;
    case Region::BIOS9: return 0xFFFF0000 + off;
    case Region::BIOS7: return off;
    default: return off;
    }
}

WBApi::RegisterSet Registers(Workbench& wb, u32 cpu)
{
    WBApi::RegisterSet r;
    r.cpu = CpuName(cpu);
    NDS* nds = wb.GetNDS();
    if (!nds) return r;
    const ARM& a = cpu ? (const ARM&)nds->ARM7 : (const ARM&)nds->ARM9;
    for (int i = 0; i < 16; i++) r.r.push_back(i == 15 ? wb.CurrentPC(cpu) : a.R[i]);
    r.cpsr = a.CPSR;
    r.mode = ModeName(a.CPSR);
    r.thumb = a.CPSR & 0x20;
    QString f;
    f += (a.CPSR & (1u << 31)) ? 'N' : '-';
    f += (a.CPSR & (1u << 30)) ? 'Z' : '-';
    f += (a.CPSR & (1u << 29)) ? 'C' : '-';
    f += (a.CPSR & (1u << 28)) ? 'V' : '-';
    f += (a.CPSR & (1u << 27)) ? 'Q' : '-';
    f += (a.CPSR & 0x80) ? 'I' : '-';
    f += (a.CPSR & 0x40) ? 'F' : '-';
    f += (a.CPSR & 0x20) ? 'T' : '-';
    r.flags = f;
    switch (a.CPSR & 0x1F)
    {
    case 0x11: r.spsr = a.R_FIQ[7]; break;
    case 0x12: r.spsr = a.R_IRQ[2]; break;
    case 0x13: r.spsr = a.R_SVC[2]; break;
    case 0x17: r.spsr = a.R_ABT[2]; break;
    case 0x1B: r.spsr = a.R_UND[2]; break;
    default: r.spsr = 0; break;
    }
    return r;
}

WBApi::TcmLayout Tcm(Workbench& wb)
{
    WBApi::TcmLayout t;
    NDS* nds = wb.GetNDS();
    if (!nds) return t;
    t.itcm_size = nds->ARM9.ITCMSize;
    t.dtcm_base = nds->ARM9.DTCMBase == 0xFFFFFFFF ? 0 : nds->ARM9.DTCMBase;
    t.dtcm_size = nds->ARM9.DTCMMask ? (~nds->ARM9.DTCMMask + 1) : 0;
    return t;
}

WBApi::RecordingState Recording(Workbench& wb)
{
    WBApi::RecordingState r;
    r.enabled = wb.Rec.Enabled();
    r.write_history = wb.Rec.WriteHistoryOn();
    r.data_origin = wb.Taint.Enabled();
    r.available = !wb.JITActive();
    r.unavailable_reason = r.available ? "" : "Recording needs the interpreter: turn the JIT recompiler off in Emu settings.";
    r.session_frame_start = wb.Rec.SessionStartFrame();
    r.frames_recorded = wb.Rec.FramesRecorded();
    r.history_entries = wb.Rec.HistoryCount();
    r.history_skipped = wb.Rec.HistorySkipped();
    r.history_bulk_threshold = wb.Rec.GetHistoryFilter().BulkThreshold;
    r.history_skip_stack = wb.Rec.GetHistoryFilter().SkipStack;
    r.history_skip_unchanged = wb.Rec.GetHistoryFilter().SkipUnchanged;
    r.history_wrapped = wb.Rec.HistoryWrapped();
    r.history_oldest_frame = wb.Rec.HistoryOldestFrame();
    r.history_file_mb = wb.Rec.HistoryInFile() ? (u32)((wb.Rec.HistoryCap() * sizeof(WriteHistoryEntry)) >> 20) : 0;
    r.history_cap = wb.Rec.HistoryCap();
    r.pages_allocated = wb.Rec.Store().PageCount();
    r.origin_follow_addresses = wb.Taint.FollowAddresses();
    r.origin_seeded_files = wb.Taint.SeededFiles();
    u32 n = 0;
    for (const DataSource& s : wb.Taint.Sources())
        if (s.Enabled && s.Kind != SourceKind::CodeConstant) n++;
    r.source_count = n;
    return r;
}

bool MemoryRead(Workbench& wb, const WBApi::MemoryReadParams& p, WBApi::MemoryReadResult& out, QString& err)
{
    u32 cpu;
    if (!ParseCpu(p.cpu, cpu, err)) return false;
    if (p.size == 0 || p.size > 0x100000) { err = "size must be 1..1048576"; return false; }
    if (!wb.GetNDS()) { err = "no game running"; return false; }
    QByteArray bytes(p.size, 0);
    for (u32 i = 0; i < p.size; )
    {
        u32 a = p.addr + i;
        PhysAddr pa = wb.Translate(cpu, a);
        u8* ptr = wb.PhysPtr(pa);
        u32 chunk = std::min<u32>(p.size - i, PageSize - (a & (PageSize - 1)));
        if (ptr && pa.Region != (u16)Region::VRAM)
        {
            // within one page the mapping is linear for every region except VRAM banks
            PhysAddr pe = wb.Translate(cpu, a + chunk - 1);
            if (pe.Region == pa.Region && pe.Offset == pa.Offset + chunk - 1)
            {
                memcpy(bytes.data() + i, ptr, chunk);
                i += chunk;
                continue;
            }
        }
        u32 v = 0;
        wb.Peek(cpu, a, 1, v);
        bytes[i] = (char)v;
        i++;
    }
    out.addr = p.addr;
    out.size = p.size;
    out.region = QString::fromStdString(RegionLabel(wb.Translate(cpu, p.addr).Region));
    out.hex = QString::fromLatin1(bytes.toHex());
    QString d = p.decode.toLower();
    const u8* b = (const u8*)bytes.constData();
    auto rd = [&](u32 off, u32 n) { u32 v = 0; memcpy(&v, b + off, n); return v; };
    if (d == "hex") {}
    else if (d == "u8" || d == "s8") for (u32 i = 0; i < p.size; i++) out.values.push_back(d == "u8" ? (double)b[i] : (double)(s8)b[i]);
    else if (d == "u16" || d == "s16" || d == "fx16")
        for (u32 i = 0; i + 2 <= p.size; i += 2)
        {
            u32 v = rd(i, 2);
            if (d == "u16") out.values.push_back((double)v);
            else if (d == "s16") out.values.push_back((double)(s16)v);
            else out.values.push_back((double)(s16)v / 4096.0);
        }
    else if (d == "u32" || d == "s32" || d == "fx32")
        for (u32 i = 0; i + 4 <= p.size; i += 4)
        {
            u32 v = rd(i, 4);
            if (d == "u32") out.values.push_back((double)v);
            else if (d == "s32") out.values.push_back((double)(s32)v);
            else out.values.push_back((double)(s32)v / 4096.0);
        }
    else if (d == "ascii")
    {
        QString s;
        for (u32 i = 0; i < p.size; i++) s += (b[i] >= 0x20 && b[i] < 0x7F) ? QChar(b[i]) : QChar('.');
        out.values.push_back(s);
    }
    else if (d == "sjis")
    {
        QStringDecoder dec("Shift_JIS");
        if (!dec.isValid()) { err = "Shift-JIS decoding is not available in this Qt build"; return false; }
        QString s = dec.decode(bytes);
        out.values.push_back(s);
    }
    else { err = "unknown decode '" + p.decode + "'"; return false; }
    return true;
}

bool ByteAt(Workbench& wb, const MergedSessions* all, u32 cpu, u32 addr, ByteRecord& out)
{
    memset(&out, 0, sizeof(out));
    PhysAddr pa = wb.Translate(cpu, addr);
    if (!pa.Recordable()) return false;
    bool any = false;
    if (const ByteRecord* r = wb.Rec.Get(pa)) { out = *r; any = true; }
    if (all)
    {
        if (RecordPage* p = all->Records.Get(pa.Region, pa.Offset >> PageShift))
        {
            const ByteRecord& m = p->Rec[pa.Offset & (PageSize - 1)];
            out.exec_count += m.exec_count;
            out.read_count += m.read_count;
            out.write_count += m.write_count;
            out.dma_read_count += m.dma_read_count;
            out.dma_write_count += m.dma_write_count;
            out.flags |= m.flags;
            if (!any) { out.last_writer_pc = m.last_writer_pc; out.last_write_frame = m.last_write_frame; out.last_writer_cpu = m.last_writer_cpu; out.mem_class = m.mem_class; }
            else if (m.mem_class != out.mem_class && m.mem_class) out.mem_class = (out.exec_count && (out.read_count)) ? (u8)MemClass::CodeAndData : std::max(out.mem_class, m.mem_class);
            any = true;
        }
    }
    return any;
}

int RecordedMode(Workbench& wb, u32 cpu, u32 addr)
{
    const ByteRecord* r = wb.Rec.Get(wb.Translate(cpu, addr));
    if (!r || !r->exec_count) return -1;
    if ((r->flags & ByteFlag::Thumb) && !(r->flags & ByteFlag::Arm)) return 1;
    if (r->flags & ByteFlag::Arm) return 0;
    return (r->flags & ByteFlag::Thumb) ? 1 : 0;
}

QString DecodeConflict(Workbench& wb, u32 cpu, u32 start, u32 end, const QString& mode)
{
    u32 thumb = 0, arm = 0;
    end = std::min<u32>(end, start + 0x10000);
    for (u32 a = start; a < end; a += 2)
    {
        ByteRecord b;
        if (!ByteAt(wb, nullptr, cpu, a, b) || !b.exec_count) continue;
        if (b.flags & ByteFlag::Thumb) thumb = std::max(thumb, b.exec_count);
        if (b.flags & ByteFlag::Arm) arm = std::max(arm, b.exec_count);
    }
    if (mode == "data" && (thumb || arm)) return QString("marked data, but executed %1 times").arg(std::max(thumb, arm));
    if (mode == "arm" && thumb) return QString("marked ARM, but executed %1 times as Thumb").arg(thumb);
    if (mode == "thumb" && arm) return QString("marked Thumb, but executed %1 times as ARM").arg(arm);
    return QString();
}

u32 ExecCount(Workbench& wb, u32 cpu, u32 addr)
{
    const ByteRecord* r = wb.Rec.Get(wb.Translate(cpu, addr));
    return r ? r->exec_count : 0;
}

static const char* ClassName(u8 c)
{
    switch ((MemClass)c)
    {
    case MemClass::Code: return "code";
    case MemClass::Data: return "data";
    case MemClass::CodeAndData: return "code_and_data";
    case MemClass::IO: return "io";
    default: return "untouched";
    }
}

bool Metadata(Workbench& wb, const MergedSessions* all, const WBApi::MetadataParams& p, WBApi::MetadataResult& out, QString& err, const KbHints* kb, const AnalysisHints* analysis)
{
    KbHints none;
    u32 cpu;
    if (!ParseCpu(p.cpu, cpu, err)) return false;
    if (p.size == 0 || p.size > 0x1000000) { err = "size must be 1..16777216"; return false; }
    QString g = p.group.toLower();
    u32 bs = g == "byte" ? 1 : std::max<u32>(p.block_size, 1);
    if (g != "byte" && g != "block" && g != "instruction") { err = "group must be byte, instruction or block"; return false; }
    bool scopeAll = p.scope.toLower() == "all";
    const MergedSessions* m = scopeAll ? all : nullptr;

    u32 end = p.addr + p.size;
    u32 a = p.addr;
    u32 maxEntries = 65536;
    while (a < end && out.entries.size() < maxEntries)
    {
        u32 n = bs;
        ByteRecord first;
        ByteAt(wb, m, cpu, a, first);
        if (g == "instruction")
        {
            if ((first.flags & ByteFlag::InstrStart))
                n = ((first.flags & ByteFlag::Thumb) && !(first.flags & ByteFlag::Arm)) ? 2 : 4;
            else n = 4 - (a & 3);
        }
        if (a + n > end) n = end - a;

        WBApi::MetadataEntry e;
        e.addr = a;
        e.size = n;
        PhysAddr pa = wb.Translate(cpu, a);
        e.region = QString::fromStdString(RegionLabel(pa.Region));
        e.overlay = IsOverlayRegion(pa.Region) ? (s32)(pa.Region - (u16)Region::OverlayARM9Base) : -1;
        u16 flags = 0;
        u8 cls = 0;
        u32 lastFrame = 0;
        bool anyThumb = false, anyArm = false;
        for (u32 i = 0; i < n; i++)
        {
            ByteRecord r;
            if (i == 0) r = first; else ByteAt(wb, m, cpu, a + i, r);
            e.exec = std::max(e.exec, r.exec_count);
            e.read = std::max(e.read, r.read_count);
            e.write = std::max(e.write, r.write_count);
            e.dma_read = std::max(e.dma_read, r.dma_read_count);
            e.dma_write = std::max(e.dma_write, r.dma_write_count);
            flags |= r.flags;
            if (r.mem_class == (u8)MemClass::CodeAndData || (cls && r.mem_class && r.mem_class != cls)) cls = (u8)MemClass::CodeAndData;
            else if (!cls) cls = r.mem_class;
            if ((r.write_count || r.dma_write_count) && r.last_write_frame >= lastFrame)
            {
                lastFrame = r.last_write_frame;
                e.last_writer_pc = r.last_writer_pc;
                e.last_writer_cpu = CpuName(r.last_writer_cpu & 0xF) + ((r.last_writer_cpu >> 4) ? QString(" ch%1").arg(r.last_writer_cpu >> 4) : QString());
                e.last_write_frame = r.last_write_frame;
            }
            if (r.exec_count)
            {
                if (r.flags & ByteFlag::Thumb) anyThumb = true;
                if (r.flags & ByteFlag::Arm) anyArm = true;
            }
        }
        QStringList fl;
        FlagNames(flags, &fl);
        e.flags.assign(fl.begin(), fl.end());
        e.mem_class = ClassName(cls);
        e.mode = anyThumb && anyArm ? "mixed" : anyThumb ? "thumb" : anyArm ? "arm" : "";
        // first frame of each kind over the range's bytes (frame numbers fit this session only)
        if (!scopeAll)
        {
            u32 best[4] = {0, 0, 0, 0};       // 1 + frame, as stored
            for (u32 i = 0; i < n; i++)
            {
                PhysAddr bp = wb.Translate(cpu, a + i);
                if (!bp.Recordable()) continue;
                const RecordPage* pg = wb.Rec.Store().Get(bp.Region, bp.Offset >> PageShift);
                if (!pg) continue;
                const u32* f = pg->First[bp.Offset & (PageSize - 1)];
                for (int k = 0; k < 4; k++)
                    if (f[k] && (!best[k] || f[k] < best[k])) best[k] = f[k];
            }
            static const char* kinds[4] = {"exec", "read", "write", "dma"};
            u32 any = 0;
            for (int k = 0; k < 4; k++)
                if (best[k])
                {
                    e.first_kinds.push_back(kinds[k]);
                    if (!any || best[k] < any) any = best[k];
                }
            e.first_exec = best[0] ? best[0] - 1 : 0;
            e.first_read = best[1] ? best[1] - 1 : 0;
            e.first_write = best[2] ? best[2] - 1 : 0;
            e.first_dma = best[3] ? best[3] - 1 : 0;
            e.first_seen = any != 0;
            e.first_frame = any ? any - 1 : 0;
        }
        Classification c = Classify(&first, kb ? *kb : none, a, analysis);
        e.classification = KindName(c.Kind);
        e.confidence = EvidenceName(c.From);
        out.entries.push_back(e);
        a += n;
        if (n == 0) break;
    }
    out.truncated = a < end;
    out.next_addr = out.truncated ? a : 0;
    return true;
}

bool WritesIn(const HistoryView& h, const WBApi::WritesListParams& p, WBApi::WritesListResult& out, QString& err)
{
    QString c = p.cpu.toLower();
    int want = -1;   // any
    if (c == "arm9") want = 0;
    else if (c == "arm7") want = 1;
    else if (c == "dma") want = 2;
    else if (c != "any" && !c.isEmpty()) { err = "cpu must be any, arm9, arm7 or dma"; return false; }
    bool everywhere = p.size == 0;
    u32 end = p.addr + std::max<u32>(p.size, 1);
    auto match = [&](const WriteHistoryEntry& e)
    {
        if (e.frame < p.frame_from || e.frame > p.frame_to) return false;
        if (!everywhere && (e.addr >= end || e.addr + e.size <= p.addr)) return false;
        if (e.pc < p.writer_from || e.pc >= p.writer_to) return false;
        if (want == 0 && e.cpu != 0) return false;
        if (want == 1 && e.cpu != 1) return false;
        if (want == 2 && e.cpu < 2) return false;
        return true;
    };
    // two passes keep memory flat however large history is: count, then take the page
    u64 total = 0;
    h.ForEach([&](const WriteHistoryEntry& e) { if (match(e)) total++; });
    u64 limit = std::min<u64>(std::max<u32>(p.limit, 1), 100000);
    u64 last = total > p.skip ? total - p.skip : 0;          // page = matches [first, last), oldest first
    u64 first = last > limit ? last - limit : 0;
    out.total = total;
    out.truncated = first > 0;
    std::vector<WriteHistoryEntry> hits;
    hits.reserve((size_t)(last - first));
    u64 k = 0;
    h.ForEach([&](const WriteHistoryEntry& e)
    {
        if (!match(e)) return;
        if (k >= first && k < last) hits.push_back(e);
        k++;
    });
    for (size_t i = 0; i < hits.size(); i++)
    {
        const WriteHistoryEntry& e = hits[i];
        WBApi::WriteEntry w;
        w.frame = e.frame;
        w.timestamp = e.timestamp;
        w.cpu = CpuName(e.cpu);
        w.pc = e.pc;
        w.addr = e.addr;
        w.size = e.size;
        w.old_value = e.old_value;
        w.new_value = e.new_value;
        out.entries.push_back(w);
    }
    out.history_wrapped = h.Wrapped();
    out.oldest_frame = h.OldestFrame();
    return true;
}

namespace
{
using TaintTable = melonDS::WB::Origin;     // (WBQ::Origin is the origin.get query)

// A label's merge, with running totals collapsed: when one of the joined labels was itself made by the
// same instruction in the same way, that instruction was adding to its own result (a loader's position,
// an allocator's total), so the chain becomes one step list instead of a tree as deep as its sources
struct HowNode
{
    u32 Label = 0;
    const TaintTable::LabelMerge* M = nullptr;      // null: one source
    u32 Steps = 1, FrameFirst = 0;
    std::vector<u32> Parts;                      // oldest first
};

HowNode CollapseHow(const TaintTable& o, u32 label)
{
    HowNode n;
    n.Label = label;
    const TaintTable::LabelMerge& m = o.MergeOf(label);
    if (!m.A && !m.B) return n;
    n.M = &m;
    n.FrameFirst = m.Frame;
    std::vector<u32> added;
    u32 cur = label;
    for (u32 guard = 0; guard < (1u << 20); guard++)
    {
        const TaintTable::LabelMerge& c = o.MergeOf(cur);
        auto same = [&](u32 l)
        {
            const TaintTable::LabelMerge& x = o.MergeOf(l);
            return (x.A || x.B) && x.PC == m.PC && x.Cpu == m.Cpu && x.Op == m.Op;
        };
        u32 acc = same(c.A) ? c.A : same(c.B) ? c.B : 0;
        if (same(c.A) && same(c.B))
            acc = o.LabelSources(c.A).size() >= o.LabelSources(c.B).size() ? c.A : c.B;
        if (!acc)
        {
            if (c.B) added.push_back(c.B);
            if (c.A) added.push_back(c.A);
            break;
        }
        added.push_back(acc == c.A ? c.B : c.A);
        n.Steps++;
        n.FrameFirst = std::min(n.FrameFirst, o.MergeOf(acc).Frame);
        cur = acc;
    }
    for (auto it = added.rbegin(); it != added.rend(); ++it)
        if (*it && std::find(n.Parts.begin(), n.Parts.end(), *it) == n.Parts.end()) n.Parts.push_back(*it);
    return n;
}
}

bool LabelTree(Workbench& wb, const WBApi::OriginLabelParams& p, WBApi::OriginLabelResult& out, QString& err)
{
    const TaintTable& o = wb.Taint;
    if (!p.label || p.label >= o.LabelCount()) { err = QString("no label %1 (labels are 1..%2)").arg(p.label).arg(o.LabelCount() - 1); return false; }
    u32 maxNodes = std::clamp<u32>(p.max_nodes, 1, 4096);
    std::map<u32, u32> index;                    // label -> node
    std::vector<u32> queue {p.label};
    index[p.label] = 0;
    out.nodes.resize(1);
    for (size_t qi = 0; qi < queue.size(); qi++)
    {
        HowNode h = CollapseHow(o, queue[qi]);
        WBApi::LabelNode& n = out.nodes[qi];
        n.label = h.Label;
        const std::vector<u32>& srcs = o.LabelSources(h.Label);
        n.source_count = (u32)srcs.size();
        for (size_t i = 0; i < srcs.size() && i < 8; i++)
            if (const DataSource* s = o.FindSource(srcs[i])) n.sources.push_back(QString::fromStdString(s->Name));
        if (!h.M) { n.how = "source"; continue; }
        n.how = TaintTable::MergeOpName(h.M->Op);
        n.pc = h.M->PC;
        n.cpu = CpuName(h.M->Cpu);
        n.frame = h.M->Frame;
        n.frame_first = h.FrameFirst;
        n.steps = h.Steps;
        for (u32 part : h.Parts)
        {
            auto it = index.find(part);
            if (it == index.end())
            {
                if (queue.size() >= maxNodes) { n.parts_more++; out.truncated = true; continue; }
                it = index.emplace(part, (u32)queue.size()).first;
                queue.push_back(part);
                out.nodes.emplace_back();
            }
            out.nodes[qi].parts.push_back(it->second);
        }
    }
    return true;
}

bool Writes(Workbench& wb, const WBApi::WritesListParams& p, WBApi::WritesListResult& out, QString& err)
{
    if (!WritesIn(wb.Rec.LiveHistory(), p, out, err)) return false;
    out.history_enabled = wb.Rec.WriteHistoryOn();
    out.history_source = "live";
    ByteRecord r;
    if (ByteAt(wb, nullptr, 0, p.addr, r) && (r.write_count || r.dma_write_count))
    {
        out.last_writer_pc = r.last_writer_pc;
        out.last_writer_cpu = CpuName(r.last_writer_cpu & 0xF);
        out.last_write_frame = r.last_write_frame;
    }
    return true;
}

namespace
{
struct RegionSel { u16 Region; };
}

bool Coverage(Workbench& wb, const MergedSessions* all, const WBApi::CoverageParams& p, const std::vector<FuncRange>& known,
              WBApi::CoverageResult& out, QString& err)
{
    QString kind = p.kind.toLower();
    int k;
    if (kind == "exec") k = 0;
    else if (kind == "read") k = 1;
    else if (kind == "write") k = 2;
    else if (kind == "dma") k = 3;
    else { err = "kind must be exec, read, write or dma"; return false; }
    bool scopeAll = p.scope.toLower() == "all";
    const MergedSessions* m = scopeAll ? all : nullptr;

    auto counter = [k](const ByteRecord& r) -> u64
    {
        switch (k)
        {
        case 0: return r.exec_count;
        case 1: return r.read_count;
        case 2: return r.write_count;
        default: return (u64)r.dma_read_count + r.dma_write_count;
        }
    };

    // collect (region, page) pairs to scan
    std::set<std::pair<u16, u32>> pages;
    auto addPages = [&](const RecordStore& st)
    {
        st.ForEach([&](u16 region, u32 page, const RecordPage*) { pages.insert({region, page}); });
    };
    addPages(wb.Rec.Store());
    if (m) addPages(m->Records);

    QString rsel = p.region.toLower();
    auto regionWanted = [&](u16 r) -> bool
    {
        if (rsel == "all" || rsel.isEmpty()) return true;
        if (rsel == "main_ram") return r == (u16)Region::MainRAM || IsOverlayRegion(r);
        if (rsel == "itcm") return r == (u16)Region::ITCM;
        if (rsel == "dtcm") return r == (u16)Region::DTCM;
        if (rsel == "shared_wram") return r == (u16)Region::SharedWRAM;
        if (rsel == "arm7_wram") return r == (u16)Region::ARM7WRAM;
        if (rsel == "vram") return r == (u16)Region::VRAM;
        return QString::fromStdString(RegionLabel(r)) == rsel;
    };
    u32 lo = p.addr, hi = p.size ? p.addr + p.size : 0xFFFFFFFF;
    bool writerFilter = k == 2 && (p.writer_from != 0 || p.writer_to != 0xFFFFFFFF);

    u32 limit = std::max<u32>(p.limit, 1);
    WBApi::CoverageRange cur;
    bool open = false;
    bool curThumb = false, curArm = false;
    u16 curRegion = 0;
    auto flush = [&]()
    {
        if (!open) return;
        open = false;
        if (p.unknown_only && k == 0)
        {
            // split the range by known functions and keep only the uncovered parts
            std::vector<std::pair<u32, u32>> parts {{cur.start, cur.end}};
            for (const FuncRange& f : known)
            {
                if (f.End <= cur.start || f.Start >= cur.end) continue;
                std::vector<std::pair<u32, u32>> next;
                for (auto& pr : parts)
                {
                    if (f.End <= pr.first || f.Start >= pr.second) { next.push_back(pr); continue; }
                    if (pr.first < f.Start) next.push_back({pr.first, f.Start});
                    if (f.End < pr.second) next.push_back({f.End, pr.second});
                }
                parts.swap(next);
            }
            for (auto& pr : parts)
            {
                if (out.ranges.size() >= limit) { out.truncated = true; out.next_addr = pr.first; return; }
                WBApi::CoverageRange r = cur;
                r.start = pr.first;
                r.end = pr.second;
                out.ranges.push_back(r);
                out.total_bytes += pr.second - pr.first;
            }
            return;
        }
        if (out.ranges.size() >= limit) { out.truncated = true; out.next_addr = cur.start; return; }
        out.ranges.push_back(cur);
        out.total_bytes += cur.end - cur.start;
    };

    for (const auto& pg : pages)
    {
        u16 region = pg.first;
        if (!regionWanted(region)) continue;
        RecordPage* a = wb.Rec.Store().Get(region, pg.second);
        RecordPage* b = m ? m->Records.Get(region, pg.second) : nullptr;
        u32 base = CanonicalAddr(wb, region, pg.second << PageShift);
        if (base + PageSize <= lo || base >= hi) continue;
        for (u32 i = 0; i < PageSize; i++)
        {
            u32 addr = base + i;
            if (addr < lo || addr >= hi) { if (open) flush(); continue; }
            u64 c = 0;
            u16 fl = 0;
            if (a) { c += counter(a->Rec[i]); fl |= a->Rec[i].flags; }
            if (b) { c += counter(b->Rec[i]); fl |= b->Rec[i].flags; }
            if (c && writerFilter)
            {
                // the current session's last writer (merged sessions keep their own, older ones)
                const ByteRecord* r = a ? &a->Rec[i] : &b->Rec[i];
                if ((r->last_writer_cpu & 0xF) >= 2 || r->last_writer_pc < p.writer_from || r->last_writer_pc >= p.writer_to) c = 0;
            }
            if (!c)
            {
                if (open) flush();
                continue;
            }
            if (open && (cur.end != addr || curRegion != region)) flush();
            if (!open)
            {
                open = true;
                cur = WBApi::CoverageRange();
                cur.start = addr;
                cur.end = addr;
                cur.count = 0;
                curThumb = curArm = false;
                curRegion = region;
                cur.overlay = IsOverlayRegion(region) ? (s32)(region - (u16)Region::OverlayARM9Base) : -1;
            }
            cur.end = addr + 1;
            cur.count += c;
            if (k == 0)
            {
                if (fl & ByteFlag::Thumb) curThumb = true;
                if (fl & ByteFlag::Arm) curArm = true;
                cur.mode = curThumb && curArm ? "mixed" : curThumb ? "thumb" : "arm";
            }
            if (out.truncated) return true;
        }
        if (out.truncated) return true;
    }
    if (open) flush();
    return true;
}

static void AddEdges(const std::unordered_map<CallEdgeKey, CallEdge, CallEdgeKeyHash>& edges, u32 entry, u32 size,
                     std::map<std::pair<u32, u32>, CallEdge>& callers, std::map<std::pair<u32, u32>, CallEdge>& callees)
{
    for (const auto& kv : edges)
    {
        const CallEdge& e = kv.second;
        if ((e.callee & ~1u) == entry)
        {
            auto& c = callers[{e.call_site, e.callee}];
            if (!c.count) c = e; else { c.count += e.count; c.last_frame = std::max(c.last_frame, e.last_frame); }
        }
        bool inside = size ? (e.call_site >= entry && e.call_site < entry + size) : (e.caller_func & ~1u) == entry;
        if (inside)
        {
            auto& c = callees[{e.call_site, e.callee}];
            if (!c.count) c = e; else { c.count += e.count; c.last_frame = std::max(c.last_frame, e.last_frame); }
        }
    }
}

static WBApi::CallEdgeInfo EdgeInfo(const CallEdge& e)
{
    WBApi::CallEdgeInfo i;
    i.call_site = e.call_site;
    i.callee = e.callee;
    i.caller_func = e.caller_func;
    i.count = e.count;
    i.kind = CallKindName(e.kind);
    i.first_frame = e.first_frame;
    i.last_frame = e.last_frame;
    return i;
}

bool Calls(Workbench& wb, const MergedSessions* all, u32 cpu, u32 entry, u32 size, u32 limit, WBApi::CallsResult& out)
{
    std::map<std::pair<u32, u32>, CallEdge> callers, callees;
    AddEdges(wb.Rec.Edges(cpu), entry, size, callers, callees);
    if (all) AddEdges(all->Edges[cpu], entry, size, callers, callees);
    out.function = entry;
    std::vector<CallEdge> a, b;
    for (auto& kv : callers) a.push_back(kv.second);
    for (auto& kv : callees) b.push_back(kv.second);
    auto bycount = [](const CallEdge& x, const CallEdge& y) { return x.count > y.count; };
    std::sort(a.begin(), a.end(), bycount);
    std::sort(b.begin(), b.end(), bycount);
    for (size_t i = 0; i < a.size() && i < limit; i++) out.callers.push_back(EdgeInfo(a[i]));
    for (size_t i = 0; i < b.size() && i < limit; i++) out.callees.push_back(EdgeInfo(b[i]));
    return true;
}

u32 InferFunctionEntry(Workbench& wb, const MergedSessions* all, u32 cpu, u32 addr)
{
    u32 best = 0xFFFFFFFF;
    auto scan = [&](const std::unordered_map<CallEdgeKey, CallEdge, CallEdgeKeyHash>& edges)
    {
        for (const auto& kv : edges)
        {
            u32 c = kv.second.callee & ~1u;
            if (c <= addr && addr - c < 0x4000 && (best == 0xFFFFFFFF || c > best)) best = c;
        }
    };
    scan(wb.Rec.Edges(cpu));
    if (all) scan(all->Edges[cpu & 1]);
    // the function that was on the call stack when this instruction first ran
    u32 first = wb.Rec.FirstRunIn(cpu, addr);
    if (!first && all)
    {
        auto it = all->FirstFunc.find(((u64)(cpu & 1) << 32) | addr);
        if (it != all->FirstFunc.end()) first = it->second;
    }
    if (first > addr || addr - first >= 0x10000) first = 0;
    if (best != 0xFFFFFFFF)
    {
        // every instruction between the entry and addr must have executed (no unrelated code in between)
        for (u32 a = best; a < addr; a += 2)
        {
            const ByteRecord* r = wb.Rec.Get(wb.Translate(cpu, a));
            if (!r || !r->exec_count)
            {
                // literal pools sit between functions' code; allow read-only words
                if (r && (r->flags & ByteFlag::LiteralPool)) continue;
                best = 0xFFFFFFFF;
                break;
            }
        }
    }
    // the nearer of the two: a callee entered through a tail call (b) runs with its caller on the stack
    if (best != 0xFFFFFFFF && (!first || best >= first)) return best;
    return first ? first : addr;
}

WBApi::CallStackResult CallStack(Workbench& wb, u32 cpu)
{
    WBApi::CallStackResult r;
    r.cpu = CpuName(cpu);
    for (const ShadowFrame& f : wb.Rec.ShadowStack(cpu))
    {
        WBApi::StackFrameInfo i;
        i.callee = f.callee;
        i.call_site = f.call_site;
        i.return_addr = f.return_addr;
        i.sp = f.sp;
        i.kind = CallKindName(f.kind);
        r.frames.push_back(i);
    }
    return r;
}

static QString ProvKind(u8 k) { return ProvenanceKindName(k); }

// ---------------------------------------------------------------- game archives

static WBApi::ArchiveMemberInfo MemberInfo(const WBArc::ArchiveIndex& arc, const WBArc::Member& m)
{
    WBApi::ArchiveMemberInfo i;
    i.index = m.Index;
    i.path = QString::fromStdString(m.Path);
    i.vfs_entry = m.VfsEntry;
    i.size = m.Size;
    i.packed_size = m.PackedSize;
    i.method = m.Method == 0 ? QString("stored") : m.Method == 8 ? QString("deflate") : QString("method %1").arg(m.Method);
    i.rom_offset = arc.ArchiveRomStart + m.DataOffset;
    i.header_rom_offset = arc.ArchiveRomStart + m.HeaderOffset;
    i.crc = m.Crc;
    i.readable = m.Ok;
    if (m.DuplicateOf >= 0) i.duplicate_of = QString::fromStdString(arc.Members()[m.DuplicateOf].Path);
    return i;
}

static QString ArchiveMemberAt(const ArchiveCtx& arc, u32 fileid, u32 romoff)
{
    if (!arc.Index || fileid != arc.Index->ArchiveFileId || romoff < arc.Index->ArchiveRomStart) return QString();
    return QString::fromStdString(arc.Index->Describe(romoff - arc.Index->ArchiveRomStart));
}

// card reads that fetched any byte of the member (header or data), newest first
static std::vector<WBApi::ProvenanceInfo> MemberLoads(Workbench& wb, const ArchiveCtx& arc, const WBArc::Member& m, size_t max)
{
    std::vector<WBApi::ProvenanceInfo> out;
    const WBArc::ArchiveIndex& a = *arc.Index;
    u32 lo = a.ArchiveRomStart + m.HeaderOffset, hi = a.ArchiveRomStart + m.DataOffset + m.PackedSize;
    const auto& prov = wb.Rec.Provenance();
    for (size_t i = prov.size(); i-- > 0 && out.size() < max; )
    {
        const ProvenanceEvent& ev = prov[i];
        if (ev.kind != (u8)ProvenanceKind::CardRead || ev.file_id != a.ArchiveFileId) continue;
        u32 rs = ev.rom_offset, re = ev.rom_offset + std::max<u32>(ev.src_size, 1);
        if (re <= lo || rs >= hi) continue;
        WBApi::ProvenanceInfo info;
        info.kind = "card_read";
        info.frame = ev.frame;
        info.pc = ev.pc;
        info.src_addr = ev.src_addr;
        info.src_size = ev.src_size;
        info.dst_addr = ev.dst_addr;
        info.dst_size = ev.dst_size;
        info.rom_offset = ev.rom_offset;
        info.file_id = (melonDS::s32)ev.file_id;
        info.file_name = QString::fromStdString(a.ArchivePath);
        info.detail = QString("file offset 0x%1").arg(ev.rom_offset - a.ArchiveRomStart, 0, 16);
        info.archive_member = QString::fromStdString(a.Describe(std::max(rs, lo) - a.ArchiveRomStart));
        out.push_back(info);
    }
    return out;
}

static bool MainRam(Workbench& wb, const u8*& mem, u32& len)
{
    melonDS::NDS* nds = wb.GetNDS();
    if (!nds || !nds->MainRAM) return false;
    mem = nds->MainRAM;
    len = nds->MainRAMMask + 1;
    return true;
}

static WBApi::ArchiveSegment Segment(Workbench& wb, const ArchiveCtx& arc, const WBArc::Match& mt, u32 focus)
{
    const WBArc::ArchiveIndex& a = *arc.Index;
    // of several files with these bytes, prefer the one the game read from the card most recently
    WBArc::Match x = mt;
    if (!x.Alternatives.empty())
    {
        int best = x.Member;
        u32 bestFrame = 0;
        bool any = false;
        std::vector<int> all = x.Alternatives;
        all.insert(all.begin(), x.Member);
        for (int i : all)
        {
            auto l = MemberLoads(wb, arc, a.Members()[i], 1);
            if (!l.empty() && (!any || l[0].frame > bestFrame)) { best = i; bestFrame = l[0].frame; any = true; }
        }
        if (best != x.Member)
        {
            // same bytes at the same place: only the member changes
            x.Alternatives.erase(std::find(x.Alternatives.begin(), x.Alternatives.end(), best));
            x.Alternatives.insert(x.Alternatives.begin(), x.Member);
            x.Member = best;
        }
    }
    const WBArc::Member& m = a.Members()[x.Member];
    WBApi::ArchiveSegment s;
    s.addr = x.Addr;
    s.size = x.Size;
    s.path = QString::fromStdString(m.Path);
    s.member_index = m.Index;
    s.member_offset = x.MemberOffset;
    s.member_size = m.Size;
    for (int i : x.Alternatives)
        if (s.alternatives.size() < 16) s.alternatives.push_back(QString::fromStdString(a.Members()[i].Path));
    ByteRecord r;
    u32 at = (focus >= x.Addr && focus < x.Addr + x.Size) ? focus : x.Addr;
    if (ByteAt(wb, nullptr, 0, at, r) && (r.write_count || r.dma_write_count)) s.writer_pc = r.last_writer_pc;
    s.loads = MemberLoads(wb, arc, m, 4);
    return s;
}

std::vector<WBApi::ArchiveSegment> ArchiveSegments(Workbench& wb, const ArchiveCtx& arc, u32 addr, u32 size, size_t max)
{
    std::vector<WBApi::ArchiveSegment> out;
    const u8* mem;
    u32 len;
    if (!arc.Index || (addr >> 24) != 0x02 || !MainRam(wb, mem, len)) return out;
    u32 a = 0x02000000 + (addr & (len - 1));
    u32 n = std::min<u32>(std::max<u32>(size, 1), 0x02000000 + len - a);
    for (const WBArc::Match& mt : arc.Index->Resolve(mem, 0x02000000, len, a, n, max))
        out.push_back(Segment(wb, arc, mt, a));
    return out;
}

bool ArchiveList(const ArchiveCtx& arc, const WBApi::ArchiveListParams& p, WBApi::ArchiveListResult& out, QString& err)
{
    out.status = arc.Status;
    if (!arc.Index) return true;
    const WBArc::ArchiveIndex& a = *arc.Index;
    out.kind = QString::fromStdString(a.Kind);
    out.archive_file = QString::fromStdString(a.ArchivePath);
    out.index_file = QString::fromStdString(a.IndexPath);
    out.summary = QString::fromStdString(a.Summary);
    out.member_count = (u32)a.Members().size();
    QString f = p.filter.toLower();
    u32 limit = std::min<u32>(p.limit ? p.limit : 200, 5000);
    for (const WBArc::Member& m : a.Members())
    {
        if (!f.isEmpty() && !QString::fromStdString(m.Path).toLower().contains(f)) continue;
        if (out.matched++ < p.offset) continue;
        if (out.members.size() < limit) out.members.push_back(MemberInfo(a, m));
    }
    return true;
}

bool ArchiveLocate(Workbench& wb, const ArchiveCtx& arc, const WBApi::ArchiveLocateParams& p, WBApi::ArchiveLocateResult& out, QString& err)
{
    out.status = arc.Status;
    if (!arc.Index) { err = "no archive available (" + arc.Status + ")"; return false; }
    const WBArc::ArchiveIndex& a = *arc.Index;
    int i = p.path.isEmpty() ? p.index : a.FindPath(p.path.toStdString());
    if (i < 0 || (size_t)i >= a.Members().size()) { err = "no such archived file: " + (p.path.isEmpty() ? QString::number(p.index) : p.path); return false; }
    const WBArc::Member& m = a.Members()[i];
    out.member = MemberInfo(a, m);
    const u8* mem;
    u32 len;
    if (MainRam(wb, mem, len))
        for (const WBArc::Match& mt : a.Locate(i, mem, 0x02000000, len))
        {
            WBApi::ArchiveSegment s = Segment(wb, arc, mt, mt.Addr);
            s.loads.clear();
            s.path = QString::fromStdString(m.Path);       // Segment may prefer an identical file that was loaded
            s.member_index = m.Index;
            s.member_size = m.Size;
            s.alternatives.erase(std::remove(s.alternatives.begin(), s.alternatives.end(), s.path), s.alternatives.end());
            // similar files (car colours, track pieces) share long runs: name the file that explains this copy best
            s.best_match = s.path;
            auto best = a.Resolve(mem, 0x02000000, len, mt.Addr + mt.Size / 2, 1, 1);
            bool shared = !best.empty() && (best[0].Member == i ||
                std::find(best[0].Alternatives.begin(), best[0].Alternatives.end(), i) != best[0].Alternatives.end());
            if (!best.empty() && !shared && best[0].Size > mt.Size)
                s.best_match = QString::fromStdString(a.Members()[best[0].Member].Path);
            // other files with exactly these bytes here (car colours share most of their models)
            s.alternatives.clear();
            if (shared)
            {
                std::vector<int> all = best[0].Alternatives;
                all.insert(all.begin(), best[0].Member);
                for (int k : all)
                    if (k != i && s.alternatives.size() < 16) s.alternatives.push_back(QString::fromStdString(a.Members()[k].Path));
            }
            out.ram.push_back(s);
        }
    out.loads = MemberLoads(wb, arc, m, 16);
    return true;
}

// ---------------------------------------------------------------- NARC and SDAT containers

static WBApi::ContainerInfo ContainerInfoOf(const WBArc::Container& c)
{
    WBApi::ContainerInfo i;
    i.path = QString::fromStdString(c.Path);
    i.file_id = c.FileId;
    i.format = QString::fromStdString(c.Format);
    i.packed = QString::fromStdString(c.Packed);
    i.rom_offset = c.RomStart;
    i.rom_size = c.RomSize;
    i.size = c.Size;
    i.member_count = (u32)c.Members.size();
    i.nested_count = (u32)c.Nested;
    return i;
}

static QString GlobalPath(const WBArc::ContainerIndex& x, int global)
{
    auto [ci, mi] = x.FromGlobal(global);
    return QString::fromStdString(x.FullName(ci, mi));
}

static WBApi::ContainerMemberInfo ContainerMemberOf(const WBArc::ContainerIndex& x, const WBArc::Container& c, int mi)
{
    const WBArc::ContainerMember& m = c.Members[mi];
    WBApi::ContainerMemberInfo i;
    i.index = (u32)mi;
    i.name = QString::fromStdString(m.Name);
    i.path = QString::fromStdString(c.Path + ":" + m.Name);
    i.kind = QString::fromStdString(m.Kind);
    i.form = QString::fromStdString(m.Form);
    i.size = m.Size;
    i.stored_size = m.StoredSize;
    i.rom_offset = m.Offset == WBArc::ContainerMember::NoOffset ? 0xFFFFFFFF : c.RomStart + m.Offset;
    i.depth = (u32)m.Depth;
    if (m.Parent >= 0) i.nested_in = QString::fromStdString(c.Members[m.Parent].Name);
    i.readable = m.Ok && m.Size <= x.MaxIndexed;
    if (m.DuplicateOf >= 0) i.duplicate_of = GlobalPath(x, m.DuplicateOf);
    return i;
}

static QString ContainerMemberAt(const ContainerCtx& cont, u32 fileid, u32 romoff)
{
    const WBArc::Container* c = cont.Index ? cont.Index->ByFile(fileid) : nullptr;
    if (!c || romoff < c->RomStart) return QString();
    return QString::fromStdString(cont.Index->Describe(fileid, romoff - c->RomStart));
}

// card reads that fetched any of the member's bytes, newest first. Inside a container packed whole, the
// member has no bytes of its own in the ROM: every read of the container counts
static std::vector<WBApi::ProvenanceInfo> ContainerLoads(Workbench& wb, const ContainerCtx& cont, const WBArc::Container& c,
                                                         const WBArc::ContainerMember& m, size_t max)
{
    std::vector<WBApi::ProvenanceInfo> out;
    bool whole = m.Offset == WBArc::ContainerMember::NoOffset;
    u32 lo = whole ? c.RomStart : c.RomStart + m.Offset, hi = whole ? c.RomStart + c.RomSize : lo + m.StoredSize;
    const auto& prov = wb.Rec.Provenance();
    for (size_t i = prov.size(); i-- > 0 && out.size() < max; )
    {
        const ProvenanceEvent& ev = prov[i];
        if (ev.kind != (u8)ProvenanceKind::CardRead || ev.file_id != c.FileId) continue;
        u32 rs = ev.rom_offset, re = ev.rom_offset + std::max<u32>(ev.src_size, 1);
        if (re <= lo || rs >= hi) continue;
        WBApi::ProvenanceInfo info;
        info.kind = "card_read";
        info.frame = ev.frame;
        info.pc = ev.pc;
        info.src_addr = ev.src_addr;
        info.src_size = ev.src_size;
        info.dst_addr = ev.dst_addr;
        info.dst_size = ev.dst_size;
        info.rom_offset = ev.rom_offset;
        info.file_id = (melonDS::s32)ev.file_id;
        info.file_name = QString::fromStdString(c.Path);
        info.detail = QString("file offset 0x%1").arg(ev.rom_offset - c.RomStart, 0, 16);
        info.archive_member = whole ? QString() : ContainerMemberAt(cont, c.FileId, std::max(rs, lo));
        out.push_back(info);
    }
    return out;
}

static WBApi::ArchiveSegment ContainerSegment(Workbench& wb, const ContainerCtx& cont, const WBArc::Match& mt, u32 focus)
{
    const WBArc::ContainerIndex& x = *cont.Index;
    // of several files with these bytes, prefer the one the game read from the card most recently
    WBArc::Match y = mt;
    if (!y.Alternatives.empty())
    {
        int best = y.Member;
        u32 bestFrame = 0;
        bool any = false;
        std::vector<int> all = y.Alternatives;
        all.insert(all.begin(), y.Member);
        for (int g : all)
        {
            auto [ci, mi] = x.FromGlobal(g);
            if (ci < 0) continue;
            auto l = ContainerLoads(wb, cont, x.Containers()[ci], x.Containers()[ci].Members[mi], 1);
            if (!l.empty() && (!any || l[0].frame > bestFrame)) { best = g; bestFrame = l[0].frame; any = true; }
        }
        if (best != y.Member)
        {
            y.Alternatives.erase(std::find(y.Alternatives.begin(), y.Alternatives.end(), best));
            y.Alternatives.insert(y.Alternatives.begin(), y.Member);
            y.Member = best;
        }
    }
    auto [ci, mi] = x.FromGlobal(y.Member);
    const WBArc::Container& c = x.Containers()[ci];
    const WBArc::ContainerMember& m = c.Members[mi];
    WBApi::ArchiveSegment s;
    s.addr = y.Addr;
    s.size = y.Size;
    s.path = QString::fromStdString(x.FullName(ci, mi));
    s.member_index = (u32)mi;
    s.member_offset = y.MemberOffset;
    s.member_size = m.Size;
    for (int g : y.Alternatives)
        if (s.alternatives.size() < 16) s.alternatives.push_back(GlobalPath(x, g));
    ByteRecord r;
    u32 at = (focus >= y.Addr && focus < y.Addr + y.Size) ? focus : y.Addr;
    if (ByteAt(wb, nullptr, 0, at, r) && (r.write_count || r.dma_write_count)) s.writer_pc = r.last_writer_pc;
    s.loads = ContainerLoads(wb, cont, c, m, 4);
    return s;
}

std::vector<WBApi::ArchiveSegment> ContainerSegments(Workbench& wb, const ContainerCtx& cont, u32 addr, u32 size, size_t max)
{
    std::vector<WBApi::ArchiveSegment> out;
    const u8* mem;
    u32 len;
    if (!cont.Index || (addr >> 24) != 0x02 || !MainRam(wb, mem, len)) return out;
    u32 a = 0x02000000 + (addr & (len - 1));
    u32 n = std::min<u32>(std::max<u32>(size, 1), 0x02000000 + len - a);
    for (const WBArc::Match& mt : cont.Index->Resolve(mem, 0x02000000, len, a, n, max))
        if (cont.Index->FromGlobal(mt.Member).first >= 0) out.push_back(ContainerSegment(wb, cont, mt, a));
    return out;
}

bool ContainerList(const ContainerCtx& cont, const WBApi::ContainerListParams& p, WBApi::ContainerListResult& out, QString& err)
{
    out.status = cont.Status;
    if (!cont.Index) return true;
    const WBArc::ContainerIndex& x = *cont.Index;
    out.summary = QString::fromStdString(x.Summary);
    QString only = p.container.toLower();
    while (only.startsWith('/')) only.remove(0, 1);
    QString f = p.filter.toLower();
    u32 limit = std::min<u32>(p.limit ? p.limit : 200, 5000);
    for (const WBArc::Container& c : x.Containers())
    {
        QString path = QString::fromStdString(c.Path);
        if (!only.isEmpty() && path.toLower().mid(path.startsWith('/') ? 1 : 0) != only) continue;
        out.containers.push_back(ContainerInfoOf(c));
        out.member_count += (u32)c.Members.size();
        for (size_t mi = 0; mi < c.Members.size(); mi++)
        {
            if (!f.isEmpty() && !(path + ":" + QString::fromStdString(c.Members[mi].Name)).toLower().contains(f)) continue;
            if (out.matched++ < p.offset) continue;
            if (out.members.size() < limit) out.members.push_back(ContainerMemberOf(x, c, (int)mi));
        }
    }
    if (!only.isEmpty() && out.containers.empty()) { err = "no container " + p.container; return false; }
    return true;
}

bool ContainerLocate(Workbench& wb, const ContainerCtx& cont, const WBApi::ContainerLocateParams& p, WBApi::ContainerLocateResult& out, QString& err)
{
    out.status = cont.Status;
    if (!cont.Index) { err = "no NARC or SDAT files (" + cont.Status + ")"; return false; }
    const WBArc::ContainerIndex& x = *cont.Index;
    int ci = -1, mi = -1;
    if (!x.Find(p.path.toStdString(), ci, mi)) { err = "no such file in a container: " + p.path + " (give it as /file.narc:name)"; return false; }
    const WBArc::Container& c = x.Containers()[ci];
    const WBArc::ContainerMember& m = c.Members[mi];
    out.container = ContainerInfoOf(c);
    out.member = ContainerMemberOf(x, c, mi);
    QString self = out.member.path;
    const u8* mem;
    u32 len;
    if (MainRam(wb, mem, len))
        for (const WBArc::Match& mt : x.Locate(ci, mi, mem, 0x02000000, len))
        {
            WBApi::ArchiveSegment s = ContainerSegment(wb, cont, mt, mt.Addr);
            s.loads.clear();
            s.path = self;                     // ContainerSegment may prefer an identical file that was loaded
            s.member_index = (u32)mi;
            s.member_size = m.Size;
            // similar files share long runs: name the file that explains this copy best
            s.best_match = self;
            auto best = x.Resolve(mem, 0x02000000, len, mt.Addr + mt.Size / 2, 1, 1);
            bool shared = !best.empty() && (best[0].Member == (int)m.Global ||
                std::find(best[0].Alternatives.begin(), best[0].Alternatives.end(), (int)m.Global) != best[0].Alternatives.end());
            if (!best.empty() && !shared && best[0].Size > mt.Size) s.best_match = GlobalPath(x, best[0].Member);
            // other files with exactly these bytes here
            s.alternatives.clear();
            if (shared)
            {
                std::vector<int> all = best[0].Alternatives;
                all.insert(all.begin(), best[0].Member);
                for (int g : all)
                    if (g != (int)m.Global && s.alternatives.size() < 16) s.alternatives.push_back(GlobalPath(x, g));
            }
            out.ram.push_back(s);
        }
    out.loads = ContainerLoads(wb, cont, c, m, 16);
    return true;
}

bool OriginRanges(Workbench& wb, const WBApi::OriginRangesParams& p, WBApi::OriginRangesResult& out, QString& err)
{
    QString want = p.source.toLower();
    std::vector<u32> ids;
    for (const DataSource& s : wb.Taint.Sources())
    {
        QString n = QString::fromStdString(s.Name).toLower();
        bool tail = n.size() > want.size() && n.endsWith(want) &&
                    (n[n.size() - want.size() - 1] == '/' || n[n.size() - want.size() - 1] == ':');
        if (want.isEmpty() || n == want || tail)
        {
            ids.push_back(s.Id);
            out.sources.push_back(QString::fromStdString(s.Name));
        }
    }
    if (!want.isEmpty() && ids.empty()) { err = "no data origin source named " + p.source; return false; }

    std::unordered_map<u32, bool> known;
    auto matches = [&](u32 label)
    {
        if (!label) return false;
        auto it = known.find(label);
        if (it != known.end()) return it->second;
        bool m = want.isEmpty();
        for (u32 s : wb.Taint.LabelSources(label))
            if (!m && std::find(ids.begin(), ids.end(), s) != ids.end()) m = true;
        known[label] = m;
        return m;
    };
    QString rsel = p.region.toLower();
    auto regionWanted = [&](u16 r) -> bool
    {
        if (rsel == "all" || rsel.isEmpty()) return true;
        if (rsel == "main_ram") return r == (u16)Region::MainRAM || IsOverlayRegion(r);
        if (rsel == "itcm") return r == (u16)Region::ITCM;
        if (rsel == "dtcm") return r == (u16)Region::DTCM;
        if (rsel == "shared_wram") return r == (u16)Region::SharedWRAM;
        if (rsel == "arm7_wram") return r == (u16)Region::ARM7WRAM;
        if (rsel == "vram") return r == (u16)Region::VRAM;
        return QString::fromStdString(RegionLabel(r)) == rsel;
    };
    u32 lo = p.addr, hi = p.size ? p.addr + p.size : 0xFFFFFFFF;
    u32 limit = std::max<u32>(p.limit, 1);
    WBApi::OriginRange cur;
    bool open = false;
    auto flush = [&]()
    {
        if (!open) return;
        open = false;
        if (out.ranges.size() >= limit) { out.truncated = true; if (!out.next_addr) out.next_addr = cur.start; return; }
        out.ranges.push_back(cur);
        out.total_bytes += cur.end - cur.start;
    };
    wb.Taint.MemLabels().ForEach([&](u16 region, u32 page, const LabelPage* pg)
    {
        if (out.truncated || !regionWanted(region)) return;
        u32 base = CanonicalAddr(wb, region, page << PageShift);
        if (base + PageSize <= lo || base >= hi) { flush(); return; }
        for (u32 i = 0; i < PageSize && !out.truncated; i++)
        {
            u32 addr = base + i;
            if (addr < lo || addr >= hi || !matches(pg->L[i])) { flush(); continue; }
            if (open && cur.end != addr) flush();
            if (!open)
            {
                open = true;
                cur = WBApi::OriginRange();
                cur.start = addr;
                cur.end = addr;
            }
            cur.end = addr + 1;
        }
    });
    flush();
    return true;
}

bool Origin(Workbench& wb, const WBApi::OriginParams& p, WBApi::OriginResult& out, QString& err, const ArchiveCtx& arc,
            const ContainerCtx& cont)
{
    u32 cpu;
    if (!ParseCpu(p.cpu, cpu, err)) return false;
    u32 size = std::max<u32>(p.size, 1);
    out.origin_enabled = wb.Taint.Enabled();
    out.state_loaded_frame = wb.Taint.StateLoadedFrame();

    const auto& prov = wb.Rec.Provenance();
    u32 a = p.addr;
    const u32 queryCpu = cpu;
    size_t limit = prov.size();
    bool pureCopies = true;    // every hop copied bytes unchanged, so the queried bytes equal the chain's source
    for (int hop = 0; hop < 12; hop++)
    {
        // each hop looks further back in time: the source was written before it was copied
        const ProvenanceEvent* found = nullptr;
        for (size_t i = limit; i-- > 0; )
        {
            const ProvenanceEvent& ev = prov[i];
            if (a >= ev.dst_addr && a < ev.dst_addr + std::max<u32>(ev.dst_size, 1))
            {
                if (ev.kind == (u8)ProvenanceKind::DMA && ev.src_addr == ev.dst_addr) continue;
                found = &ev;
                limit = i;
                break;
            }
        }
        if (!found) break;
        if (hop == 0)
        {
            // the queried bytes may have been rewritten by ordinary code after this event
            ByteRecord r;
            if (ByteAt(wb, nullptr, cpu, a, r) && (r.write_count || r.dma_write_count) && r.last_write_frame >= found->frame)
            {
                bool cpuWriter = (r.last_writer_cpu & 0xF) < 2;
                bool stale = false;
                switch ((ProvenanceKind)found->kind)
                {
                case ProvenanceKind::BootLoad: stale = true; break;
                case ProvenanceKind::DMA: stale = cpuWriter; break;
                case ProvenanceKind::Decompress: stale = cpuWriter && (r.last_writer_pc >> 16) != 0xFFFF && r.last_writer_pc >= 0x4000; break;
                case ProvenanceKind::CpuCopy:
                case ProvenanceKind::CardRead:
                    if (found->cpu >= 2) stale = cpuWriter;   // DMA'd card data later rewritten by code
                    else stale = cpuWriter && (r.last_writer_pc + 0x80 < found->pc || r.last_writer_pc > found->pc + 0x80);
                    break;
                default: break;
                }
                if (stale) break;
            }
        }
        WBApi::ProvenanceInfo info;
        info.kind = ProvKind(found->kind);
        info.frame = found->frame;
        info.pc = found->pc;
        info.src_addr = found->src_addr;
        info.src_size = found->src_size;
        info.dst_addr = found->dst_addr;
        info.dst_size = found->dst_size;
        info.rom_offset = found->rom_offset;
        info.file_id = found->file_id == FileNone ? -1 : (s32)found->file_id;
        if (found->file_id != FileNone)
            if (const RomFile* f = wb.Rom.FileById(found->file_id)) info.file_name = QString::fromStdString(f->Path);
        if (found->kind == (u8)ProvenanceKind::Decompress) info.detail = QString("BIOS SWI 0x%1").arg(found->aux, 2, 16, QChar('0'));
        else if (found->kind == (u8)ProvenanceKind::DMA) info.detail = QString("%1 channel %2").arg(CpuName(found->cpu)).arg(found->channel);
        else if (found->kind == (u8)ProvenanceKind::CpuCopy) info.detail = QString("copied by %1 code").arg(CpuName(found->cpu));
        else if (found->kind == (u8)ProvenanceKind::CardRead)
        {
            u32 fileoff = 0;
            if (const RomFile* f = wb.Rom.FileById(found->file_id)) fileoff = found->rom_offset + (a - found->dst_addr) - f->Start;
            info.detail = QString("file offset 0x%1").arg(fileoff, 0, 16);
            info.archive_member = ArchiveMemberAt(arc, found->file_id, found->rom_offset + (a - found->dst_addr));
        }
        if (found->kind == (u8)ProvenanceKind::BootLoad)
        {
            info.kind = "boot_load";
            info.detail = QString("file offset 0x%1, loaded when the game booted").arg(a - found->dst_addr, 0, 16);
        }
        out.chain.insert(out.chain.begin(), info);
        if (found->kind == (u8)ProvenanceKind::CardRead || found->kind == (u8)ProvenanceKind::BootLoad) break;
        if ((found->kind == (u8)ProvenanceKind::DMA || found->kind == (u8)ProvenanceKind::CpuCopy) && found->src_size == found->dst_size)
            a = found->src_addr + (a - found->dst_addr);
        else a = found->src_addr;
        if (found->kind == (u8)ProvenanceKind::CpuCopy) cpu = found->cpu & 1;
        if (!((found->kind == (u8)ProvenanceKind::DMA || found->kind == (u8)ProvenanceKind::CpuCopy) && found->src_size == found->dst_size))
            pureCopies = false;
    }

    bool fromRom = !out.chain.empty() && (out.chain.front().kind == "card_read" || out.chain.front().kind == "boot_load");
    if (!fromRom && wb.Rom.Valid())
    {
        // the start of the chain was loaded before recording began (boot, or before the toggle):
        // compare with the static modules and overlays in the ROM image
        // loaded before recording began: compare with the static modules and overlays in the ROM
        auto check = [&](u32 fileid, u32 romoff, u32 ram, u32 len) -> bool
        {
            if (a < ram || a >= ram + len) return false;
            // through pure copies the queried bytes are the original ones, even if the source was reused since;
            // compare a run of at least 4 bytes so a coincidental byte does not count as a match
            u32 n = std::min<u32>(std::max<u32>(size, 4), 16);
            if (a + n > ram + len) return false;
            for (u32 i = 0; i < n; i++)
            {
                u32 v;
                if (pureCopies ? !wb.Peek(queryCpu, p.addr + i, 1, v) : !wb.Peek(cpu, a + i, 1, v)) return false;
                if (wb.Rom.Data()[romoff + (a - ram) + i] != (u8)v) return false;
            }
            WBApi::ProvenanceInfo info;
            info.kind = "boot_load";
            info.src_addr = romoff;
            info.src_size = len;
            info.dst_addr = ram;
            info.dst_size = len;
            info.rom_offset = romoff;
            info.file_id = (s32)fileid;
            if (const RomFile* f = wb.Rom.FileById(fileid)) info.file_name = QString::fromStdString(f->Path);
            info.detail = QString("file offset 0x%1 (bytes still match the ROM)").arg(a - ram, 0, 16);
            out.chain.insert(out.chain.begin(), info);
            return true;
        };
        if (!check(FileARM9, wb.Rom.ARM9RomOffset, wb.Rom.ARM9RamAddress, wb.Rom.ARM9Size) &&
            !check(FileARM7, wb.Rom.ARM7RomOffset, wb.Rom.ARM7RamAddress, wb.Rom.ARM7Size))
        {
            for (const OverlayInfo& o : wb.Rom.Overlays9)
            {
                const RomFile* f = wb.Rom.FileById(o.FileId);
                if (f && !o.Compressed() && check(o.FileId, f->Start, o.RamStart, std::min(o.RamSize, f->End - f->Start))) break;
            }
        }
    }

    if (!fromRom && !(out.chain.size() && (out.chain.front().kind == "boot_load")))
    {
        // the start of the chain was produced by code (decompression, decoding, computation)
        ByteRecord r;
        if (ByteAt(wb, nullptr, cpu, a, r) && (r.write_count || r.dma_write_count) && (r.last_writer_cpu & 0xF) < 2)
        {
            WBApi::ProvenanceInfo info;
            info.kind = "computed";
            info.frame = r.last_write_frame;
            info.pc = r.last_writer_pc;
            info.dst_addr = a;
            info.dst_size = 1;
            info.rom_offset = 0xFFFFFFFF;
            info.file_id = -1;
            info.detail = QString("written by %1 code at %2").arg(CpuName(r.last_writer_cpu & 0xF)).arg(r.last_writer_pc, 8, 16, QChar('0'));
            out.chain.insert(out.chain.begin(), info);
        }
    }

    // taint runs
    u32 runStart = p.addr, runLabel = 0;
    bool have = false;
    auto addRun = [&](u32 start, u32 endx, u32 label)
    {
        if (!label) return;
        WBApi::TaintInfo t;
        t.addr = start;
        t.size = endx - start;
        t.label = label;
        for (u32 s : wb.Taint.LabelSources(label))
            if (const DataSource* src = wb.Taint.FindSource(s)) t.sources.push_back(QString::fromStdString(src->Name));
        HowNode h = CollapseHow(wb.Taint, label);
        if (h.M)
        {
            t.how = TaintTable::MergeOpName(h.M->Op);
            t.how_pc = h.M->PC;
            t.how_cpu = CpuName(h.M->Cpu);
            t.how_frame = h.M->Frame;
            t.how_steps = h.Steps;
            t.how_frame_first = h.FrameFirst;
        }
        out.taint.push_back(t);
    };
    for (u32 i = 0; i < size; i++)
    {
        u32 l = wb.Taint.GetMemLabel(wb.Translate(queryCpu, p.addr + i));
        if (!have) { runStart = p.addr; runLabel = l; have = true; continue; }
        if (l != runLabel)
        {
            addRun(runStart, p.addr + i, runLabel);
            runStart = p.addr + i;
            runLabel = l;
        }
    }
    if (have) addRun(runStart, p.addr + size, runLabel);

    // where the labelled ROM files were read from the card, and by which code. Content files (NitroFS
    // data) come first; code and file-system tables (arm9.bin, overlays, fnt/fat) only when nothing else is labelled.
    auto isContent = [&](u32 fid)
    {
        if (fid >= 0x10000) return false;
        const RomFile* f = wb.Rom.FileById(fid);
        return f && f->OverlayId < 0;
    };
    std::vector<u32> content, system, members;    // members: archived files (index + 1)
    std::vector<std::pair<u32, u32>> contained;   // files inside containers: {container file, index + 1}
    for (const WBApi::TaintInfo& t : out.taint)
        for (u32 s : wb.Taint.LabelSources(t.label))
            if (const DataSource* src = wb.Taint.FindSource(s))
                if (src->Kind == SourceKind::RomFile)
                {
                    if (src->Addr && arc.Index && src->FileId == arc.Index->ArchiveFileId)
                    {
                        if (std::find(members.begin(), members.end(), src->Addr) == members.end()) members.push_back(src->Addr);
                        continue;
                    }
                    if (src->Addr && cont.Index && cont.Index->ByFile(src->FileId))
                    {
                        std::pair<u32, u32> k {src->FileId, src->Addr};
                        if (std::find(contained.begin(), contained.end(), k) == contained.end()) contained.push_back(k);
                        continue;
                    }
                    auto& v = isContent(src->FileId) ? content : system;
                    if (std::find(v.begin(), v.end(), src->FileId) == v.end()) v.push_back(src->FileId);
                }
    for (WBApi::TaintInfo& t : out.taint)
    {
        // list archived files first, then content files, in each run's sources
        auto rank = [&](const QString& name)
        {
            if (name.contains(':')) return 0;
            const RomFile* f = wb.Rom.FileByPath(name.toStdString());
            return f && isContent(f->Id) ? 1 : 2;
        };
        std::stable_sort(t.sources.begin(), t.sources.end(), [&](const QString& a, const QString& b) { return rank(a) < rank(b); });
    }
    // card reads of the archived files the labels name: their loaders
    for (u32 mi : members)
        if (mi <= arc.Index->Members().size())
            for (const WBApi::ProvenanceInfo& l : MemberLoads(wb, arc, arc.Index->Members()[mi - 1], 3))
                out.file_reads.push_back(l);
    for (auto [fid, mi] : contained)
        if (const WBArc::Container* c = cont.Index->ByFile(fid); c && mi <= c->Members.size())
            for (const WBApi::ProvenanceInfo& l : ContainerLoads(wb, cont, *c, c->Members[mi - 1], 3))
                out.file_reads.push_back(l);
    // the archive's own reads add nothing beyond its files' reads
    std::vector<u32> files = !members.empty() || !contained.empty() ? std::vector<u32>() : (content.empty() ? system : content);
    for (u32 fid : files)
    {
        int n = 0;
        for (size_t i = prov.size(); i-- > 0 && n < 3; )
        {
            const ProvenanceEvent& ev = prov[i];
            if (ev.file_id != fid || (ev.kind != (u8)ProvenanceKind::CardRead && ev.kind != (u8)ProvenanceKind::BootLoad)) continue;
            WBApi::ProvenanceInfo info;
            info.kind = ev.kind == (u8)ProvenanceKind::CardRead ? "card_read" : "boot_load";
            info.frame = ev.frame;
            info.pc = ev.pc;
            info.src_addr = ev.src_addr;
            info.src_size = ev.src_size;
            info.dst_addr = ev.dst_addr;
            info.dst_size = ev.dst_size;
            info.rom_offset = ev.rom_offset;
            info.file_id = (s32)fid;
            if (const RomFile* f = wb.Rom.FileById(fid))
            {
                info.file_name = QString::fromStdString(f->Path);
                info.detail = QString("file offset 0x%1").arg(ev.rom_offset - f->Start, 0, 16);
            }
            info.archive_member = ArchiveMemberAt(arc, fid, ev.rom_offset);
            if (info.archive_member.isEmpty()) info.archive_member = ContainerMemberAt(cont, fid, ev.rom_offset);
            out.file_reads.push_back(info);
            n++;
        }
    }

    // content: the queried bytes may equal a file of a detected archive, whatever path they took
    out.archive_status = arc.Status;
    out.archive = ArchiveSegments(wb, arc, p.addr, size, 257);
    out.archive_truncated = out.archive.size() > 256;
    if (out.archive_truncated) out.archive.pop_back();
    out.containers_status = cont.Status;
    out.containers = ContainerSegments(wb, cont, p.addr, size, 257);
    out.containers_truncated = out.containers.size() > 256;
    if (out.containers_truncated) out.containers.pop_back();
    return true;
}

bool FetchCode(Workbench& wb, u32 cpu, u32 addr, bool thumb, u32& word)
{
    if (thumb)
    {
        u32 lo = 0, hi = 0;
        bool ok = wb.Peek(cpu, addr, 2, lo);
        wb.Peek(cpu, addr + 2, 2, hi);
        word = (lo & 0xFFFF) | (hi << 16);
        return ok;
    }
    return wb.Peek(cpu, addr, 4, word);
}

bool Disasm(Workbench& wb, const WBApi::DisasmParams& p, WBApi::DisasmResult& out, QString& err, const KbHints* kb,
            const std::vector<DataUnit>* units, const AnalysisHints* analysis)
{
    KbHints none;
    u32 cpu;
    if (!ParseCpu(p.cpu, cpu, err)) return false;
    QString mode = p.mode.toLower();
    if (mode != "auto" && mode != "arm" && mode != "thumb") { err = "mode must be auto, arm or thumb"; return false; }
    u32 count = std::min<u32>(std::max<u32>(p.count, 1), 4096);

    // an overlay epoch: where another overlay is loaded, that overlay's own records and its bytes from the ROM
    const OverlayInfo* ov = nullptr;
    std::vector<u8> image;
    NDS* nds = wb.GetNDS();
    u32 mask = nds ? nds->MainRAMMask : 0x3FFFFF;
    int ovCode = -1;
    if (p.overlay >= 0)
    {
        ov = wb.Rom.Overlay(cpu, (u32)p.overlay);
        if (!ov) { err = QString("the ROM has no %1 overlay %2").arg(CpuName(cpu)).arg(p.overlay); return false; }
        if ((ov->RamStart >> 24) != 0x02) { err = "only overlays in main RAM are tracked, so only they have epochs"; return false; }
        if (!OverlayImage(wb.Rom, *ov, image)) { err = QString("cannot read overlay %1 from the ROM").arg(p.overlay); return false; }
        ovCode = (int)((cpu << 15) | ov->Id);
        out.overlay_loaded = wb.Rec.OverlayAt(ov->RamStart & mask) == ovCode;
    }
    auto epochAt = [&](u32 addr) { return ov && addr >= ov->RamStart && addr < ov->RamEnd() && wb.Rec.OverlayAt(addr & mask) != ovCode; };
    auto recordAt = [&](u32 addr, ByteRecord& r) -> bool
    {
        if (!epochAt(addr)) return ByteAt(wb, nullptr, cpu, addr, r);
        memset(&r, 0, sizeof(r));
        u16 region = (u16)(cpu ? Region::OverlayARM7Base : Region::OverlayARM9Base) + (u16)ov->Id;
        u32 off = addr & mask;
        RecordPage* pg = wb.Rec.Store().Get(region, off >> PageShift);
        if (!pg) return false;
        r = pg->Rec[off & (PageSize - 1)];
        return true;
    };
    auto peek = [&](u32 addr, u32 size, u32& v) -> bool
    {
        if (!epochAt(addr)) return wb.Peek(cpu, addr, size, v);
        v = 0;
        u32 off = addr - ov->RamStart;
        for (u32 i = 0; i < size; i++)
            if (off + i < image.size()) v |= (u32)image[off + i] << (8 * i);   // past the file: the BSS, zero when loaded
        return true;
    };
    auto fetch = [&](u32 addr, bool thumb, u32& word)
    {
        if (!thumb) return peek(addr, 4, word);
        u32 lo = 0, hi = 0;
        bool ok = peek(addr, 2, lo);
        peek(addr + 2, 2, hi);
        word = (lo & 0xFFFF) | (hi << 16);
        return ok;
    };

    u32 a = p.addr;
    for (u32 n = 0; n < count; n++)
    {
        ByteRecord at;
        recordAt(a, at);
        int rm = !at.exec_count ? -1 : ((at.flags & ByteFlag::Thumb) && !(at.flags & ByteFlag::Arm)) ? 1
               : (at.flags & ByteFlag::Arm) ? 0 : (at.flags & ByteFlag::Thumb) ? 1 : 0;
        // decoded the way every view shows it: recorded mode, override, data object, function
        Classification c = Classify(&at, kb ? *kb : none, a, analysis);
        bool thumb;
        bool data = false;
        if (mode == "arm") thumb = false;
        else if (mode == "thumb") thumb = true;
        else if (!c.IsCode()) { thumb = false; data = true; }
        else thumb = c.Kind == CodeKind::Thumb || (c.Kind == CodeKind::Mixed && rm == 1);
        // data of a defined type starts at its fields, whatever their alignment
        const DataUnit* unit = nullptr;
        if (data && units)
        {
            auto it = std::upper_bound(units->begin(), units->end(), a, [](u32 v, const DataUnit& u) { return v < u.Addr; });
            if (it != units->begin() && a < std::prev(it)->Addr + std::prev(it)->Size) unit = &*std::prev(it);
        }
        if (unit) a = unit->Addr;
        else if (!thumb) a &= ~3u;
        else a &= ~1u;

        WBApi::DisasmLine line;
        line.addr = a;
        line.thumb = thumb;
        line.classification = KindName(c.Kind);
        line.confidence = EvidenceName(c.From);
        line.conflict = c.Conflict;
        line.why = c.Why;
        ByteRecord r;
        recordAt(a, r);
        line.executed = r.exec_count > 0;
        u32 word = 0;
        fetch(a, thumb, word);
        if (unit && unit->Str)
        {
            QString s;
            for (u32 i = 0; i < std::min<u32>(unit->Size, 48); i++)
            {
                u32 ch = 0;
                if (!peek(a + i, 1, ch) || !ch) break;
                s += (ch >= 32 && ch < 127) ? QChar((char)ch) : QChar('.');
            }
            line.text = QString(".asciz \"%1\" ; %2").arg(s, unit->Name);
            a += unit->Size;
        }
        else if (unit)
        {
            u32 v = 0;
            peek(a, unit->Size, v);
            int digits = (int)unit->Size * 2;
            line.bytes = QString("%1").arg(v, digits, 16, QChar('0'));
            line.text = QString("%1 0x%2 ; %3").arg(unit->Size == 1 ? ".byte" : unit->Size == 2 ? ".hword" : ".word")
                            .arg(v, digits, 16, QChar('0')).arg(unit->Name);
            if (unit->Size == 4 && ((v >> 24) == 0x02 || (v >> 24) == 0x03)) line.target = v;
            a += unit->Size;
        }
        else if (data)
        {
            line.bytes = QString("%1").arg(word, 8, 16, QChar('0'));
            line.text = QString(".word 0x%1").arg(word, 8, 16, QChar('0'));
            if ((word >> 24) == 0x02 || (word >> 24) == 0x01 || (word >> 24) == 0x03) line.target = word;
            a += 4;
        }
        else
        {
            DisasmInsn in;
            Disassemble(a, word, thumb, in);
            if (in.Size == 4 && thumb) line.bytes = QString("%1 %2").arg(word & 0xFFFF, 4, 16, QChar('0')).arg(word >> 16, 4, 16, QChar('0'));
            else if (thumb) line.bytes = QString("%1").arg(word & 0xFFFF, 4, 16, QChar('0'));
            else line.bytes = QString("%1").arg(word, 8, 16, QChar('0'));
            line.text = QString::fromStdString(in.Text());
            if (in.HasTarget) line.target = in.Target;
            else if (in.HasRef)
            {
                line.target = in.Ref;
                if (in.LiteralLoad)
                {
                    u32 v;
                    if (peek(in.Ref, 4, v)) line.text += QString(" ; =0x%1").arg(v, 8, 16, QChar('0'));
                }
            }
            a += in.Size;
        }
        out.lines.push_back(line);
    }
    return true;
}

bool CaptureScreens(Workbench& wb, QImage& top, QImage& bottom, QString& err)
{
    NDS* nds = wb.GetNDS();
    if (!nds) { err = "no game running"; return false; }
    void* t = nullptr;
    void* b = nullptr;
    if (!nds->GPU.GetFramebuffers(&t, &b) || !t || !b)
    {
#ifdef OGLRENDERER_ENABLED
        // OpenGL renderers: both screens are the two layers of the front output texture.
        // Runs on the emulation thread, which owns the current GL context.
        if (t && !b)
        {
            GLuint tex = *(GLuint*)t;
            GLint w = 0, h = 0, layers = 0;
            glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &layers);
            if (w > 0 && h > 0 && layers >= 2 && glGetError() == GL_NO_ERROR)
            {
                std::vector<u8> buf((size_t)w * h * 4 * layers);
                // the renderer leaves its own pack state (row length, pixel buffer objects) set: use defaults, then restore
                GLint pbo = 0, rowlen = 0, skiprows = 0, skippix = 0, align = 4;
                glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
                glGetIntegerv(GL_PACK_ROW_LENGTH, &rowlen);
                glGetIntegerv(GL_PACK_SKIP_ROWS, &skiprows);
                glGetIntegerv(GL_PACK_SKIP_PIXELS, &skippix);
                glGetIntegerv(GL_PACK_ALIGNMENT, &align);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glFinish();
                glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_BGRA, GL_UNSIGNED_BYTE, buf.data());
                GLenum readErr = glGetError();
                glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
                glPixelStorei(GL_PACK_ROW_LENGTH, rowlen);
                glPixelStorei(GL_PACK_SKIP_ROWS, skiprows);
                glPixelStorei(GL_PACK_SKIP_PIXELS, skippix);
                glPixelStorei(GL_PACK_ALIGNMENT, align);
                if (readErr == GL_NO_ERROR)
                {
                    auto layer = [&](int i)
                    {
                        QImage img(buf.data() + (size_t)w * h * 4 * i, w, h, QImage::Format_RGB32);
                        return img.scaled(256, 192, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    };
                    top = layer(0);
                    bottom = layer(1);
                    return true;
                }
            }
        }
#endif
        err = "the renderer did not provide its framebuffers";
        return false;
    }
    top = QImage(256, 192, QImage::Format_RGB32);
    bottom = QImage(256, 192, QImage::Format_RGB32);
    memcpy(top.bits(), t, 256 * 192 * 4);
    memcpy(bottom.bits(), b, 256 * 192 * 4);
    return true;
}

bool OverlayImage(const NitroROM& rom, const OverlayInfo& ov, std::vector<u8>& out)
{
    const RomFile* f = rom.FileById(ov.FileId);
    if (!f || f->End <= f->Start || f->End > rom.Length()) return false;
    const u8* data = rom.Data() + f->Start;
    u32 len = f->End - f->Start;
    if (ov.Compressed()) return UnpackBackwardLz(data, len, out);
    out.assign(data, data + len);
    return true;
}

void RuntimeXrefs(Workbench& wb, const MergedSessions* all, u32 cpu, u32 addr, u32 fsize, std::vector<XrefRow>& rows)
{
    WBApi::CallsResult cr;
    Calls(wb, all, cpu, addr, fsize, 500, cr);
    for (const auto& e : cr.callers) rows.push_back({"call", e.call_site, "runtime", e.count});
    // writers: from write history, and the last writer the byte records keep
    std::map<u32, u64> writers;
    wb.Rec.ForEachHistory([&](const WriteHistoryEntry& e)
    {
        if (e.cpu == (u8)cpu && e.addr <= addr && addr < e.addr + e.size) writers[e.pc]++;
    });
    ByteRecord r;
    if (ByteAt(wb, nullptr, cpu, addr, r) && r.write_count && (r.last_writer_cpu & 0xF) == (u8)cpu) writers.emplace(r.last_writer_pc, 0);
    for (auto& kv : writers) rows.push_back({"write", kv.first, "runtime", kv.second});
    PhysAddr pa = wb.Translate(cpu, addr);
    if (pa.Recordable())
        if (RecordPage* p = wb.Rec.Store().Get(pa.Region, pa.Offset >> PageShift))
            for (int k = 0; k < 2; k++)
            {
                u32 rd = p->Readers[(pa.Offset & (PageSize - 1)) >> 2][k];
                if (rd && (rd & 1) == cpu) rows.push_back({"read", rd & ~1u, "runtime", 0});
            }
    // literal pool words in executed code that hold this address
    wb.Rec.Store().ForEach([&](u16 region, u32 page, const RecordPage* p)
    {
        for (u32 i = 0; i < PageSize; i += 4)
        {
            if (!(p->Rec[i].flags & ByteFlag::LiteralPool)) continue;
            PhysAddr q {region, (page << PageShift) + i};
            u8* ptr = wb.PhysPtr(q);
            if (!ptr) continue;
            u32 v;
            memcpy(&v, ptr, 4);
            if ((v & ~1u) == addr || (v >= addr && v < addr + 4))
            {
                u32 from = CanonicalAddr(wb, region, q.Offset);
                u32 reader = p->Readers[i >> 2][0] & ~1u;
                rows.push_back({"literal", reader ? reader : from, "runtime", 0});
            }
        }
    });
}

// ---------------------------------------------------------------- search

namespace
{
struct SearchRange { u32 Start, Size; };
std::vector<SearchRange> SearchRanges(Workbench& wb, bool wide)
{
    std::vector<SearchRange> r;
    NDS* nds = wb.GetNDS();
    if (!nds) return r;
    r.push_back({0x02000000, nds->MainRAMMask + 1});
    if (wide)
    {
        r.push_back({0x03000000, 0x8000});
        if (nds->ARM9.DTCMBase != 0xFFFFFFFF) r.push_back({nds->ARM9.DTCMBase, 0x4000});
        r.push_back({0x01FF8000, 0x8000});
    }
    return r;
}
}

bool ParseBytePattern(const QString& text, QByteArray& pat, QByteArray& mask, QString& err)
{
    QStringList parts = text.simplified().split(' ', Qt::SkipEmptyParts);
    if (parts.size() == 1 && parts[0].size() > 2)
    {
        QString s = parts[0];
        parts.clear();
        for (int i = 0; i + 1 < s.size(); i += 2) parts << s.mid(i, 2);
        if (s.size() & 1) { err = "odd number of hex digits"; return false; }
    }
    pat.clear();
    mask.clear();
    for (const QString& p : parts)
    {
        if (p == "??" || p == "?") { pat.append('\0'); mask.append('\0'); continue; }
        bool ok;
        uint v = p.toUInt(&ok, 16);
        if (!ok || v > 255) { err = "bad byte '" + p + "'"; return false; }
        pat.append((char)v);
        mask.append('\1');
    }
    if (pat.isEmpty()) { err = "empty pattern"; return false; }
    if (!mask.contains('\1')) { err = "the pattern has only wildcards"; return false; }
    return true;
}

bool EncodeSearchText(const QString& text, const QString& encoding, QByteArray& out, QString& err)
{
    if (text.isEmpty()) { err = "empty text"; return false; }
    QString e = encoding.toLower().remove('-').remove('_');
    if (e == "ascii" || e == "latin1")
    {
        // toLatin1 would turn other characters into '?' and search for those
        for (QChar ch : text)
            if (ch.unicode() > 0xFF) { err = QString("'%1' is not ASCII or Latin-1: search with sjis or utf16le").arg(ch); return false; }
        out = text.toLatin1();
    }
    else if (e == "sjis" || e == "shiftjis")
    {
        QStringEncoder enc("Shift_JIS");
        if (!enc.isValid()) { err = "Shift-JIS encoding is not available in this Qt build"; return false; }
        out = enc.encode(text);
    }
    else if (e == "utf16le" || e == "utf16") out = QByteArray((const char*)text.utf16(), text.size() * 2);
    else { err = "encoding must be ascii, sjis or utf16le"; return false; }
    return true;
}

std::vector<SearchHit> FindPattern(Workbench& wb, const QByteArray& pat, const QByteArray& mask, bool wide, size_t max)
{
    std::vector<SearchHit> out;
    for (const SearchRange& sr : SearchRanges(wb, wide))
    {
        std::vector<u8> buf(sr.Size);
        for (u32 i = 0; i < sr.Size; i += 4)
        {
            u32 v = 0;
            wb.Peek(0, sr.Start + i, 4, v);
            memcpy(&buf[i], &v, std::min<u32>(4, sr.Size - i));
        }
        for (u32 i = 0; i + pat.size() <= sr.Size && out.size() < max; i++)
        {
            bool ok = true;
            for (int k = 0; k < pat.size(); k++)
                if (mask[k] && buf[i + k] != (u8)pat[k]) { ok = false; break; }
            if (!ok) continue;
            QString prev;
            for (int k = 0; k < 16 && i + k < sr.Size; k++) prev += (buf[i + k] >= 0x20 && buf[i + k] < 0x7F) ? QChar(buf[i + k]) : QChar('.');
            out.push_back({sr.Start + i, prev});
        }
    }
    return out;
}

bool ScanNeedsValue(ScanCond c)
{
    return c == ScanCond::Equal || c == ScanCond::NotEqual || c == ScanCond::Greater || c == ScanCond::Less;
}

bool ParseScanValue(const QString& text, ScanType type, s64& out)
{
    QString s = text.trimmed();
    bool ok;
    if (type == ScanType::FX32) { double d = s.toDouble(&ok); out = (s64)std::llround(d * 4096.0); return ok; }
    if (s.startsWith("0x", Qt::CaseInsensitive)) out = (s64)s.mid(2).toULongLong(&ok, 16);
    else out = s.toLongLong(&ok);
    return ok;
}

bool ScanValueInRange(ScanType type, s64 value, QString& err)
{
    s64 lo = 0, hi = 0xFFFFFFFFLL;
    const char* name = "u32";
    switch (type)
    {
    case ScanType::U8: hi = 0xFF; name = "u8"; break;
    case ScanType::U16: hi = 0xFFFF; name = "u16"; break;
    case ScanType::S32: lo = INT32_MIN; hi = INT32_MAX; name = "s32"; break;
    case ScanType::FX32: lo = INT32_MIN; hi = INT32_MAX; name = "fx32"; break;
    default: break;
    }
    if (value >= lo && value <= hi) return true;
    if (type == ScanType::FX32) err = "fx32 values are from -524288 to 524287.999";
    else err = QString("%1 values are from %2 to %3%4").arg(name).arg(lo).arg(hi)
                   .arg(lo == 0 && value < 0 ? ": scan as s32 for negative values" : "");
    return false;
}

double ScanDecode(ScanType type, u32 raw)
{
    switch (type)
    {
    case ScanType::S32: return (double)(s32)raw;
    case ScanType::FX32: return (double)(s32)raw / 4096.0;
    default: return (double)raw;
    }
}

static bool ScanSigned(ScanType t) { return t == ScanType::S32 || t == ScanType::FX32; }

void ScanFirst(Workbench& wb, ScanState& s, ScanCond cond, s64 value, bool wide)
{
    s.Addrs.clear();
    s.Values.clear();
    u32 size = s.Size();
    for (const SearchRange& sr : SearchRanges(wb, wide))
        for (u32 i = 0; i + size <= sr.Size; i += size)
        {
            u32 v = 0;
            wb.Peek(0, sr.Start + i, size, v);
            s64 sv = ScanSigned(s.Type) ? (s64)(s32)v : (s64)v;
            bool keep = true;
            if (cond == ScanCond::Equal) keep = sv == value;
            else if (cond == ScanCond::NotEqual) keep = sv != value;
            else if (cond == ScanCond::Greater) keep = sv > value;
            else if (cond == ScanCond::Less) keep = sv < value;
            if (keep) { s.Addrs.push_back(sr.Start + i); s.Values.push_back(v); }
        }
}

void ScanNext(Workbench& wb, ScanState& s, ScanCond cond, s64 value)
{
    std::vector<u32> addrs, values;
    u32 size = s.Size();
    bool sgn = ScanSigned(s.Type);
    for (size_t i = 0; i < s.Addrs.size(); i++)
    {
        u32 v = 0;
        wb.Peek(0, s.Addrs[i], size, v);
        s64 sv = sgn ? (s64)(s32)v : (s64)v;
        s64 sp = sgn ? (s64)(s32)s.Values[i] : (s64)s.Values[i];
        bool keep;
        switch (cond)
        {
        case ScanCond::Equal: keep = sv == value; break;
        case ScanCond::NotEqual: keep = sv != value; break;
        case ScanCond::Changed: keep = v != s.Values[i]; break;
        case ScanCond::Unchanged: keep = v == s.Values[i]; break;
        case ScanCond::Increased: keep = sv > sp; break;
        case ScanCond::Decreased: keep = sv < sp; break;
        case ScanCond::Greater: keep = sv > value; break;
        case ScanCond::Less: keep = sv < value; break;
        default: keep = true; break;
        }
        if (keep) { addrs.push_back(s.Addrs[i]); values.push_back(v); }
    }
    s.Addrs.swap(addrs);
    s.Values.swap(values);
}

}
