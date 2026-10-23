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

// Session files: SessionHeader, then SessionSectionHeader + payload per section.
// All layouts come from the schema (WBRecords.h / dswb Python bindings).

#include <cstring>
#include <ctime>
#include "Session.h"
#include "version.h"

namespace melonDS::WB
{

namespace
{
struct Writer
{
    std::vector<u8>& Out;
    size_t SectionStart = 0;
    u32 Sections = 0;

    explicit Writer(std::vector<u8>& o) : Out(o) {}
    void Raw(const void* p, size_t n) { Out.insert(Out.end(), (const u8*)p, (const u8*)p + n); }
    void Begin(SessionSection id, u32 count)
    {
        SessionSectionHeader h {};
        h.id = (u32)id;
        h.count = count;
        h.size = 0;
        SectionStart = Out.size();
        Raw(&h, sizeof(h));
    }
    void End()
    {
        u64 size = Out.size() - SectionStart - sizeof(SessionSectionHeader);
        memcpy(&Out[SectionStart + offsetof(SessionSectionHeader, size)], &size, 8);
        Sections++;
    }
};
}

void Recorder::SerializeSession(std::vector<u8>& out, u32 flags) const
{
    out.clear();
    SessionHeader hdr {};
    memcpy(hdr.magic, "DSWBSESS", 8);
    hdr.version = SchemaVersion;
    hdr.header_size = sizeof(SessionHeader);
    memcpy(hdr.rom_sha1, WB.Rom.Sha1, 20);
    hdr.frame_start = StartFrame;
    hdr.frame_end = On ? WB.Frame() : EndFrame;
    hdr.start_time = 0;
    hdr.end_time = (u64)time(nullptr);
    hdr.flags = flags;
    strncpy(hdr.melonds_version, MELONDS_VERSION, sizeof(hdr.melonds_version) - 1);
    Writer w(out);
    w.Raw(&hdr, sizeof(hdr));

    // byte records
    w.Begin(SessionSection::ByteRecords, Records.PageCount());
    Records.ForEach([&](u16 region, u32 page, const RecordPage* p)
    {
        PageHeader ph {};
        ph.region = region;
        ph.page = page;
        w.Raw(&ph, sizeof(ph));
        w.Raw(p->Rec, sizeof(p->Rec));
    });
    w.End();

    w.Begin(SessionSection::BlockFrames, Records.PageCount());
    Records.ForEach([&](u16 region, u32 page, const RecordPage* p)
    {
        PageHeader ph {};
        ph.region = region;
        ph.page = page;
        w.Raw(&ph, sizeof(ph));
        w.Raw(p->BlockFrame, sizeof(p->BlockFrame));
    });
    w.End();

    // per byte: 1 + the first frame of exec, read, write and DMA (0 = never)
    w.Begin(SessionSection::FirstFrames, Records.PageCount());
    Records.ForEach([&](u16 region, u32 page, const RecordPage* p)
    {
        PageHeader ph {};
        ph.region = region;
        ph.page = page;
        w.Raw(&ph, sizeof(ph));
        w.Raw(p->First, sizeof(p->First));
    });
    w.End();

    w.Begin(SessionSection::Readers, Records.PageCount());
    Records.ForEach([&](u16 region, u32 page, const RecordPage* p)
    {
        PageHeader ph {};
        ph.region = region;
        ph.page = page;
        w.Raw(&ph, sizeof(ph));
        w.Raw(p->Readers, sizeof(p->Readers));
    });
    w.End();

    // the newest entries only: a history file can hold far more than a session file should
    u64 keep = std::min<u64>(HistoryUsed, SessionHistoryMax);
    u64 skip = HistoryUsed - keep, i = 0;
    w.Begin(SessionSection::WriteHistory, (u32)keep);
    ForEachHistory([&](const WriteHistoryEntry& e) { if (i++ >= skip) w.Raw(&e, sizeof(e)); });
    w.End();

    w.Begin(SessionSection::FirstRun, (u32)FirstFunc.size());
    for (const auto& kv : FirstFunc)
    {
        FirstRunEntry e {};
        e.pc = (u32)kv.first;
        e.func = kv.second;
        e.cpu = (u8)(kv.first >> 32);
        w.Raw(&e, sizeof(e));
    }
    w.End();

    u32 nedges = (u32)(CallEdges[0].size() + CallEdges[1].size());
    w.Begin(SessionSection::CallEdges, nedges);
    for (int c = 0; c < 2; c++)
        for (const auto& kv : CallEdges[c]) w.Raw(&kv.second, sizeof(CallEdge));
    w.End();

    // open transfers are flushed by the caller before saving
    w.Begin(SessionSection::Provenance, (u32)ProvList.size());
    if (!ProvList.empty()) w.Raw(ProvList.data(), ProvList.size() * sizeof(ProvenanceEvent));
    w.End();

    w.Begin(SessionSection::OverlayEvents, (u32)OvlEvents.size());
    if (!OvlEvents.empty()) w.Raw(OvlEvents.data(), OvlEvents.size() * sizeof(OverlayEvent));
    w.End();

    w.Begin(SessionSection::TcmEvents, (u32)TcmList.size());
    if (!TcmList.empty()) w.Raw(TcmList.data(), TcmList.size() * sizeof(TcmEvent));
    w.End();

    w.Begin(SessionSection::RecEvents, (u32)EventLog.size());
    if (!EventLog.empty()) w.Raw(EventLog.data(), EventLog.size() * sizeof(RecEvent));
    w.End();

    std::vector<u8> labels, sources;
    WB.Taint.Serialize(labels, sources);
    w.Begin(SessionSection::Sources, (u32)WB.Taint.Sources().size());
    w.Raw(sources.data(), sources.size());
    w.End();
    w.Begin(SessionSection::Labels, WB.Taint.LabelCount());
    w.Raw(labels.data(), labels.size());
    w.End();

    std::vector<u8> merges;
    WB.Taint.SerializeMerges(merges);
    w.Begin(SessionSection::LabelMerges, (u32)(merges.size() / sizeof(LabelMergeEntry)));
    w.Raw(merges.data(), merges.size());
    w.End();

    w.Begin(SessionSection::LabelPages, WB.Taint.MemLabels().PageCount());
    WB.Taint.MemLabels().ForEach([&](u16 region, u32 page, const LabelPage* p)
    {
        PageHeader ph {};
        ph.region = region;
        ph.page = page;
        w.Raw(&ph, sizeof(ph));
        w.Raw(p->L, sizeof(p->L));
    });
    w.End();

    SessionHeader* h = (SessionHeader*)out.data();
    h->section_count = w.Sections;
}

bool ReadSession(const u8* data, size_t len, SessionHeader& hdr, std::vector<SessionSectionView>& sections)
{
    sections.clear();
    if (len < sizeof(SessionHeader)) return false;
    memcpy(&hdr, data, sizeof(hdr));
    if (memcmp(hdr.magic, "DSWBSESS", 8) != 0) return false;
    if (hdr.version > SchemaVersion) return false;
    size_t pos = hdr.header_size ? hdr.header_size : sizeof(SessionHeader);
    for (u32 i = 0; i < hdr.section_count; i++)
    {
        if (pos + sizeof(SessionSectionHeader) > len) return false;
        SessionSectionView v;
        memcpy(&v.Header, data + pos, sizeof(SessionSectionHeader));
        pos += sizeof(SessionSectionHeader);
        if (pos + v.Header.size > len) return false;
        v.Data = data + pos;
        pos += v.Header.size;
        sections.push_back(v);
    }
    return true;
}

bool SessionHistory(const u8* data, size_t len, HistoryView& out)
{
    SessionHeader hdr;
    std::vector<SessionSectionView> sections;
    if (!ReadSession(data, len, hdr, sections)) return false;
    for (const SessionSectionView& s : sections)
        if ((SessionSection)s.Header.id == SessionSection::WriteHistory)
        {
            u64 n = std::min<u64>(s.Header.count, s.Header.size / sizeof(WriteHistoryEntry));
            out = {(const WriteHistoryEntry*)s.Data, n, n, n};
            return true;
        }
    out = {};
    return true;
}

static const char HistoryFileMagic[8] = {'D', 'S', 'W', 'B', 'H', '1', 0, 0};

HistoryFileTrailer MakeHistoryTrailer(const HistoryView& v, u32 frameStart, u32 frameEnd)
{
    HistoryFileTrailer t {};
    memcpy(t.Magic, HistoryFileMagic, 8);
    t.Pos = v.Pos;
    t.Used = v.Used;
    t.Slots = v.Slots;
    t.FrameStart = frameStart;
    t.FrameEnd = frameEnd;
    t.EntrySize = sizeof(WriteHistoryEntry);
    return t;
}

bool ReadHistoryFile(const u8* data, size_t len, HistoryView& out, HistoryFileTrailer* trailer)
{
    HistoryFileTrailer t;
    if (len < sizeof(t)) return false;
    memcpy(&t, data + len - sizeof(t), sizeof(t));
    if (memcmp(t.Magic, HistoryFileMagic, 8) != 0 || t.EntrySize != sizeof(WriteHistoryEntry)) return false;
    if (t.Used > t.Slots || t.Used > t.Pos || t.Slots > (len - sizeof(t)) / sizeof(WriteHistoryEntry)) return false;
    out = {(const WriteHistoryEntry*)data, t.Slots, t.Pos, t.Used};
    if (trailer) *trailer = t;
    return true;
}

bool MergeSession(const u8* data, size_t len, MergedSessions& into)
{
    SessionHeader hdr;
    std::vector<SessionSectionView> sections;
    if (!ReadSession(data, len, hdr, sections)) return false;

    for (const SessionSectionView& s : sections)
    {
        switch ((SessionSection)s.Header.id)
        {
        case SessionSection::ByteRecords:
            {
                const size_t stride = sizeof(PageHeader) + sizeof(ByteRecord) * PageSize;
                for (u32 i = 0; i < s.Header.count && (i + 1) * stride <= s.Header.size; i++)
                {
                    PageHeader ph;
                    memcpy(&ph, s.Data + i * stride, sizeof(ph));
                    RecordPage* dst = into.Records.GetOrAlloc(ph.region, ph.page);
                    if (!dst) continue;
                    const ByteRecord* src = (const ByteRecord*)(s.Data + i * stride + sizeof(PageHeader));
                    for (u32 b = 0; b < PageSize; b++)
                    {
                        ByteRecord r;
                        memcpy(&r, &src[b], sizeof(r));
                        ByteRecord& d = dst->Rec[b];
                        d.exec_count += r.exec_count;
                        d.read_count += r.read_count;
                        d.write_count += r.write_count;
                        d.dma_read_count += r.dma_read_count;
                        d.dma_write_count += r.dma_write_count;
                        if (r.write_count || r.dma_write_count)
                        {
                            d.last_writer_pc = r.last_writer_pc;
                            d.last_write_frame = r.last_write_frame;
                            d.last_writer_cpu = r.last_writer_cpu;
                        }
                        d.flags |= r.flags;
                        if (r.mem_class > d.mem_class || (d.mem_class == (u8)MemClass::Code && r.mem_class == (u8)MemClass::Data))
                            d.mem_class = (d.exec_count && (d.read_count)) ? (u8)MemClass::CodeAndData : r.mem_class;
                        if (d.exec_count && d.mem_class == (u8)MemClass::Data) d.mem_class = (u8)MemClass::CodeAndData;
                    }
                }
            }
            break;
        case SessionSection::CallEdges:
            for (u32 i = 0; i < s.Header.count && (i + 1) * sizeof(CallEdge) <= s.Header.size; i++)
            {
                CallEdge e;
                memcpy(&e, s.Data + i * sizeof(CallEdge), sizeof(e));
                auto& m = into.Edges[e.cpu & 1];
                CallEdgeKey k {e.call_site, e.callee};
                auto it = m.find(k);
                if (it == m.end()) m.emplace(k, e);
                else
                {
                    it->second.count += e.count;
                    if (!it->second.caller_func) it->second.caller_func = e.caller_func;
                }
            }
            break;
        case SessionSection::FirstRun:
            for (u32 i = 0; i < s.Header.count && (i + 1) * sizeof(FirstRunEntry) <= s.Header.size; i++)
            {
                FirstRunEntry e;
                memcpy(&e, s.Data + i * sizeof(e), sizeof(e));
                into.FirstFunc.emplace(((u64)(e.cpu & 1) << 32) | e.pc, e.func);   // the earliest session wins
            }
            break;
        case SessionSection::Provenance:
            for (u32 i = 0; i < s.Header.count && (i + 1) * sizeof(ProvenanceEvent) <= s.Header.size; i++)
            {
                ProvenanceEvent e;
                memcpy(&e, s.Data + i * sizeof(e), sizeof(e));
                into.Provenance.push_back(e);
            }
            break;
        default:
            break;
        }
    }
    into.SessionCount++;
    into.FramesRecorded += hdr.frame_end >= hdr.frame_start ? hdr.frame_end - hdr.frame_start : 0;
    return true;
}

}
