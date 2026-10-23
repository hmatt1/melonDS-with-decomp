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
#include <cstring>
#include "Workbench.h"
#include "Disasm.h"
#include "../NDS.h"
#include "../ARM.h"
#include "../GPU.h"

namespace melonDS::WB
{

static constexpr u32 CardDataPort = 0x04100010;

Recorder::Recorder(Workbench& wb) : WB(wb)
{
    Owner.assign(0x1000000 / 4, 0);
    PageMixed.assign(0x1000000 >> PageShift, 0);
}

void Recorder::Reset()
{
    Records.Clear();
    if (!Ext)
    {
        History.clear();
        History.shrink_to_fit();
    }
    HistoryPos = HistoryUsed = 0;
    Skipped = 0;
    for (auto& w : Bulk) for (BulkRun& r : w) r = BulkRun();
    FirstFunc.clear();
    for (int i = 0; i < 2; i++)
    {
        CallEdges[i].clear();
        KnownEntries[i].clear();
        EntryMode[i].clear();
        Stack[i].clear();
        ExpectedPC[i] = 0xFFFFFFFF;
        LastCard[i] = CardWord();
        CardCopy[i] = OpenTransfer();
        CpuCopy[i] = OpenTransfer();
        Decomp[i] = PendingDecompress();
        LoadPos[i] = 0;
        memset(Loads[i], 0, sizeof(Loads[i]));
    }
    for (auto& t : DmaOpen) t = OpenTransfer();
    ProvList.clear();
    OvlEvents.clear();
    TcmList.clear();
    EventLog.clear();
    EntryGen = IdentifiedGen = 0;
    PrologueScanned = false;
    DidIdentify = false;
    StartFrame = EndFrame = WB.Frame();
    WB.MapGen++;
}

void Recorder::LogEvent(RecEventKind kind, u16 value)
{
    RecEvent e {};
    e.frame = WB.Frame();
    e.kind = (u16)kind;
    e.value = value;
    e.timestamp = WB.Timestamp();
    EventLog.push_back(e);
}

void Recorder::NoteEntry(u32 cpu, u32 addr, bool thumb)
{
    cpu &= 1;
    addr &= ~1u;
    if (!addr || addr >= 0xFFFF0000) return;
    auto& m = EntryMode[cpu];
    auto it = m.find(addr);
    if (it != m.end()) return;
    m.emplace(addr, thumb ? 1 : 0);
    EntryGen++;
}

void Recorder::ScanExecutedPrologues()
{
    if (PrologueScanned) return;
    PrologueScanned = true;
    for (int c = 0; c < 2; c++)
        for (const auto& kv : CallEdges[c])
        {
            const CallEdge& e = kv.second;
            if (e.kind != (u8)CallKind::Call && e.kind != (u8)CallKind::TailCall) continue;
            NoteEntry((u32)c, e.callee & ~1u, e.thumb || (e.callee & 1));
        }
    if (!WB.GetNDS()) return;
    Records.ForEach([&](u16 region, u32 page, const RecordPage* p)
    {
        u32 base = 0;
        int cpuHint = -1;
        if (region == (u16)Region::MainRAM || IsOverlayRegion(region)) base = 0x02000000;
        else if (region == (u16)Region::ITCM) { base = 0x01FF8000; cpuHint = 0; }
        else if (region == (u16)Region::ARM7WRAM) { base = 0x03800000; cpuHint = 1; }
        else if (region == (u16)Region::SharedWRAM) base = 0x03000000;
        else return;
        for (u32 i = 0; i < PageSize; )
        {
            const ByteRecord& r = p->Rec[i];
            if (!(r.flags & ByteFlag::InstrStart) || !r.exec_count) { i++; continue; }
            bool thumb = (r.flags & ByteFlag::Thumb) && !(r.flags & ByteFlag::Arm);
            if (!thumb && (i & 3)) { i++; continue; }
            u32 vaddr = base + (page << PageShift) + i;
            int cpu = cpuHint >= 0 ? cpuHint : ((r.flags & ByteFlag::Exec7) && !(r.flags & ByteFlag::Exec9) ? 1 : 0);
            u32 instr = 0;
            if (WB.Peek((u32)cpu, vaddr, 4, instr) && IsFunctionPrologue(instr, thumb))
                NoteEntry((u32)cpu, vaddr, thumb);
            i += thumb ? 2 : 4;
        }
    });
}

bool Recorder::TakeEntries(std::vector<ObservedEntry>& out)
{
    ScanExecutedPrologues();
    if (DidIdentify && EntryGen == IdentifiedGen) return false;
    DidIdentify = true;
    IdentifiedGen = EntryGen;
    out.clear();
    for (int c = 0; c < 2; c++)
        for (const auto& kv : EntryMode[c])
            out.push_back({(u32)c, kv.first, kv.second != 0});
    return true;
}

void Recorder::SetEnabled(bool on)
{
    if (on == On) return;
    if (on)
    {
        StartFrame = WB.Frame();
        EndFrame = StartFrame;
        for (int i = 0; i < 2; i++)
        {
            Stack[i].clear();
            ExpectedPC[i] = 0xFFFFFFFF;
        }
        On = true;
        LogEvent(RecEventKind::RecordingOn);
        WB.TcmChanged();
    }
    else
    {
        FlushTransfers();
        EndFrame = WB.Frame();
        LogEvent(RecEventKind::RecordingOff);
        On = false;
    }
    WB.UpdateHookFlags();
}

void Recorder::SetWriteHistory(bool on)
{
    if (on == HistoryOn) return;
    HistoryOn = on;
    LogEvent(on ? RecEventKind::HistoryOn : RecEventKind::HistoryOff);
}

void Recorder::SetHistoryStorage(WriteHistoryEntry* buffer, u64 entries)
{
    History.clear();
    History.shrink_to_fit();
    Ext = buffer;
    if (buffer) Cap = std::max<u64>(entries, 1024);
    HistoryPos = HistoryUsed = 0;
}

u32 Recorder::HistoryOldestFrame() const
{
    u64 nslots = HistSlots();
    if (!nslots || !HistoryUsed) return 0;
    return HistData()[(HistoryPos - HistoryUsed) % nslots].frame;
}

void Recorder::SetHistoryCap(u64 entries)
{
    if (Ext) return;                // the history file's size decides
    if (entries < 1024) entries = 1024;
    if (entries == Cap) return;
    // keep the newest entries
    std::vector<WriteHistoryEntry> keep;
    keep.reserve(std::min<u64>(HistoryUsed, entries));
    u64 skip = HistoryUsed > entries ? HistoryUsed - entries : 0;
    u64 i = 0;
    ForEachHistory([&](const WriteHistoryEntry& e) { if (i++ >= skip) keep.push_back(e); });
    History = std::move(keep);
    HistoryUsed = History.size();
    HistoryPos = HistoryUsed;
    Cap = entries;
}

u32 Recorder::FramesRecorded() const
{
    u32 end = On ? WB.Frame() : EndFrame;
    return end >= StartFrame ? end - StartFrame : 0;
}

int Recorder::OverlayAt(u32 mainoff) const
{
    u16 o = Owner[(mainoff & 0xFFFFFF) >> 2];
    return o ? (int)(o - 1) : -1;
}

bool Recorder::HistoryWanted(u32 writer, u32 addr, u32 size, u32 pc, u32 sp, bool unchanged)
{
    if (HFilter.SkipUnchanged && unchanged) { Skipped++; return false; }
    // the stack: [sp - 256, sp + 16K) covers register spills and the current functions' locals
    if (HFilter.SkipStack && sp && addr - (sp - 0x100) < 0x4100) { Skipped++; return false; }
    if (HFilter.BulkThreshold)
    {
        BulkRun* runs = Bulk[writer & 3];
        BulkRun* r = nullptr;
        for (u32 i = 0; i < 4; i++)
            if (runs[i].Len && runs[i].End == addr) { r = &runs[i]; break; }
        if (!r) { r = &runs[BulkNext[writer & 3]++ & 3]; r->Len = 0; }
        r->End = addr + size;
        r->Len += size;
        if (r->Len > HFilter.BulkThreshold) { Skipped++; return false; }
    }
    if (!HistoryIncludes(addr, size)) { Skipped++; return false; }
    if (HistoryExcludesWriter(pc)) { Skipped++; return false; }
    return true;
}

// sorted by start, overlapping and touching ranges merged, empty ones dropped
static std::vector<std::pair<u32, u32>> NormalizeRanges(std::vector<std::pair<u32, u32>> v)
{
    v.erase(std::remove_if(v.begin(), v.end(), [](const std::pair<u32, u32>& r) { return r.second <= r.first; }), v.end());
    std::sort(v.begin(), v.end());
    std::vector<std::pair<u32, u32>> out;
    for (const auto& r : v)
    {
        if (!out.empty() && r.first <= out.back().second) out.back().second = std::max(out.back().second, r.second);
        else out.push_back(r);
    }
    return out;
}

// the range of normalized `v` with the highest start at or before addr, or end()
static std::vector<std::pair<u32, u32>>::const_iterator RangeAtOrBefore(const std::vector<std::pair<u32, u32>>& v, u32 addr)
{
    auto it = std::upper_bound(v.begin(), v.end(), addr, [](u32 a, const std::pair<u32, u32>& r) { return a < r.first; });
    return it == v.begin() ? v.end() : it - 1;
}

void Recorder::SetHistoryFilter(const HistoryFilter& f)
{
    HFilter = f;
    HFilter.Include = NormalizeRanges(f.Include);
    HFilter.ExcludeWriters = NormalizeRanges(f.ExcludeWriters);
}

bool Recorder::HistoryIncludes(u32 addr, u32 size) const
{
    const auto& v = HFilter.Include;
    if (v.empty()) return true;
    if (!size) size = 1;
    u64 end = (u64)addr + size;
    // the range starting at or before addr may reach into the write; else the first one after it
    auto it = RangeAtOrBefore(v, addr);
    if (it != v.end() && it->second > addr) return true;
    auto next = it == v.end() ? v.begin() : it + 1;
    return next != v.end() && next->first < end;
}

bool Recorder::HistoryExcludesWriter(u32 pc) const
{
    auto it = RangeAtOrBefore(HFilter.ExcludeWriters, pc);
    return it != HFilter.ExcludeWriters.end() && pc < it->second;
}

void Recorder::ComputeOverlaySpans()
{
    for (u32 c = 0; c < 2; c++)
    {
        u32 lo = 0xFFFFFFFF, hi = 0;
        for (const OverlayInfo& o : c ? WB.Rom.Overlays7 : WB.Rom.Overlays9)
        {
            if ((o.RamStart >> 24) != 0x02 || !o.RamSize) continue;
            lo = std::min(lo, o.RamStart);
            hi = std::max(hi, o.RamStart + o.RamSize);
        }
        OvlSpanStart[c] = lo == 0xFFFFFFFF ? 0 : lo;
        OvlSpanSize[c] = lo == 0xFFFFFFFF ? 0 : hi - lo;
        OvlDirty[c] = OvlSpanSize[c] != 0;
    }
    OvlImages.clear();
}

const u8* Recorder::OverlayImage(const OverlayInfo& o, u32& len)
{
    const RomFile* f = WB.Rom.FileById(o.FileId);
    if (!f || f->End <= f->Start) return nullptr;
    const u8* rom = WB.Rom.Data();
    if (!o.Compressed())
    {
        len = std::min(o.RamSize, f->End - f->Start);
        return rom + f->Start;
    }
    auto it = OvlImages.find((o.Cpu << 16) | o.Id);
    if (it == OvlImages.end())
    {
        std::vector<u8> img;
        u32 n = std::min(o.Flags & 0x00FFFFFF, f->End - f->Start);
        if (!n) n = f->End - f->Start;
        if (!UnpackBackwardLz(rom + f->Start, n, img) || img.size() != o.RamSize) img.clear();
        it = OvlImages.emplace((o.Cpu << 16) | o.Id, std::move(img)).first;
    }
    len = (u32)it->second.size();
    return len ? it->second.data() : nullptr;
}

void Recorder::NoteOverlayWrite(u32 addr, u16 flags)
{
    // writes to an overlay's own variables are normal; rewriting code, or RAM no overlay owns yet, may be a load
    u32 mask = WB.GetNDS() ? WB.GetNDS()->MainRAMMask : 0x3FFFFF;
    for (u32 c = 0; c < 2; c++)
        if (InOverlaySpan(c, addr) && !OvlDirty[c] && ((flags & (ByteFlag::Exec9 | ByteFlag::Exec7)) || OverlayAt(addr & mask) < 0))
            OvlDirty[c] = true;
}

void Recorder::VerifyOverlay(u32 cpu, u32 pc)
{
    OvlDirty[cpu] = false;
    melonDS::NDS* nds = WB.GetNDS();
    if (!nds || !nds->MainRAM) return;
    u32 mask = nds->MainRAMMask;
    auto same = [&](const OverlayInfo& o, u32 off, u32 n)
    {
        u32 isize = 0;
        const u8* img = OverlayImage(o, isize);
        if (!img || off >= isize) return false;
        n = std::min(n, isize - off);
        for (u32 i = 0; i < n; i++)
            if (nds->MainRAM[(o.RamStart + off + i) & mask] != img[off + i]) return false;
        return true;
    };
    // the code around the PC must equal the overlay's image (unpacked, for a compressed overlay); its
    // first bytes break ties
    std::vector<const OverlayInfo*> hits;
    for (const OverlayInfo& o : cpu ? WB.Rom.Overlays7 : WB.Rom.Overlays9)
    {
        if (pc < o.RamStart || pc >= o.RamStart + o.RamSize) continue;
        u32 off = pc - o.RamStart;
        u32 from = off >= 32 ? off - 32 : 0;
        if (same(o, from, 64)) hits.push_back(&o);
    }
    if (hits.size() > 1)
    {
        std::vector<const OverlayInfo*> strict;
        for (const OverlayInfo* o : hits)
            if (same(*o, 0, 32)) strict.push_back(o);
        hits = strict;
    }
    if (hits.size() != 1) return;
    int want = (int)((cpu << 15) | hits[0]->Id);
    if (OverlayAt(hits[0]->RamStart & mask) != want || OverlayAt(pc & mask) != want)
        MarkOverlayLoaded(cpu, hits[0]->Id, 2);
}

void Recorder::MarkOverlayLoaded(u32 cpu, u32 id, u8 kind)
{
    const OverlayInfo* ov = WB.Rom.Overlay(cpu, id);
    if (!ov || (ov->RamStart >> 24) != 0x02) return;
    u32 mask = WB.GetNDS() ? WB.GetNDS()->MainRAMMask : 0x3FFFFF;
    u32 start = ov->RamStart & mask;
    u32 end = std::min<u32>(start + ov->RamSize + ov->BssSize, mask + 1);
    u16 code = (u16)(((cpu << 15) | (id & 0x7FFF)) + 1);
    if (Owner[start >> 2] == code && Owner[(end - 1) >> 2] == code) return;

    // overlays this one overwrites, in whole or in part, are evicted
    std::vector<u16> evicted;
    for (u32 a = start & ~3u; a < end; a += 4)
    {
        u16 o = Owner[a >> 2];
        if (o && o != code && std::find(evicted.begin(), evicted.end(), o) == evicted.end()) evicted.push_back(o);
        Owner[a >> 2] = code;
    }

    // recompute page uniformity for the touched pages
    for (u32 p = start >> PageShift; p <= (end - 1) >> PageShift; p++)
    {
        u16 first = Owner[(p << PageShift) >> 2];
        u8 mixed = 0;
        for (u32 a = p << PageShift; a < ((p + 1) << PageShift); a += 4)
            if (Owner[a >> 2] != first) { mixed = 1; break; }
        PageMixed[p] = mixed;
    }
    WB.MapGen++;

    auto event = [&](const OverlayInfo& o, u32 ocpu, u8 okind)
    {
        OverlayEvent e {};
        e.frame = WB.Frame();
        e.cpu = (u8)ocpu;
        e.kind = okind;
        e.overlay_id = (u16)o.Id;
        e.ram_start = o.RamStart;
        e.ram_end = o.RamStart + o.RamSize + o.BssSize;
        e.timestamp = WB.Timestamp();
        OvlEvents.push_back(e);
    };
    for (u16 o : evicted)
        if (const OverlayInfo* old = WB.Rom.Overlay((o - 1) >> 15, (o - 1) & 0x7FFF))
            event(*old, (o - 1) >> 15, 1);
    event(*ov, cpu, kind);
    LogEvent(RecEventKind::OverlayLoad, (u16)id);

    if (WB.OnEvent)
    {
        WBEvent ev {};
        ev.Type = WBEvent::OverlayLoadedEvent;
        ev.Hit.Id = (int)id;
        ev.Hit.Cpu = cpu;
        WB.OnEvent(ev);
    }
}

RecordPage* Recorder::PageFor(u32 cpu, u32 addr, PageCache& cache, bool alloc)
{
    u32 vp = addr >> PageShift;
    if (cache.VPage == vp && cache.Gen == WB.MapGen) return cache.Page;

    PhysAddr pa = WB.Translate(cpu, addr);
    RecordPage* p = nullptr;
    if (pa.Recordable())
        p = alloc ? Records.GetOrAlloc(pa.Region, pa.Offset >> PageShift) : Records.Get(pa.Region, pa.Offset >> PageShift);

    bool cacheable = (alloc || p) && pa.Region != (u16)Region::VRAM;
    if (pa.Region == (u16)Region::MainRAM || IsOverlayRegion(pa.Region))
        if (PageMixed[(pa.Offset & 0xFFFFFF) >> PageShift]) cacheable = false;
    if (!pa.Recordable()) cacheable = true;   // remember "nothing here" too
    if (cacheable)
    {
        cache.VPage = vp;
        cache.Gen = WB.MapGen;
        cache.Page = p;
    }
    else
        cache.VPage = 0xFFFFFFFF;
    return p;
}

const ByteRecord* Recorder::Get(PhysAddr pa) const
{
    if (!pa.Recordable()) return nullptr;
    RecordPage* p = Records.Get(pa.Region, pa.Offset >> PageShift);
    return p ? &p->Rec[pa.Offset & (PageSize - 1)] : nullptr;
}

static inline void ClassExec(ByteRecord& r)
{
    r.mem_class = (u8)(r.read_count ? MemClass::CodeAndData : MemClass::Code);
}

static inline void ClassData(ByteRecord& r)
{
    if (r.exec_count) r.mem_class = (u8)MemClass::CodeAndData;
    else r.mem_class = (u8)MemClass::Data;
}

void Recorder::OnExec(u32 cpu, u32 pc, bool thumb)
{
    // finish a BIOS decompression when control returns to the caller
    if (Decomp[cpu].Active && pc == Decomp[cpu].ReturnAddr)
    {
        PendingDecompress& d = Decomp[cpu];
        d.Active = false;
        if (d.WriteMax >= d.WriteMin)
        {
            ProvenanceEvent ev {};
            ev.kind = (u8)ProvenanceKind::Decompress;
            ev.cpu = (u8)cpu;
            ev.channel = (u8)d.Swi;
            ev.frame = d.Frame;
            ev.pc = d.PC;
            ev.src_addr = d.ReadMax >= d.ReadMin ? d.ReadMin : d.Src;
            ev.src_size = d.ReadMax >= d.ReadMin ? d.ReadMax - d.ReadMin + 1 : 0;
            ev.dst_addr = d.WriteMin;
            ev.dst_size = d.WriteMax - d.WriteMin + 1;
            ev.rom_offset = 0xFFFFFFFF;
            ev.file_id = FileNone;
            ev.aux = d.Swi;
            ev.timestamp = WB.Timestamp();
            AddProvenance(ev);

            for (u32 a = ev.dst_addr; a < ev.dst_addr + ev.dst_size; a++)
            {
                PhysAddr pa = WB.Translate(cpu, a);
                if (!pa.Recordable()) continue;
                RecordPage* p = Records.GetOrAlloc(pa.Region, pa.Offset >> PageShift);
                if (p) p->Rec[pa.Offset & (PageSize-1)].flags |= ByteFlag::Decompressed;
            }
            if (WB.Taint.Enabled() && ev.src_size)
                WB.Taint.CopyRange(cpu, ev.src_addr, ev.src_size, ev.dst_addr, ev.dst_size);
        }
    }

    if (!On) return;
    if (OvlDirty[cpu] && InOverlaySpan(cpu, pc)) VerifyOverlay(cpu, pc);
    RecordPage* p = PageFor(cpu, pc, ExecCache[cpu], true);
    if (!p) return;
    u32 idx = pc & (PageSize - 1);
    ByteRecord* r = &p->Rec[idx];
    u16 fl = (cpu ? ByteFlag::Exec7 : ByteFlag::Exec9) | (thumb ? ByteFlag::Thumb : ByteFlag::Arm);
    r[0].flags |= ByteFlag::InstrStart;
    // once per instruction: which function was running, so code reached only through unexecuted
    // branches (inner loops of an unpacker) still gets a function
    if (!r[0].exec_count && !Stack[cpu].empty()) FirstFunc.emplace(((u64)cpu << 32) | pc, Stack[cpu].back().callee & ~1u);
    u32 n = thumb ? 2 : 4;
    // first frames: one test per instruction, and the bytes' only on their first run
    if (!r[0].exec_count || !r[n - 1].exec_count)
        for (u32 i = 0; i < n; i++)
            if (!r[i].exec_count) p->First[idx + i][Block_Exec] = WB.Frame() + 1;
    for (u32 i = 0; i < n; i++)
    {
        r[i].exec_count++;
        r[i].flags |= fl;
        ClassExec(r[i]);
    }
    p->BlockFrame[idx >> BlockShift][Block_Exec] = WB.Frame();
}

void Recorder::PushFrame(u32 cpu, const ShadowFrame& f)
{
    auto& st = Stack[cpu];
    if (st.size() >= 1024) st.erase(st.begin(), st.begin() + 256);
    st.push_back(f);
}

void Recorder::AddEdge(u32 cpu, u32 callsite, u32 callee, u32 callerfunc, CallKind kind, bool thumb)
{
    if (kind == CallKind::Call || kind == CallKind::TailCall)
    {
        KnownEntries[cpu & 1].insert(callee);
        NoteEntry(cpu, callee & ~1u, thumb || (callee & 1));
    }
    CallEdgeKey k {callsite, callee};
    auto it = CallEdges[cpu].find(k);
    u32 frame = WB.Frame();
    if (it == CallEdges[cpu].end())
    {
        CallEdge e {};
        e.cpu = (u8)cpu;
        e.kind = (u8)kind;
        e.thumb = thumb ? 1 : 0;
        e.call_site = callsite;
        e.callee = callee;
        e.caller_func = callerfunc;
        e.count = 1;
        e.first_frame = frame;
        e.last_frame = frame;
        CallEdges[cpu].emplace(k, e);
    }
    else
    {
        it->second.count++;
        it->second.last_frame = frame;
        if (!it->second.caller_func) it->second.caller_func = callerfunc;
    }
}

void Recorder::OnDiscontinuity(u32 cpu, ARM* arm, u32 pc)
{
    // control arrived somewhere other than the next instruction without an instruction doing it:
    // an interrupt, or an external change (reset, savestate, debugger)
    u32 expected = ExpectedPC[cpu];
    ExpectedPC[cpu] = 0xFFFFFFFF;
    if (expected == 0xFFFFFFFF) return;
    if (pc == arm->ExceptionBase + 0x18)
    {
        ShadowFrame f {};
        f.callee = pc;
        f.call_site = expected;
        f.return_addr = expected;
        f.sp = arm->R[13];
        f.kind = (u8)CallKind::Interrupt;
        f.thumb = 0;
        PushFrame(cpu, f);
        AddEdge(cpu, 0, pc, 0, CallKind::Interrupt, false);
    }
    else
    {
        Stack[cpu].clear();
    }
}

void Recorder::OnPostExec(u32 cpu, ARM* arm, u32 pc, bool thumb, u32 instr)
{
    u32 size = thumb ? 2 : 4;
    bool nowthumb = arm->CPSR & 0x20;
    u32 npc = arm->R[15] - (nowthumb ? 2 : 4);
    ExpectedPC[cpu] = npc;

    // BIOS decompression start
    int swi = SwiNumber(instr, thumb);
    if (swi >= 0)
    {
        bool decomp = (swi >= 0x10 && swi <= 0x18);
        if (decomp && !Decomp[cpu].Active)
        {
            PendingDecompress& d = Decomp[cpu];
            d = PendingDecompress();
            d.Active = true;
            d.Swi = (u32)swi;
            d.Src = arm->R[0];
            d.Dst = arm->R[1];
            d.ReturnAddr = pc + size;
            d.PC = pc;
            d.Frame = WB.Frame();
        }
    }

    if (!On) return;
    if (IsFunctionPrologue(instr, thumb)) NoteEntry(cpu, pc & ~1u, thumb);
    if (npc == pc + size) return;

    auto& st = Stack[cpu];
    for (int i = (int)st.size() - 1; i >= 0; i--)
    {
        if ((st[i].return_addr & ~1u) == npc)
        {
            st.resize(i);
            if (WB.Taint.Enabled()) WB.Taint.OnReturn(cpu);
            return;
        }
    }

    u32 lr = arm->R[14] & ~1u;
    u32 vecbase = arm->ExceptionBase;
    bool vector = (npc >= vecbase && npc < vecbase + 0x20) && ((arm->CPSR & 0x1F) != 0x10 && (arm->CPSR & 0x1F) != 0x1F);
    if (lr == pc + size || (vector && lr == ((pc + size) & ~1u)))
    {
        u32 callsite = pc;
        if (thumb && IsThumbBLSuffix((u16)instr)) callsite = pc - 2;
        CallKind kind = vector ? CallKind::Exception : CallKind::Call;
        u32 callerfunc = st.empty() ? 0 : st.back().callee;
        ShadowFrame f {};
        f.callee = npc;
        f.call_site = callsite;
        f.return_addr = arm->R[14];
        f.sp = arm->R[13];
        f.kind = (u8)kind;
        f.thumb = nowthumb ? 1 : 0;
        PushFrame(cpu, f);
        AddEdge(cpu, callsite, npc, callerfunc, kind, nowthumb);
        return;
    }

    // a veneer (first instruction of the callee is a branch) forwards the call. ARMv4T interworking
    // veneers take two steps: "bx pc" into ARM, then "b target"
    if (!st.empty() && st.back().callee == pc)
    {
        bool bx = thumb ? ((instr & 0xFF87) == 0x4700) : ((instr & 0x0FFFFFF0) == 0x012FFF10);
        bool ldrpc = thumb ? false : ((instr & 0x0E10F000) == 0x0410F000);
        bool bimm = thumb ? ((instr & 0xF800) == 0xE000) : ((instr & 0xFF000000) == 0xEA000000);
        if (bx || ldrpc || bimm)
        {
            ShadowFrame& f = st.back();
            u32 callerfunc = st.size() >= 2 ? st[st.size()-2].callee : 0;
            f.callee = npc;
            f.thumb = nowthumb ? 1 : 0;
            AddEdge(cpu, f.call_site, npc, callerfunc, CallKind::Call, nowthumb);
            return;
        }
    }
    // a tail call: an immediate B (conditional or not) to a known function entry hands this frame to
    // that function, which returns to our caller. Entries are known from observed calls and from the
    // knowledge store
    bool b = thumb ? ((instr & 0xF800) == 0xE000 || ((instr & 0xF000) == 0xD000 && ((instr >> 8) & 0xF) < 0xE))
                   : ((instr & 0x0F000000) == 0x0A000000 && (instr >> 28) != 0xF);
    if (b && !st.empty() && (st.back().callee & ~1u) != npc && (KnownEntries[cpu].count(npc) || KbEntries[cpu].count(npc)))
    {
        ShadowFrame& f = st.back();
        u32 callerfunc = f.callee;
        f.callee = npc;
        f.thumb = nowthumb ? 1 : 0;
        AddEdge(cpu, pc, npc, callerfunc, CallKind::TailCall, nowthumb);
    }
}

void Recorder::Touch(PhysAddr pa, u32 size, int kind)
{
    (void)pa; (void)size; (void)kind;
}

void Recorder::AddProvenance(const ProvenanceEvent& ev)
{
    if (ProvList.size() >= 4000000) return;
    ProvList.push_back(ev);
}

void Recorder::FlushTransfer(OpenTransfer& t)
{
    if (!t.Active) return;
    t.Active = false;
    AddProvenance(t.Ev);
}

void Recorder::FlushTransfers()
{
    for (auto& t : CardCopy) FlushTransfer(t);
    for (auto& t : DmaOpen) FlushTransfer(t);
    for (auto& t : CpuCopy) FlushCopy(t);
}

void Recorder::FlushCopy(OpenTransfer& t)
{
    if (!t.Active) return;
    t.Active = false;
    // short runs are register spills and struct assignments, not copies of loaded data
    if (t.Ev.dst_size >= 16) AddProvenance(t.Ev);
}

void Recorder::TrackCopy(u32 cpu, u32 src, u32 dst, u32 size)
{
    OpenTransfer& t = CpuCopy[cpu];
    if (t.Active && t.NextSrc == src && t.NextDst == dst)
    {
        t.Ev.src_size += size;
        t.Ev.dst_size += size;
        t.NextSrc += size;
        t.NextDst += size;
        return;
    }
    // copies that run backwards (memmove) extend the other way
    if (t.Active && t.Ev.src_addr == src + size && t.Ev.dst_addr == dst + size)
    {
        t.Ev.src_addr = src;
        t.Ev.dst_addr = dst;
        t.Ev.src_size += size;
        t.Ev.dst_size += size;
        return;
    }
    FlushCopy(t);
    t.Active = true;
    ProvenanceEvent& ev = t.Ev;
    ev = ProvenanceEvent {};
    ev.kind = (u8)ProvenanceKind::CpuCopy;
    ev.cpu = (u8)cpu;
    ev.frame = WB.Frame();
    ev.pc = WB.Cur[cpu];
    ev.src_addr = src;
    ev.src_size = size;
    ev.dst_addr = dst;
    ev.dst_size = size;
    ev.rom_offset = 0xFFFFFFFF;
    ev.file_id = FileNone;
    ev.timestamp = WB.Timestamp();
    t.NextSrc = src + size;
    t.NextDst = dst + size;
}

void Recorder::OnCardCommand(const u8* cmd)
{
    if (cmd[0] == 0xB7)
    {
        CardActive = true;
        CardAddr = ((u32)cmd[1] << 24) | ((u32)cmd[2] << 16) | ((u32)cmd[3] << 8) | cmd[4];
        CardWordIdx = 0;
    }
    else
        CardActive = false;
}

void Recorder::OnCardData(u32 value)
{
    // the word that will be consumed by the CPU or DMA read currently in progress
    u32 cpu = WB.GetNDS() ? (u32)WB.GetNDS()->CurCPU : 0;
    if (!CardActive)
    {
        LastCard[cpu].Valid = false;
        return;
    }
    LastCard[cpu].Valid = true;
    LastCard[cpu].RomOffset = CardAddr + CardWordIdx * 4;
    LastCard[cpu].Value = value;
    CardWordIdx++;
}

void Recorder::HandleCardWordStored(u32 cpu, u32 dst, u32 dstsize, PhysAddr pa, bool dma, u32 channel)
{
    CardWord& cw = LastCard[cpu & 1];
    u32 romoff = cw.RomOffset;
    cw.Valid = false;

    const RomFile* file = WB.Rom.FileForOffset(romoff);

    // an overlay file landing in its RAM range means that overlay is now loaded there
    if (file && file->OverlayId >= 0)
    {
        const OverlayInfo* ov = WB.Rom.Overlay((u32)file->OverlayCpu, (u32)file->OverlayId);
        if (ov && dst >= ov->RamStart && dst < ov->RamEnd() && pa.Region != 0 && (dst >> 24) == 0x02)
        {
            int cur = OverlayAt(pa.Offset);
            int want = (file->OverlayCpu << 15) | file->OverlayId;
            if (cur != want)
            {
                MarkOverlayLoaded((u32)file->OverlayCpu, (u32)file->OverlayId);
                pa = WB.Translate(cpu, dst);
            }
        }
    }

    if (On && pa.Recordable())
    {
        RecordPage* p = Records.GetOrAlloc(pa.Region, pa.Offset >> PageShift);
        if (p)
            for (u32 i = 0; i < dstsize; i++)
                p->Rec[(pa.Offset + i) & (PageSize - 1)].flags |= ByteFlag::CardLoaded;
    }

    // coalesce into one provenance event per contiguous transfer
    OpenTransfer& t = dma ? DmaOpen[channel & 7] : CardCopy[cpu & 1];
    if (t.Active && t.Ev.kind == (u8)ProvenanceKind::CardRead && t.NextSrc == romoff && t.NextDst == dst)
    {
        t.Ev.src_size += dstsize;
        t.Ev.dst_size += dstsize;
        t.NextSrc += dstsize;
        t.NextDst += dstsize;
        return;
    }
    FlushTransfer(t);
    t.Active = true;
    ProvenanceEvent& ev = t.Ev;
    ev = ProvenanceEvent {};
    ev.kind = (u8)ProvenanceKind::CardRead;
    ev.cpu = (u8)(dma ? ((cpu & 1) ? (u8)Cpu::DMA7 : (u8)Cpu::DMA9) : cpu);
    ev.channel = (u8)channel;
    ev.frame = WB.Frame();
    ev.pc = WB.Cur[cpu & 1];
    ev.src_addr = romoff;
    ev.src_size = dstsize;
    ev.dst_addr = dst;
    ev.dst_size = dstsize;
    ev.rom_offset = romoff;
    ev.file_id = file ? file->Id : FileNone;
    ev.timestamp = WB.Timestamp();
    t.NextSrc = romoff + dstsize;
    t.NextDst = dst + dstsize;
}

void Recorder::OnRead(u32 cpu, u32 addr, u32 size, u32 value)
{
    (void)value;
    if (addr == CardDataPort && size == 4)
    {
        // keep LastCard for the store that follows (CPU copy loop)
        return;
    }

    PendingDecompress& d = Decomp[cpu];
    if (d.Active)
    {
        if (addr >= d.Src && addr - d.Src < 0x400000)
        {
            d.ReadMin = std::min(d.ReadMin, addr);
            d.ReadMax = std::max(d.ReadMax, addr + size - 1);
        }
    }

    if (!On) return;
    RecordPage* p = PageFor(cpu, addr, ReadCache[cpu], true);
    if (!p) return;
    if (!LiteralLoad[cpu])
    {
        LoadRec& l = Loads[cpu][LoadPos[cpu]++ & 15];
        l.Addr = addr;
        l.Value = value;
        l.Size = (u8)size;
    }
    u32 idx = addr & (PageSize - 1);
    ByteRecord* r = &p->Rec[idx];
    u16 fl = cpu ? ByteFlag::Read7 : ByteFlag::Read9;
    if (LiteralLoad[cpu]) fl |= ByteFlag::LiteralPool;
    if (!r[0].read_count || !r[size - 1].read_count)
        for (u32 i = 0; i < size; i++)
            if (!r[i].read_count) p->First[idx + i][Block_Read] = WB.Frame() + 1;
    for (u32 i = 0; i < size; i++)
    {
        r[i].read_count++;
        r[i].flags |= fl;
        ClassData(r[i]);
    }
    p->BlockFrame[idx >> BlockShift][Block_Read] = WB.Frame();
    u32* rd = p->Readers[idx >> 2];
    u32 pc = WB.Cur[cpu] | cpu;   // bit 0 marks ARM7 (instruction addresses are even)
    if (rd[0] != pc)
    {
        rd[1] = rd[0];
        rd[0] = pc;
    }
}

void Recorder::OnWrite(u32 cpu, u32 addr, u32 size, u32 value, u32 oldvalue, PhysAddr pa)
{
    PendingDecompress& d = Decomp[cpu];
    if (d.Active)
    {
        if (addr >= d.Dst && addr - d.Dst < 0x400000)
        {
            d.WriteMin = std::min(d.WriteMin, addr);
            d.WriteMax = std::max(d.WriteMax, addr + size - 1);
        }
    }

    if (LastCard[cpu].Valid && size == 4 && value == LastCard[cpu].Value)
        HandleCardWordStored(cpu, addr, 4, pa, false, 0);

    if (!On) return;

    if (value && pa.Recordable())
    {
        u32 pos = LoadPos[cpu];
        for (u32 k = 1; k <= 16; k++)
        {
            const LoadRec& l = Loads[cpu][(pos - k) & 15];
            if (l.Size == size && l.Value == value)
            {
                TrackCopy(cpu, l.Addr, addr, size);
                break;
            }
        }
    }

    if (HistoryOn && pa.Recordable() && HistoryWanted(cpu & 1, addr, size, WB.Cur[cpu], WB.GetNDS() ? (cpu ? WB.GetNDS()->ARM7.R[13] : WB.GetNDS()->ARM9.R[13]) : 0, value == oldvalue))
    {
        WriteHistoryEntry e;
        e.timestamp = WB.Timestamp();
        e.frame = WB.Frame();
        e.pc = WB.Cur[cpu];
        e.addr = addr;
        e.old_value = oldvalue;
        e.new_value = value;
        e.cpu = (u8)cpu;
        e.size = (u8)size;
        e.region = pa.Region;
        PushHistory(e);
    }

    RecordPage* p = PageFor(cpu, addr, WriteCache[cpu], true);
    if (!p) return;
    u32 idx = addr & (PageSize - 1);
    ByteRecord* r = &p->Rec[idx];
    if (InOverlaySpan(0, addr) || InOverlaySpan(1, addr)) NoteOverlayWrite(addr, r[0].flags);
    u16 fl = cpu ? ByteFlag::Write7 : ByteFlag::Write9;
    u32 frame = WB.Frame();
    u32 pc = WB.Cur[cpu];
    if (!r[0].write_count || !r[size - 1].write_count)
        for (u32 i = 0; i < size; i++)
            if (!r[i].write_count) p->First[idx + i][Block_Write] = frame + 1;
    for (u32 i = 0; i < size; i++)
    {
        r[i].write_count++;
        r[i].flags = (r[i].flags & ~ByteFlag::LastWriteDMA) | fl;
        r[i].last_writer_pc = pc;
        r[i].last_write_frame = frame;
        r[i].last_writer_cpu = (u8)cpu;
        if (r[i].mem_class == (u8)MemClass::Untouched) r[i].mem_class = (u8)MemClass::Data;
    }
    p->BlockFrame[idx >> BlockShift][Block_Write] = frame;
}

void Recorder::OnDMA(u32 cpu, u32 channel, u32 src, u32 dst, u32 size, u32 value)
{
    u32 dcpu = cpu ? 3 : 2;   // DMA address space of that CPU
    PhysAddr dpa = WB.Translate(dcpu, dst);
    if (On && (InOverlaySpan(0, dst) || InOverlaySpan(1, dst)))
    {
        const ByteRecord* r = dpa.Recordable() ? Get(dpa) : nullptr;
        NoteOverlayWrite(dst, r ? r->flags : 0);
    }

    if (src == CardDataPort)
    {
        if (LastCard[cpu].Valid)
            HandleCardWordStored(cpu, dst, size, dpa, true, channel + cpu * 4);
        dpa = WB.Translate(dcpu, dst);
    }

    if (!On) return;

    PhysAddr spa = WB.Translate(dcpu, src);
    u32 frame = WB.Frame();

    if (spa.Recordable())
    {
        RecordPage* p = Records.GetOrAlloc(spa.Region, spa.Offset >> PageShift);
        if (p)
        {
            u32 idx = spa.Offset & (PageSize - 1);
            for (u32 i = 0; i < size; i++)
            {
                ByteRecord& r = p->Rec[idx + i];
                if (!r.dma_read_count && !r.dma_write_count) p->First[idx + i][Block_DMA] = frame + 1;
                r.dma_read_count++;
                r.flags |= ByteFlag::DmaRead;
                if (r.mem_class == (u8)MemClass::Untouched) r.mem_class = (u8)MemClass::Data;
            }
            p->BlockFrame[idx >> BlockShift][Block_DMA] = frame;
        }
    }

    if (dpa.Recordable())
    {
        u32 old = 0;
        if (HistoryOn) if (u8* ptr = WB.PhysPtr(dpa)) memcpy(&old, ptr, size);
        if (HistoryOn && HistoryWanted(cpu ? 3 : 2, dst, size, src, 0, old == value))
        {
            WriteHistoryEntry e;
            e.timestamp = WB.Timestamp();
            e.frame = frame;
            e.pc = src;
            e.addr = dst;
            e.old_value = old;
            e.new_value = value;
            e.cpu = (u8)(cpu ? Cpu::DMA7 : Cpu::DMA9);
            e.size = (u8)size;
            e.region = dpa.Region;
            PushHistory(e);
        }

        RecordPage* p = Records.GetOrAlloc(dpa.Region, dpa.Offset >> PageShift);
        if (p)
        {
            u32 idx = dpa.Offset & (PageSize - 1);
            u8 wcpu = (u8)((cpu ? (u8)Cpu::DMA7 : (u8)Cpu::DMA9) | (channel << 4));
            for (u32 i = 0; i < size; i++)
            {
                ByteRecord& r = p->Rec[idx + i];
                if (!r.dma_read_count && !r.dma_write_count) p->First[idx + i][Block_DMA] = frame + 1;
                r.dma_write_count++;
                r.flags |= ByteFlag::DmaWrite | ByteFlag::LastWriteDMA;
                r.last_writer_pc = src + i;
                r.last_write_frame = frame;
                r.last_writer_cpu = wcpu;
                if (r.mem_class == (u8)MemClass::Untouched) r.mem_class = (u8)MemClass::Data;
            }
            p->BlockFrame[idx >> BlockShift][Block_DMA] = frame;
        }
    }

    if (src == CardDataPort) return;   // already part of a CardRead event

    // coalesce plain DMA copies into provenance events
    OpenTransfer& t = DmaOpen[(channel + cpu * 4) & 7];
    if (t.Active && t.Ev.kind == (u8)ProvenanceKind::DMA && t.NextSrc == src && t.NextDst == dst)
    {
        t.Ev.src_size += size;
        t.Ev.dst_size += size;
        t.NextSrc = src + size;
        t.NextDst = dst + size;
        return;
    }
    if (t.Active && t.Ev.kind == (u8)ProvenanceKind::DMA && t.Ev.src_addr == src && t.NextDst == dst)
    {
        // fixed source (fill or FIFO)
        t.Ev.dst_size += size;
        t.NextDst = dst + size;
        return;
    }
    if (t.Active && t.Ev.kind == (u8)ProvenanceKind::DMA && t.NextSrc == src && t.Ev.dst_addr == dst)
    {
        // fixed destination (FIFO port such as GXFIFO)
        t.Ev.src_size += size;
        t.NextSrc = src + size;
        return;
    }
    FlushTransfer(t);
    t.Active = true;
    ProvenanceEvent& ev = t.Ev;
    ev = ProvenanceEvent {};
    ev.kind = (u8)ProvenanceKind::DMA;
    ev.cpu = (u8)(cpu ? Cpu::DMA7 : Cpu::DMA9);
    ev.channel = (u8)channel;
    ev.frame = frame;
    ev.pc = WB.Cur[cpu];
    ev.src_addr = src;
    ev.src_size = size;
    ev.dst_addr = dst;
    ev.dst_size = size;
    ev.rom_offset = 0xFFFFFFFF;
    ev.file_id = FileNone;
    ev.timestamp = WB.Timestamp();
    t.NextSrc = src + size;
    t.NextDst = dst + size;
}

void Recorder::OnTcmChange(u32 itcmsize, u32 dtcmbase, u32 dtcmsize)
{
    if (!On) return;
    if (!TcmList.empty())
    {
        const TcmEvent& l = TcmList.back();
        if (l.itcm_size == itcmsize && l.dtcm_base == dtcmbase && l.dtcm_size == dtcmsize) return;
    }
    TcmEvent e {};
    e.frame = WB.Frame();
    e.itcm_size = itcmsize;
    e.dtcm_base = dtcmbase;
    e.dtcm_size = dtcmsize;
    e.timestamp = WB.Timestamp();
    TcmList.push_back(e);
}

void Recorder::OnBootLoad()
{
    if (!On || !WB.Rom.Valid()) return;
    auto add = [&](u32 cpu, u32 fileid, u32 romoff, u32 ram, u32 size)
    {
        if (!size) return;
        ProvenanceEvent ev {};
        ev.kind = (u8)ProvenanceKind::BootLoad;
        ev.cpu = (u8)Cpu::Boot;
        ev.channel = (u8)cpu;
        ev.frame = WB.Frame();
        ev.src_addr = romoff;
        ev.src_size = size;
        ev.dst_addr = ram;
        ev.dst_size = size;
        ev.rom_offset = romoff;
        ev.file_id = fileid;
        ev.timestamp = WB.Timestamp();
        AddProvenance(ev);
    };
    add(0, FileARM9, WB.Rom.ARM9RomOffset, WB.Rom.ARM9RamAddress, WB.Rom.ARM9Size);
    add(1, FileARM7, WB.Rom.ARM7RomOffset, WB.Rom.ARM7RamAddress, WB.Rom.ARM7Size);
}

void Recorder::OnFrameEnd()
{
    if (!On) return;
    EndFrame = WB.Frame();
    for (auto& t : CpuCopy) FlushCopy(t);
}

// ---------------------------------------------------------------- Workbench

std::string RegionLabel(u16 region)
{
    if (region >= (u16)Region::OverlayARM9Base && region < (u16)Region::OverlayARM7Base)
        return "ov9_" + std::to_string(region - (u16)Region::OverlayARM9Base);
    if (region >= (u16)Region::OverlayARM7Base && region < (u16)Region::RegionMax)
        return "ov7_" + std::to_string(region - (u16)Region::OverlayARM7Base);
    return RegionName(region);
}

u32 RegionSize(u16 region)
{
    switch ((Region)region)
    {
    case Region::MainRAM: return 0x1000000;
    case Region::SharedWRAM: return 0x8000;
    case Region::ARM7WRAM: return 0x10000;
    case Region::ITCM: return 0x8000;
    case Region::DTCM: return 0x4000;
    case Region::VRAM: return 0xA4000;
    case Region::Palette: return 0x800;
    case Region::OAM: return 0x800;
    case Region::BIOS9: return 0x10000;
    case Region::BIOS7: return 0x10000;
    default:
        if (IsOverlayRegion(region)) return 0x1000000;
        return 0;
    }
}

static const u32 VRAMBankBase[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};

Workbench::Workbench() : Rec(*this), Taint(*this), Dbg(*this)
{
}

Workbench::~Workbench()
{
    if (NDS) NDS->WB = nullptr;
}

void Workbench::Attach(melonDS::NDS* nds)
{
    if (NDS == nds) return;
    if (NDS)
    {
        NDS->WB = nullptr;
        NDS->WBMemHooks = false;
        NDS->WBExecHooks = false;
    }
    NDS = nds;
    if (NDS) NDS->WB = this;
    MapGen++;
    UpdateHookFlags();
}

void Workbench::OnRomLoaded(const u8* rom, u32 romlen, u32 filelen)
{
    Rec.Reset();
    Taint.Reset();
    Rom.Load(rom, romlen, filelen);
    Rec.ComputeOverlaySpans();
    std::fill(Rec.Owner.begin(), Rec.Owner.end(), 0);
    std::fill(Rec.PageMixed.begin(), Rec.PageMixed.end(), 0);
    MapGen++;
}

void Workbench::OnReset()
{
    std::fill(Rec.Owner.begin(), Rec.Owner.end(), 0);
    std::fill(Rec.PageMixed.begin(), Rec.PageMixed.end(), 0);
    for (int i = 0; i < 2; i++)
    {
        Rec.Stack[i].clear();
        Rec.ExpectedPC[i] = 0xFFFFFFFF;
        Rec.Decomp[i] = PendingDecompress();
        Rec.LastCard[i] = CardWord();
    }
    Rec.CardActive = false;
    if (Rec.Enabled()) Rec.LogEvent(RecEventKind::Reset);
    MapGen++;
}

void Workbench::OnDirectBoot()
{
    Rec.OnBootLoad();
    if (Taint.Enabled()) Taint.SeedStaticModules();
}

void Workbench::OnStateLoaded()
{
    for (int i = 0; i < 2; i++)
    {
        Rec.Stack[i].clear();
        Rec.ExpectedPC[i] = 0xFFFFFFFF;
        Rec.Decomp[i] = PendingDecompress();
    }
    if (Rec.Enabled()) Rec.LogEvent(RecEventKind::StateLoaded);
    Rec.InvalidateOverlays();      // the loaded RAM may hold any overlay
    MapGen++;
}

bool Workbench::JITActive() const
{
    return NDS && NDS->IsJITEnabled();
}

void Workbench::UpdateHookFlags()
{
    if (!NDS) return;
    bool jit = NDS->IsJITEnabled();
    bool mem = !jit && (Rec.Enabled() || Taint.Enabled() || Dbg.HasWatchpoints());
    bool exec = !jit && (mem || Dbg.ExecHooksNeeded());
    NDS->WBMemHooks = mem;
    NDS->WBExecHooks = exec;
}

u32 Workbench::Frame() const { return NDS ? NDS->NumFrames : 0; }

u64 Workbench::Timestamp() const
{
    if (!NDS) return 0;
    return NDS->ARM9Timestamp >> NDS->ARM9ClockShift;
}

PhysAddr Workbench::Translate(u32 cpu, u32 addr) const
{
    PhysAddr pa;
    if (!NDS) return pa;
    melonDS::NDS& nds = *NDS;

    if (cpu == 0 || cpu == 2)
    {
        if (cpu == 0)
        {
            if (addr < nds.ARM9.ITCMSize) { pa.Region = (u16)Region::ITCM; pa.Offset = addr & (ITCMPhysicalSize - 1); return pa; }
            if ((addr & nds.ARM9.DTCMMask) == nds.ARM9.DTCMBase) { pa.Region = (u16)Region::DTCM; pa.Offset = addr & (DTCMPhysicalSize - 1); return pa; }
        }
        if ((addr & 0xFFFF0000) == 0xFFFF0000) { pa.Region = (u16)Region::BIOS9; pa.Offset = addr & 0xFFFF; return pa; }
        switch (addr >> 24)
        {
        case 0x02:
            {
                u32 off = addr & nds.MainRAMMask;
                u16 o = Rec.Owner[off >> 2];
                if (o)
                {
                    u32 code = o - 1;
                    u32 id = code & 0x7FFF;
                    if (id < 256) { pa.Region = (u16)(((code >> 15) ? (u16)Region::OverlayARM7Base : (u16)Region::OverlayARM9Base) + id); pa.Offset = off; return pa; }
                }
                pa.Region = (u16)Region::MainRAM; pa.Offset = off; return pa;
            }
        case 0x03:
            if (nds.SWRAM_ARM9.Mem)
            {
                pa.Region = (u16)Region::SharedWRAM;
                pa.Offset = (u32)(nds.SWRAM_ARM9.Mem - nds.SharedWRAM) + (addr & nds.SWRAM_ARM9.Mask);
            }
            return pa;
        case 0x04: pa.Region = (u16)Region::IO9; pa.Offset = addr & 0xFFFFFF; return pa;
        case 0x05: pa.Region = (u16)Region::Palette; pa.Offset = addr & 0x7FF; return pa;
        case 0x06:
            {
                u32 mask = 0;
                u32 bankoff = 0;
                GPU& gpu = nds.GPU;
                switch (addr & 0x00E00000)
                {
                case 0x00000000: mask = gpu.VRAMMap_ABG[(addr >> 14) & 0x1F]; break;
                case 0x00200000: mask = gpu.VRAMMap_BBG[(addr >> 14) & 0x7]; break;
                case 0x00400000: mask = gpu.VRAMMap_AOBJ[(addr >> 14) & 0xF]; break;
                case 0x00600000: mask = gpu.VRAMMap_BOBJ[(addr >> 14) & 0x7]; break;
                default:
                    {
                        u32 a = addr & 0xFFFFFF;
                        int bank = -1;
                        if (a >= 0x800000 && a < 0x8A4000)
                        {
                            u32 r = a - 0x800000;
                            for (int b = 8; b >= 0; b--)
                                if (r >= VRAMBankBase[b]) { bank = b; break; }
                        }
                        if (bank >= 0 && (gpu.VRAMMap_LCDC & (1 << bank)))
                        {
                            pa.Region = (u16)Region::VRAM;
                            pa.Offset = VRAMBankBase[bank] + ((a - 0x800000 - VRAMBankBase[bank]) & gpu.VRAMMask[bank]);
                        }
                        return pa;
                    }
                }
                if (!mask) return pa;
                int bank = __builtin_ctz(mask);
                (void)bankoff;
                pa.Region = (u16)Region::VRAM;
                pa.Offset = VRAMBankBase[bank] + (addr & gpu.VRAMMask[bank]);
                return pa;
            }
        case 0x07: pa.Region = (u16)Region::OAM; pa.Offset = addr & 0x7FF; return pa;
        case 0x08: case 0x09: case 0x0A: pa.Region = (u16)Region::GBASlot; pa.Offset = addr & 0xFFFFFF; return pa;
        }
        return pa;
    }

    // ARM7 / DMA7
    if (addr < 0x4000) { pa.Region = (u16)Region::BIOS7; pa.Offset = addr; return pa; }
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        {
            u32 off = addr & nds.MainRAMMask;
            u16 o = Rec.Owner[off >> 2];
            if (o)
            {
                u32 code = o - 1;
                u32 id = code & 0x7FFF;
                if (id < 256) { pa.Region = (u16)(((code >> 15) ? (u16)Region::OverlayARM7Base : (u16)Region::OverlayARM9Base) + id); pa.Offset = off; return pa; }
            }
            pa.Region = (u16)Region::MainRAM; pa.Offset = off; return pa;
        }
    case 0x03000000:
        if (nds.SWRAM_ARM7.Mem)
        {
            pa.Region = (u16)Region::SharedWRAM;
            pa.Offset = (u32)(nds.SWRAM_ARM7.Mem - nds.SharedWRAM) + (addr & nds.SWRAM_ARM7.Mask);
        }
        else
        {
            pa.Region = (u16)Region::ARM7WRAM;
            pa.Offset = addr & (nds.ARM7WRAMSize - 1);
        }
        return pa;
    case 0x03800000:
        pa.Region = (u16)Region::ARM7WRAM;
        pa.Offset = addr & (nds.ARM7WRAMSize - 1);
        return pa;
    case 0x04000000: pa.Region = (u16)Region::IO7; pa.Offset = addr & 0xFFFFFF; return pa;
    case 0x04800000: pa.Region = (u16)Region::WifiRAM; pa.Offset = addr & 0xFFFF; return pa;
    case 0x06000000:
    case 0x06800000:
        {
            u32 mask = nds.GPU.VRAMMap_ARM7[(addr >> 17) & 0x1];
            if (!mask) return pa;
            int bank = (mask & (1 << 2)) ? 2 : 3;
            pa.Region = (u16)Region::VRAM;
            pa.Offset = VRAMBankBase[bank] + (addr & 0x1FFFF);
            return pa;
        }
    case 0x08000000: case 0x08800000: case 0x09000000: case 0x09800000: case 0x0A000000: case 0x0A800000:
        pa.Region = (u16)Region::GBASlot; pa.Offset = addr & 0xFFFFFF; return pa;
    }
    return pa;
}

u8* Workbench::PhysPtr(PhysAddr pa) const
{
    if (!NDS) return nullptr;
    melonDS::NDS& nds = *NDS;
    if (IsOverlayRegion(pa.Region)) return nds.MainRAM + (pa.Offset & nds.MainRAMMask);
    switch ((Region)pa.Region)
    {
    case Region::MainRAM: return nds.MainRAM + (pa.Offset & nds.MainRAMMask);
    case Region::SharedWRAM: return nds.SharedWRAM + (pa.Offset & 0x7FFF);
    case Region::ARM7WRAM: return nds.ARM7WRAM + (pa.Offset & 0xFFFF);
    case Region::ITCM: return nds.ARM9.ITCM + (pa.Offset & (ITCMPhysicalSize - 1));
    case Region::DTCM: return nds.ARM9.DTCM + (pa.Offset & (DTCMPhysicalSize - 1));
    case Region::Palette: return nds.GPU.Palette + (pa.Offset & 0x7FF);
    case Region::OAM: return nds.GPU.OAM + (pa.Offset & 0x7FF);
    case Region::VRAM:
        for (int b = 8; b >= 0; b--)
            if (pa.Offset >= VRAMBankBase[b])
                return nds.GPU.VRAM[b] + ((pa.Offset - VRAMBankBase[b]) & nds.GPU.VRAMMask[b]);
        return nullptr;
    case Region::BIOS9: return const_cast<u8*>(nds.GetARM9BIOS().data()) + (pa.Offset & (ARM9BIOSSize - 1));
    case Region::BIOS7: return const_cast<u8*>(nds.GetARM7BIOS().data()) + (pa.Offset & (ARM7BIOSSize - 1));
    default: return nullptr;
    }
}

bool Workbench::Peek(u32 cpu, u32 addr, u32 size, u32& out) const
{
    out = 0;
    PhysAddr pa = Translate(cpu, addr);
    u8* p = PhysPtr(pa);
    if (!p) return false;
    // stay inside the mapping for multi-byte reads
    PhysAddr pb = Translate(cpu, addr + size - 1);
    if (pb.Region != pa.Region)
    {
        for (u32 i = 0; i < size; i++)
        {
            u32 b;
            if (!Peek(cpu, addr + i, 1, b)) return false;
            out |= b << (i * 8);
        }
        return true;
    }
    memcpy(&out, p, size);
    return true;
}

bool Workbench::Poke(u32 cpu, u32 addr, u32 size, u32 val)
{
    PhysAddr pa = Translate(cpu, addr);
    u8* p = PhysPtr(pa);
    if (!p || pa.Region == (u16)Region::BIOS9 || pa.Region == (u16)Region::BIOS7) return false;
    memcpy(p, &val, size);
    return true;
}

u32 Workbench::CPSR(u32 cpu) const
{
    if (!NDS) return 0;
    return cpu ? NDS->ARM7.CPSR : NDS->ARM9.CPSR;
}

u32 Workbench::CurrentPC(u32 cpu) const
{
    if (!NDS) return 0;
    const ARM& a = cpu ? (const ARM&)NDS->ARM7 : (const ARM&)NDS->ARM9;
    return a.R[15] - ((a.CPSR & 0x20) ? 2 : 4);
}

u32 Workbench::ReadReg(u32 cpu, u32 reg) const
{
    if (!NDS) return 0;
    const ARM& a = cpu ? (const ARM&)NDS->ARM7 : (const ARM&)NDS->ARM9;
    if (reg == 15) return a.R[15] - ((a.CPSR & 0x20) ? 2 : 4);
    if (reg == 16) return a.CPSR;
    return a.R[reg & 15];
}

bool Workbench::PCInRecordedThumb(u32 cpu, u32 addr) const
{
    const ByteRecord* r = Rec.Get(Translate(cpu, addr));
    return r && (r->flags & ByteFlag::Thumb) && !(r->flags & ByteFlag::Arm);
}

static bool ConditionPasses(u32 cpsr, u32 cond)
{
    bool n = cpsr & (1u << 31), z = cpsr & (1u << 30), c = cpsr & (1u << 29), v = cpsr & (1u << 28);
    switch (cond & 0xF)
    {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    default: return true;   // AL, and the unconditional space (BLX imm, PLD)
    }
}

bool Workbench::PreExec(melonDS::ARM* arm, u32 pc, u32 instr, bool thumb)
{
    u32 cpu = arm->Num;
    if (thumb) instr &= 0xFFFF;

    bool irqEntry = false;
    if (pc != Rec.ExpectedPC[cpu] && Rec.ExpectedPC[cpu] != 0xFFFFFFFF)
    {
        if (Rec.Enabled()) Rec.OnDiscontinuity(cpu, arm, pc);
        else Rec.ExpectedPC[cpu] = 0xFFFFFFFF;
        irqEntry = pc == arm->ExceptionBase + 0x18;
    }

    Cur[cpu] = pc;
    CurThumb[cpu] = thumb;
    CurInstr[cpu] = instr;

    TrackMode(cpu, arm->CPSR & 0x1F, irqEntry);

    if (Dbg.CheckExec(cpu, pc))
        Dbg.EnterBreak(cpu, pc);

    Rec.LiteralLoad[cpu] = LoadsLiteralPool(instr, thumb);
    Rec.OnExec(cpu, pc, thumb);

    if (Taint.Enabled())
    {
        bool cond = thumb || ConditionPasses(arm->CPSR, instr >> 28);
        Taint.PreExec(cpu, instr, thumb, cond, arm->R);
    }
    return false;
}

void Workbench::TrackMode(u32 cpu, u32 mode, bool irqEntry)
{
    if (mode != PrevMode[cpu])
    {
        if (Taint.Enabled()) Taint.OnModeChange(cpu, PrevMode[cpu], mode);
        PrevMode[cpu] = mode;
    }
    // after the bank switch: the IRQ mode's lr holds the return address, the interrupted mode's keeps its label
    if (Taint.Enabled() && irqEntry) Taint.SetRegLabel(cpu, 14, 0);
}

void Workbench::PostExec(melonDS::ARM* arm)
{
    u32 cpu = arm->Num;
    if (Taint.Enabled()) Taint.PostExec(cpu);
    Rec.OnPostExec(cpu, arm, Cur[cpu], CurThumb[cpu], CurInstr[cpu]);
    if (!Rec.Enabled())
    {
        // still track the next PC for decompression and interrupt detection
    }
}

void Workbench::CpuRead(u32 cpu, u32 addr, u32 size, u32 value)
{
    if (size == 1) value &= 0xFF;
    else if (size == 2) value &= 0xFFFF;
    Rec.OnRead(cpu, addr, size, value);
    if (Taint.Enabled())
    {
        u32 fixed = 0xFFFFFFFF;
        if (addr == 0x04100010 && Rec.LastCard[cpu].Valid)
        {
            const RomFile* f = Rom.FileForOffset(Rec.LastCard[cpu].RomOffset);
            u32 src = f ? Taint.SourceForRomFile(f->Id, Rec.LastCard[cpu].RomOffset) : 0;
            fixed = src ? Taint.LabelForSource(src) : 0;
        }
        Taint.OnRead(cpu, addr, size, fixed, value);
    }
    if (Dbg.WatchPages && Dbg.WatchPages[addr >> PageShift])
        Dbg.CheckAccess(cpu, addr, size, value, false);
}

void Workbench::CpuWrite(u32 cpu, u32 addr, u32 size, u32 value)
{
    if (size == 1) { value &= 0xFF; }
    else if (size == 2) { value &= 0xFFFF; addr &= ~1u; }
    else addr &= ~3u;
    PhysAddr pa = Translate(cpu, addr);
    u32 old = 0;
    if (u8* p = PhysPtr(pa)) memcpy(&old, p, size);
    Rec.OnWrite(cpu, addr, size, value, old, pa);
    if (Taint.Enabled())
    {
        Taint.OnWrite(cpu, addr, size);
        Taint.TrackWriteRun(cpu, addr, size);
    }
    if (Dbg.WatchPages && Dbg.WatchPages[addr >> PageShift])
        Dbg.CheckAccess(cpu, addr, size, value, true);
}

void Workbench::DMAUnit(u32 cpu, u32 channel, u32 src, u32 dst, u32 size, u32 value)
{
    if (Taint.Enabled())
    {
        u32 fixed = 0xFFFFFFFF;
        if (src == 0x04100010 && Rec.LastCard[cpu].Valid)
        {
            const RomFile* f = Rom.FileForOffset(Rec.LastCard[cpu].RomOffset);
            u32 s = f ? Taint.SourceForRomFile(f->Id, Rec.LastCard[cpu].RomOffset) : 0;
            fixed = s ? Taint.LabelForSource(s) : 0;
        }
        Taint.OnDMA(cpu, src, dst, size, fixed);
    }
    Rec.OnDMA(cpu, channel, src, dst, size, value);
    if (Dbg.WatchPages && Dbg.WatchPages[dst >> PageShift])
        Dbg.CheckAccess(cpu ? 3 : 2, dst, size, value, true);
}

void Workbench::CardCommand(const u8* cmd)
{
    Rec.OnCardCommand(cmd);
}

void Workbench::CardData(u32 value)
{
    Rec.OnCardData(value);
}

void Workbench::TcmChanged()
{
    MapGen++;
    if (!NDS) return;
    u32 dtcmsize = NDS->ARM9.DTCMMask ? (~NDS->ARM9.DTCMMask + 1) : 0;
    Rec.OnTcmChange(NDS->ARM9.ITCMSize, NDS->ARM9.DTCMBase, dtcmsize);
}

void Workbench::FrameEnd()
{
    Rec.OnFrameEnd();
    Dbg.FrameEnd();
    Dbg.ServiceJobs();
}

}
