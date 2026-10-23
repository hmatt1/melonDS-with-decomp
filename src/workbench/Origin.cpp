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

// Data origin tracking: labels follow values from the sources the user chose
// (ROM files by default) through registers, memory, DMA and BIOS decompression.

#include <algorithm>
#include <cstring>
#include "Workbench.h"
#include "../NDS.h"
#include "../ARM.h"

namespace melonDS::WB
{

static constexpr u32 MaxLabels = 1u << 20;

static const std::vector<u32> EmptySources;

void Origin::Reset()
{
    On = false;
    SourceList.clear();
    NextSourceId = 1;
    RomFileSource.clear();
    Pending.clear();
    for (auto& c : Runs) for (WriteRun& r : c) r = WriteRun();
    SourceLabel.clear();
    Labels.clear();
    Labels.push_back({});
    Made.assign(1, LabelMerge());
    LabelIndex.clear();
    UnionCache.clear();
    Mem.Clear();
    memset(Reg, 0, sizeof(Reg));
    memset(AddrReg, 0, sizeof(AddrReg));
    memset(Pointer, 0, sizeof(Pointer));
    memset(RegFIQ, 0, sizeof(RegFIQ));
    memset(RegIRQ, 0, sizeof(RegIRQ));
    memset(RegSVC, 0, sizeof(RegSVC));
    memset(RegABT, 0, sizeof(RegABT));
    memset(RegUND, 0, sizeof(RegUND));
    memset(AddrFIQ, 0, sizeof(AddrFIQ));
    memset(AddrIRQ, 0, sizeof(AddrIRQ));
    memset(AddrSVC, 0, sizeof(AddrSVC));
    memset(AddrABT, 0, sizeof(AddrABT));
    memset(AddrUND, 0, sizeof(AddrUND));
    memset(PointerBank, 0, sizeof(PointerBank));
    NonZero[0] = NonZero[1] = 0;
    OpsActive[0] = OpsActive[1] = false;

    DataSource c;
    c.Kind = SourceKind::CodeConstant;
    c.Name = "code constant";
    u32 cid = AddSource(c);
    ConstLabel = LabelForSource(cid);
}

void Origin::Enable(bool on)
{
    if (Labels.empty()) Reset();
    if (on == On) return;
    On = on;
    if (on)
    {
        WB.PrevMode[0] = WB.CPSR(0) & 0x1F;
        WB.PrevMode[1] = WB.CPSR(1) & 0x1F;
        SeedStaticModules();
    }
    WB.Rec.LogEvent(on ? RecEventKind::OriginOn : RecEventKind::OriginOff);
    WB.UpdateHookFlags();
}

u32 Origin::AddSource(DataSource src)
{
    if (Labels.empty() && src.Kind != SourceKind::CodeConstant) Reset();
    src.Id = NextSourceId++;
    if (src.Name.empty())
    {
        char buf[64];
        switch (src.Kind)
        {
        case SourceKind::Range: snprintf(buf, sizeof(buf), "range %08X+%X", src.Addr, src.Size); break;
        case SourceKind::Register: snprintf(buf, sizeof(buf), "%s r%u @%08X", src.Cpu ? "arm7" : "arm9", src.Reg, src.Addr); break;
        case SourceKind::Keypad: snprintf(buf, sizeof(buf), "keypad"); break;
        case SourceKind::Touch: snprintf(buf, sizeof(buf), "touch"); break;
        case SourceKind::RTC: snprintf(buf, sizeof(buf), "rtc"); break;
        default: snprintf(buf, sizeof(buf), "source %u", src.Id); break;
        }
        src.Name = buf;
    }
    SourceList.push_back(src);
    const DataSource& s = SourceList.back();
    if (s.Kind == SourceKind::RomFile && s.FileId != FileNone) RomFileSource[RomKey(s.FileId, s.Addr)] = s.Id;

    if (s.Kind != SourceKind::CodeConstant)
        WB.Rec.LogEvent(RecEventKind::SourceAdded, (u16)s.Id);

    // sources that label existing state right away
    if (s.Kind == SourceKind::Range && s.Size)
        SetRangeLabel(s.Cpu, s.Addr, s.Size, LabelForSource(s.Id), true);
    else if (s.Kind == SourceKind::Register)
        SetRegLabel(s.Cpu, s.Reg & 15, UnionAt(Reg[s.Cpu & 1][s.Reg & 15], LabelForSource(s.Id), s.Cpu, MergeSource));
    return s.Id;
}

bool Origin::RemoveSource(u32 id)
{
    for (DataSource& s : SourceList)
    {
        if (s.Id != id || s.Kind == SourceKind::CodeConstant) continue;
        s.Enabled = false;
        if (s.Kind == SourceKind::RomFile) RomFileSource.erase(RomKey(s.FileId, s.Addr));
        WB.Rec.LogEvent(RecEventKind::SourceRemoved, (u16)id);
        return true;
    }
    return false;
}

const DataSource* Origin::FindSource(u32 id) const
{
    for (const DataSource& s : SourceList)
        if (s.Id == id) return &s;
    return nullptr;
}

void Origin::SetAllRomFiles(bool on)
{
    AllRomFilesOn = on;
}

u32 Origin::SourceForRomFile(u32 fileid, u32 romoff)
{
    u32 member = (romoff != 0xFFFFFFFF && WB.SubFile) ? WB.SubFile(fileid, romoff) : 0;
    u32 src = SourceFor(fileid, member);
    bool members = !member && WB.FilePackedMembers && WB.FilePackedMembers(fileid);
    if (src && WB.SubFileResolve && (member || members || (WB.FileIndexed && WB.FileIndexed(fileid))))
    {
        // its unpacked copy will be labelled once it appears (CheckSeeds)
        bool known = false;
        for (PendingFile& p : Pending)
            if (p.FileId == fileid && p.Member == member) { p.Frame = WB.Frame(); p.Source = src; p.Members = members; known = true; break; }
        if (!known)
        {
            if (Pending.size() >= 64) Pending.erase(Pending.begin());
            Pending.push_back({fileid, member, src, WB.Frame(), members});
        }
    }
    return src;
}

void Origin::NoteWriteRun(u32 cpu, u32 addr, u32 size)
{
    WriteRun* runs = Runs[cpu & 1];
    for (u32 i = 0; i < 8; i++)
    {
        WriteRun& r = runs[i];
        if (r.End == addr && r.End != r.Start) { r.End += size; r.Grown = true; return; }
        if (addr == r.Start)
        {
            // the buffer is filled again from its start (the next chunk of a streamed file)
            r.End = addr + size;
            r.Checked = addr;
            r.Grown = true;
            return;
        }
        if (addr >= r.Start && addr + size <= r.End) { r.Checked = std::min(r.Checked, addr); r.Grown = true; return; }
    }
    WriteRun& r = runs[RunNext[cpu & 1]++ & 7];
    r.Start = r.Checked = addr;
    r.End = addr + size;
    r.Grown = true;
}

void Origin::CheckSeeds(u32 cpu)
{
    melonDS::NDS* nds = WB.GetNDS();
    if (!nds || !nds->MainRAM || !WB.SubFileResolve) return;
    u32 mask = nds->MainRAMMask;
    u32 now = WB.Frame();
    // search only the indexes that hold a pending file
    u32 which = 0;
    for (const PendingFile& p : Pending) which |= (p.Member ? 1 : 2) | (p.Members ? 1 : 0);
    for (WriteRun& r : Runs[cpu & 1])
    {
        if (!r.Grown) continue;
        u32 from = std::max(r.Checked, r.Start);
        if (r.End - from < 64) continue;          // wait for a useful amount of new output
        r.Grown = false;
        r.Checked = r.End;
        SeedRuns.clear();
        WB.SubFileResolve(nds->MainRAM, 0x02000000, mask + 1, 0x02000000 + (from & mask), r.End - from, which, SeedRuns);
        for (const SubFileRun& m : SeedRuns)
        {
            // only files whose bytes were just read: equal bytes elsewhere are a coincidence
            for (u32 j = 0; j < m.Count; j++)
            {
                auto it = std::find_if(Pending.begin(), Pending.end(), [&](const PendingFile& p) { return p.FileId == m.FileId && p.Member == m.Members[j]; });
                if (it == Pending.end()) continue;
                SetRangeLabel(cpu, m.Addr, m.Size, LabelForSource(it->Source), false);
                Seeded++;
                break;
            }
        }
        // a file packed whole that holds archived files, or an archived file that is an archive itself:
        // the files inside, once unpacked, are labelled as themselves over the label of what held them
        for (const SubFileRun& m : SeedRuns)
        {
            if (!m.Count || !m.Members[0]) continue;
            auto it = std::find_if(Pending.begin(), Pending.end(), [&](const PendingFile& p)
            {
                if (p.FileId != m.FileId || p.Member == m.Members[0]) return false;
                return p.Member ? WB.SubFileInside && WB.SubFileInside(p.FileId, p.Member, m.Members[0]) : p.Members;
            });
            if (it == Pending.end()) continue;
            if (u32 src = SourceFor(m.FileId, m.Members[0]))
            {
                SetRangeLabel(cpu, m.Addr, m.Size, LabelForSource(src), false);
                Seeded++;
            }
        }
    }
    // files read but never unpacked where we could see them are dropped after a while
    Pending.erase(std::remove_if(Pending.begin(), Pending.end(), [now](const PendingFile& p) { return now - p.Frame > 600; }), Pending.end());
}

u32 Origin::SourceFor(u32 fileid, u32 member)
{
    auto enabled = [&](u32 m) -> u32
    {
        auto it = RomFileSource.find(RomKey(fileid, m));
        if (it == RomFileSource.end()) return 0;
        const DataSource* s = FindSource(it->second);
        return s && s->Enabled ? s->Id : 0;
    };
    if (member)
        if (u32 id = enabled(member)) return id;
    // the whole file chosen as a source covers the files archived in it
    u32 whole = enabled(0);
    if (whole && (!member || !AllRomFilesOn)) return whole;
    if (!AllRomFilesOn) return 0;
    DataSource s;
    s.Kind = SourceKind::RomFile;
    s.FileId = fileid;
    s.Addr = member;
    const RomFile* f = WB.Rom.FileById(fileid);
    s.Name = f ? f->Path : ("file " + std::to_string(fileid));
    if (member && WB.SubFileName) s.Name += ":" + WB.SubFileName(fileid, member);
    return AddSource(s);
}

u32 Origin::LabelForSource(u32 srcid)
{
    if (Labels.empty()) Reset();
    auto it = SourceLabel.find(srcid);
    if (it != SourceLabel.end()) return it->second;
    std::vector<u32> set {srcid};
    auto li = LabelIndex.find({set, 0});
    u32 label;
    if (li != LabelIndex.end()) label = li->second;
    else
    {
        label = (u32)Labels.size();
        Labels.push_back(set);
        Made.push_back(LabelMerge());
        LabelIndex.emplace(std::make_pair(set, (u64)0), label);
    }
    SourceLabel[srcid] = label;
    return label;
}

u32 Origin::Union(u32 a, u32 b)
{
    if (a == b || b == 0) return a;
    if (a == 0) return b;
    if (a == ConstLabel) return b;
    if (b == ConstLabel) return a;
    if (a > b) std::swap(a, b);
    u32 pc = WB.Cur[OpCpu & 1];
    u64 site = MergeSite(OpCpu, Op, pc);
    UnionKey key {a, b, site};
    auto it = UnionCache.find(key);
    if (it != UnionCache.end()) return it->second;

    const auto& sa = Labels[a];
    const auto& sb = Labels[b];
    std::vector<u32> merged;
    merged.reserve(sa.size() + sb.size());
    std::set_union(sa.begin(), sa.end(), sb.begin(), sb.end(), std::back_inserter(merged));
    u32 label;
    // nothing new joined: the label (and its history) stays
    if (merged.size() == sa.size()) { UnionCache.emplace(key, a); return a; }
    if (merged.size() == sb.size()) { UnionCache.emplace(key, b); return b; }
    auto li = LabelIndex.find({merged, site});
    if (li != LabelIndex.end()) label = li->second;
    else if (Labels.size() >= MaxLabels) label = b;   // table full: keep the newer label
    else
    {
        label = (u32)Labels.size();
        Labels.push_back(merged);
        LabelMerge m;
        m.A = a;
        m.B = b;
        m.PC = pc;
        m.Frame = WB.Frame();
        m.Cpu = OpCpu;
        m.Op = Op;
        Made.push_back(m);
        LabelIndex.emplace(std::make_pair(std::move(merged), site), label);
    }
    UnionCache.emplace(key, label);
    return label;
}

const Origin::LabelMerge& Origin::MergeOf(u32 label) const
{
    static const LabelMerge none;
    return label < Made.size() ? Made[label] : none;
}

const char* Origin::MergeOpName(u8 op)
{
    switch (op)
    {
    case MergeAlu: return "computed";           // an instruction combined its operands
    case MergeAddress: return "table_lookup";   // a load took its address's label (following table lookups)
    case MergeBytes: return "mixed_bytes";      // a load spanned bytes with different labels
    case MergeMath: return "math_unit";         // the divider or square root unit
    case MergeCopy: return "copy";              // a copied range joined its bytes' labels (BIOS unpacking)
    case MergeSource: return "source_added";    // a source added on top of a labelled value
    default: return "";
    }
}

void Origin::SerializeMerges(std::vector<u8>& out) const
{
    out.clear();
    for (u32 l = 1; l < Made.size(); l++)
    {
        const LabelMerge& m = Made[l];
        if (!m.A && !m.B) continue;
        LabelMergeEntry e {};
        e.label = l;
        e.a = m.A;
        e.b = m.B;
        e.pc = m.PC;
        e.frame = m.Frame;
        e.cpu = m.Cpu;
        e.op = m.Op;
        out.insert(out.end(), (const u8*)&e, (const u8*)&e + sizeof(e));
    }
}

const std::vector<u32>& Origin::LabelSources(u32 label) const
{
    if (label >= Labels.size()) return EmptySources;
    return Labels[label];
}

u32 Origin::GetMemLabel(PhysAddr pa) const
{
    if (!pa.Recordable()) return 0;
    LabelPage* p = Mem.Get(pa.Region, pa.Offset >> PageShift);
    return p ? p->L[pa.Offset & (PageSize - 1)] : 0;
}

void Origin::SetMemLabel(PhysAddr pa, u32 label)
{
    if (!pa.Recordable()) return;
    LabelPage* p = label ? Mem.GetOrAlloc(pa.Region, pa.Offset >> PageShift) : Mem.Get(pa.Region, pa.Offset >> PageShift);
    if (p) p->L[pa.Offset & (PageSize - 1)] = label;
}

void Origin::SetRangeLabel(u32 cpu, u32 addr, u32 size, u32 label, bool merge)
{
    for (u32 i = 0; i < size; i++)
    {
        PhysAddr pa = WB.Translate(cpu, addr + i);
        if (!pa.Recordable()) continue;
        u32 l = merge ? UnionAt(GetMemLabel(pa), label, cpu, MergeSource) : label;
        SetMemLabel(pa, l);
    }
}

void Origin::UpdateNonZero(u32 cpu)
{
    u16 m = 0;
    for (int i = 0; i < 16; i++) if (Reg[cpu][i]) m |= 1 << i;
    NonZero[cpu] = m;
}

u32 Origin::AccessLabel(u32 cpu, u32 addr, u32 size) const
{
    PhysAddr pa = WB.Translate(cpu, addr);
    if (!pa.Recordable()) return 0;
    LabelPage* p = Mem.Get(pa.Region, pa.Offset >> PageShift);
    if (!p) return 0;
    u32 idx = pa.Offset & (PageSize - 1);
    u32 l = p->L[idx];
    for (u32 i = 1; i < size && idx + i < PageSize; i++)
    {
        u32 m = p->L[idx + i];
        if (m != l) l = const_cast<Origin*>(this)->UnionAt(l, m, cpu, MergeBytes);
    }
    return l;
}

// values that address memory (main RAM, TCM, WRAM, IO, VRAM): pointers rather than data
static bool PointerLike(u32 v)
{
    u32 hi = v >> 24;
    return hi == 0x02 || hi == 0x03 || hi == 0x04 || hi == 0x06 || (v >= 0x01FF8000 && v < 0x02000000);
}

void Origin::PreExec(u32 cpu, u32 instr, bool thumb, bool condpass, const u32* regs)
{
    OpsActive[cpu] = false;
    if (!condpass) return;

    InstrOps& o = Ops[cpu];
    DecodeOps(instr, thumb, o);
    LoadIdx[cpu] = StoreIdx[cpu] = 0;

    switch (o.kind)
    {
    case InstrOps::Alu:
    case InstrOps::Branch:
        if (!o.dstMask) return;
        if (!(o.srcMask & NonZero[cpu]) && !(o.dstMask & NonZero[cpu])) return; // nothing labeled involved
        {
            u16 m = o.srcMask & NonZero[cpu];
            u16 lm = m, am = m, isPtr = 0;
            if (FollowAddr && regs)
            {
                // pointer arithmetic (base + index, pointer += length) carries only the data operand's
                // label, even an unlabelled one: as an address for any address-like operand, and in the
                // ordinary label for operands known to be pointers
                u16 data = 0, ptrs = 0, known = 0;
                for (int r = 0; r < 15; r++)
                    if (o.srcMask & (1 << r))
                    {
                        if (PointerLike(regs[r]))
                        {
                            ptrs |= (u16)(1 << r);
                            if (Pointer[cpu] & (1 << r)) known |= (u16)(1 << r);
                        }
                        else data |= (u16)(1 << r);
                    }
                if (data && ptrs) am = (u16)(m & data);
                isPtr = known ? 1 : 0;
            }
            // a known pointer contributes its label as a pointer, everything else its full label
            u32 l = 0, al = 0;
            for (u16 k = lm, r = 0; k; r++, k >>= 1)
                if (k & 1) l = UnionAt(l, (FollowAddr && (Pointer[cpu] & (1 << r))) ? AddrReg[cpu][r] : Reg[cpu][r], cpu, MergeAlu);
            if (FollowAddr)
            {
                for (u16 k = am, r = 0; k; r++, k >>= 1)
                    if (k & 1) al = UnionAt(al, AddrReg[cpu][r], cpu, MergeAlu);
            }
            else al = l;
            PendingLabel[cpu] = l;
            PendingAddrLabel[cpu] = al;
            PendingPointer[cpu] = isPtr;
        }
        OpsActive[cpu] = true;
        return;
    case InstrOps::Load:
    case InstrOps::Store:
    case InstrOps::Swap:
        AddrLabel[cpu] = 0;
        Pointer[cpu] |= o.addrMask;             // these registers address memory
        if (FollowAddr && (o.addrMask & NonZero[cpu]))
        {
            u16 m = o.addrMask & NonZero[cpu];
            for (int r = 0; m; r++, m >>= 1)
                if (m & 1) AddrLabel[cpu] = UnionAt(AddrLabel[cpu], AddrReg[cpu][r], cpu, MergeAddress);
        }
        for (u32 i = 0; i < o.nStore; i++)
        {
            u32 r = o.storeRegs[i];
            // a stored pointer keeps its label as a pointer: what it points to is not its history
            StoreLabels[cpu][i] = r < 15 ? ((FollowAddr && (Pointer[cpu] & (1 << r))) ? AddrReg[cpu][r] : Reg[cpu][r]) : 0;
        }
        OpsActive[cpu] = true;
        return;
    default:
        return;
    }
}

void Origin::PostExec(u32 cpu)
{
    if (!OpsActive[cpu]) return;
    InstrOps& o = Ops[cpu];
    if (o.kind == InstrOps::Alu || o.kind == InstrOps::Branch)
    {
        u16 m = o.dstMask;
        for (int r = 0; m; r++, m >>= 1)
            if (m & 1) { Reg[cpu][r] = PendingLabel[cpu]; AddrReg[cpu][r] = PendingAddrLabel[cpu]; }
        if (PendingPointer[cpu]) Pointer[cpu] |= o.dstMask;
        else Pointer[cpu] &= (u16)~o.dstMask;
        UpdateNonZero(cpu);
    }
    OpsActive[cpu] = false;
}

void Origin::OnRead(u32 cpu, u32 addr, u32 size, u32 fixedlabel, u32 value)
{
    if (!OpsActive[cpu]) return;
    InstrOps& o = Ops[cpu];
    if (LoadIdx[cpu] >= o.nLoad) return;
    u8 r = o.loadRegs[LoadIdx[cpu]++];
    u32 label;
    if (fixedlabel != 0xFFFFFFFF) label = fixedlabel;
    else if (o.pcRelLoad) label = ConstLabel;
    else
    {
        label = AccessLabel(cpu, addr, size);
        if (!label && cpu == 0 && (addr >> 8) == 0x040002) label = MathResultLabel(addr);
        if (!label && (addr >> 24) == 0x04)
        {
            // optional hardware input sources
            SourceKind want = SourceKind::CodeConstant;
            if (addr == 0x04000130 || addr == 0x04000136) want = SourceKind::Keypad;
            else if (cpu == 1 && addr == 0x040001C2) want = SourceKind::Touch;
            else if (cpu == 1 && addr == 0x04000138) want = SourceKind::RTC;
            if (want != SourceKind::CodeConstant)
                for (const DataSource& s : SourceList)
                    if (s.Kind == want && s.Enabled) { label = LabelForSource(s.Id); break; }
        }
    }
    if (AddrLabel[cpu]) label = UnionAt(label, AddrLabel[cpu], cpu, MergeAddress);
    if (r < 15)
    {
        Reg[cpu][r] = label;
        // a loaded pointer addresses data; its own label is not the data's
        AddrReg[cpu][r] = (FollowAddr && size == 4 && PointerLike(value)) ? 0 : label;
        Pointer[cpu] &= (u16)~(1 << r);         // a new value
        if (label) NonZero[cpu] |= 1 << r;
        else NonZero[cpu] &= ~(1 << r);
    }
}

u32 Origin::MathResultLabel(u32 addr)
{
    u32 o = addr & 0xFF;
    if (o >= 0xA0 && o < 0xB0)          // DIV_RESULT, DIVREM_RESULT: numerator and denominator
        return UnionAt(UnionAt(MathLabel[0], MathLabel[1], 0, MergeMath), UnionAt(MathLabel[2], MathLabel[3], 0, MergeMath), 0, MergeMath);
    if (o >= 0xB4 && o < 0xB8)          // SQRT_RESULT
        return UnionAt(MathLabel[4], MathLabel[5], 0, MergeMath);
    return 0;
}

void Origin::OnWrite(u32 cpu, u32 addr, u32 size)
{
    u32 label = 0;
    if (OpsActive[cpu])
    {
        InstrOps& o = Ops[cpu];
        if (StoreIdx[cpu] < o.nStore) label = StoreLabels[cpu][StoreIdx[cpu]++];
    }
    if (cpu == 0 && (addr >> 8) == 0x040002)
    {
        // operands of the math unit (word writes; 64-bit operands are written as two words)
        u32 o = addr & 0xFF;
        if (o >= 0x90 && o < 0xA0) MathLabel[(o - 0x90) >> 2] = label;
        else if (o >= 0xB8 && o < 0xC0) MathLabel[4 + ((o - 0xB8) >> 2)] = label;
        return;
    }
    PhysAddr pa = WB.Translate(cpu, addr);
    if (!pa.Recordable()) return;
    LabelPage* p = label ? Mem.GetOrAlloc(pa.Region, pa.Offset >> PageShift) : Mem.Get(pa.Region, pa.Offset >> PageShift);
    if (!p) return;
    u32 idx = pa.Offset & (PageSize - 1);
    for (u32 i = 0; i < size && idx + i < PageSize; i++) p->L[idx + i] = label;
}

void Origin::OnDMA(u32 cpu, u32 src, u32 dst, u32 size, u32 fixedlabel)
{
    u32 dcpu = cpu ? 3 : 2;
    PhysAddr dpa = WB.Translate(dcpu, dst);
    if (!dpa.Recordable()) return;
    u32 labels[4] = {0, 0, 0, 0};
    bool any = false;
    if (fixedlabel != 0xFFFFFFFF)
    {
        for (u32 i = 0; i < size; i++) labels[i] = fixedlabel;
        any = fixedlabel != 0;
    }
    else
    {
        PhysAddr spa = WB.Translate(dcpu, src);
        if (spa.Recordable())
            if (LabelPage* sp = Mem.Get(spa.Region, spa.Offset >> PageShift))
            {
                u32 idx = spa.Offset & (PageSize - 1);
                for (u32 i = 0; i < size && idx + i < PageSize; i++) { labels[i] = sp->L[idx + i]; any |= labels[i] != 0; }
            }
    }
    LabelPage* dp = any ? Mem.GetOrAlloc(dpa.Region, dpa.Offset >> PageShift) : Mem.Get(dpa.Region, dpa.Offset >> PageShift);
    if (!dp) return;
    u32 idx = dpa.Offset & (PageSize - 1);
    for (u32 i = 0; i < size && idx + i < PageSize; i++) dp->L[idx + i] = labels[i];
}

void Origin::CopyRange(u32 cpu, u32 src, u32 srcsize, u32 dst, u32 dstsize)
{
    u32 l = 0;
    for (u32 i = 0; i < srcsize; i++)
    {
        u32 m = GetMemLabel(WB.Translate(cpu, src + i));
        if (m && m != l) l = UnionAt(l, m, cpu, MergeCopy);
    }
    if (!l) return;
    SetRangeLabel(cpu, dst, dstsize, l, false);
}

void Origin::OnModeChange(u32 cpu, u32 oldmode, u32 newmode)
{
    u32* R = Reg[cpu];
    u32* A = AddrReg[cpu];
    // a register's label, address label and pointer flag are banked together
    auto swapPointers = [&](u16& saved, u16 mask)
    {
        u16 cur = Pointer[cpu] & mask;
        Pointer[cpu] = (u16)((Pointer[cpu] & ~mask) | (saved & mask));
        saved = cur;
    };
    auto spLr = [&](u32 (&regs)[2][2], u32 (&addrs)[2][2], int b)
    {
        std::swap(R[13], regs[cpu][0]); std::swap(R[14], regs[cpu][1]);
        std::swap(A[13], addrs[cpu][0]); std::swap(A[14], addrs[cpu][1]);
        swapPointers(PointerBank[cpu][b], 0x6000);
    };
    auto bank = [&](u32 mode)
    {
        switch (mode & 0x1F)
        {
        case 0x11:
            for (int i = 0; i < 7; i++) { std::swap(R[8+i], RegFIQ[cpu][i]); std::swap(A[8+i], AddrFIQ[cpu][i]); }
            swapPointers(PointerBank[cpu][0], 0x7F00);
            break;
        case 0x12: spLr(RegIRQ, AddrIRQ, 1); break;
        case 0x13: spLr(RegSVC, AddrSVC, 2); break;
        case 0x17: spLr(RegABT, AddrABT, 3); break;
        case 0x1B: spLr(RegUND, AddrUND, 4); break;
        }
    };
    bank(oldmode);
    bank(newmode);
    UpdateNonZero(cpu);
}

void Origin::SeedStaticModules(bool byContent)
{
    melonDS::NDS* nds = WB.GetNDS();
    if (!nds || !WB.Rom.Valid() || !AllRomFilesOn) return;
    const u8* rom = WB.Rom.Data();

    // runs of RAM still equal to img are labelled with the file. A few bytes that happen to be equal (a
    // zero where the image had one, in memory reused after boot) are no evidence: runs are 16 bytes or more
    auto seed = [&](u32 cpu, u32 fileid, const u8* img, u32 ramaddr, u32 size)
    {
        if (!size || !img) return;
        u32 src = SourceForRomFile(fileid);
        if (!src) return;
        u32 label = LabelForSource(src);
        u32 i = 0;
        while (i < size)
        {
            u32 s = i;
            for (; i < size; i++)
            {
                u8* p = WB.PhysPtr(WB.Translate(cpu, ramaddr + i));
                if (!p || *p != img[i]) break;
            }
            if (i - s >= 16)
                for (u32 k = s; k < i; k++)
                {
                    PhysAddr pa = WB.Translate(cpu, ramaddr + k);
                    if (!GetMemLabel(pa)) SetMemLabel(pa, label);
                }
            if (i == s) i++;
        }
    };
    auto romImage = [&](u32 off, u32 size) { return off + size <= WB.Rom.Length() ? rom + off : nullptr; };

    seed(0, FileARM9, romImage(WB.Rom.ARM9RomOffset, WB.Rom.ARM9Size), WB.Rom.ARM9RamAddress, WB.Rom.ARM9Size);
    seed(1, FileARM7, romImage(WB.Rom.ARM7RomOffset, WB.Rom.ARM7Size), WB.Rom.ARM7RamAddress, WB.Rom.ARM7Size);
    for (int cpu = 0; cpu < 2; cpu++)
    {
        const std::vector<OverlayInfo>& ovs = cpu ? WB.Rom.Overlays7 : WB.Rom.Overlays9;
        for (const OverlayInfo& o : ovs)
        {
            if ((o.RamStart >> 24) != 0x02) continue;
            u32 len = 0;
            const u8* img = WB.Rec.OverlayImage(o, len);
            if (!img) continue;
            if (!byContent)
            {
                if (cpu != 0 || WB.Rec.OverlayAt(o.RamStart & nds->MainRAMMask) != (int)o.Id) continue;
            }
            else
            {
                // the overlay whose image matches most of a sample of RAM, among those sharing its start
                auto score = [&](const OverlayInfo& x, const u8* xi, u32 xl)
                {
                    u32 n = std::min<u32>(xl, 4096), same = 0;
                    for (u32 i = 0; i < n; i++)
                        if (nds->MainRAM[(x.RamStart + i) & nds->MainRAMMask] == xi[i]) same++;
                    return n ? same * 100 / n : 0;
                };
                u32 mine = score(o, img, len);
                if (mine < 60) continue;
                bool best = true;
                for (const OverlayInfo& other : ovs)
                {
                    if (&other == &o || other.RamStart != o.RamStart) continue;
                    u32 ol = 0;
                    const u8* oi = WB.Rec.OverlayImage(other, ol);
                    if (oi && score(other, oi, ol) > mine) { best = false; break; }
                }
                if (!best) continue;
            }
            seed(cpu, o.FileId, img, o.RamStart, std::min(o.RamSize, len));
        }
    }
}

u64 Origin::RebuildAfterStateLoad()
{
    LoadedFrame = WB.Frame();
    // what was labelled described memory the load replaced
    Mem.Clear();
    memset(Reg, 0, sizeof(Reg));
    memset(AddrReg, 0, sizeof(AddrReg));
    memset(Pointer, 0, sizeof(Pointer));
    memset(RegFIQ, 0, sizeof(RegFIQ));
    memset(RegIRQ, 0, sizeof(RegIRQ));
    memset(RegSVC, 0, sizeof(RegSVC));
    memset(RegABT, 0, sizeof(RegABT));
    memset(RegUND, 0, sizeof(RegUND));
    memset(AddrFIQ, 0, sizeof(AddrFIQ));
    memset(AddrIRQ, 0, sizeof(AddrIRQ));
    memset(AddrSVC, 0, sizeof(AddrSVC));
    memset(AddrABT, 0, sizeof(AddrABT));
    memset(AddrUND, 0, sizeof(AddrUND));
    memset(PointerBank, 0, sizeof(PointerBank));
    memset(MathLabel, 0, sizeof(MathLabel));
    NonZero[0] = NonZero[1] = 0;
    AddrLabel[0] = AddrLabel[1] = 0;
    OpsActive[0] = OpsActive[1] = false;
    Pending.clear();
    for (auto& c : Runs) for (WriteRun& r : c) r = WriteRun();
    if (!On) return 0;

    SeedStaticModules(true);
    // archived and packed ROM files and copies of the static modules, wherever their bytes are: main
    // RAM, and ITCM (autoloaded code)
    melonDS::NDS* nds = WB.GetNDS();
    auto scan = [&](const u8* mem, u32 base, u32 len)
    {
        std::vector<SubFileRun> runs;
        u32 a = base, end = base + len;
        while (a < end)
        {
            u32 chunk = std::min<u32>(0x10000, end - a);
            runs.clear();
            WB.SubFileResolve(mem, base, len, a, chunk, 3, runs);
            u32 next = a + chunk;
            // whole files first, then archived files: a file's bytes inside an unpacked archive are labelled
            // as the archived file
            for (int pass = 0; pass < 2; pass++)
                for (const SubFileRun& r : runs)
                {
                    if (!r.Count || (r.Members[0] != 0) != (pass == 1)) continue;
                    u32 src = SourceFor(r.FileId, r.Members[0]);
                    // labels the module itself gave stay (they are the same file)
                    if (src) SetRangeLabel(0, r.Addr, r.Size, LabelForSource(src), false);
                    next = std::max(next, r.Addr + r.Size);
                }
            a = next;
        }
    };
    if (nds && nds->MainRAM && WB.SubFileResolve)
    {
        scan(nds->MainRAM, 0x02000000, nds->MainRAMMask + 1);
        scan(nds->ARM9.ITCM, 0x01FF8000, ITCMPhysicalSize);
    }
    u64 n = 0;
    Mem.ForEach([&](u16, u32, const LabelPage* p) { for (u32 i = 0; i < PageSize; i++) if (p->L[i]) n++; });
    return n;
}

void Origin::Serialize(std::vector<u8>& labels, std::vector<u8>& sources) const
{
    auto put32 = [](std::vector<u8>& v, u32 x) { v.insert(v.end(), (u8*)&x, (u8*)&x + 4); };
    labels.clear();
    for (const auto& l : Labels)
    {
        put32(labels, (u32)l.size());
        for (u32 s : l) put32(labels, s);
    }
    sources.clear();
    for (const DataSource& s : SourceList)
    {
        put32(sources, s.Id);
        sources.push_back((u8)s.Kind);
        sources.push_back(s.Enabled ? 1 : 0);
        sources.push_back((u8)s.Cpu);
        sources.push_back((u8)s.Reg);
        put32(sources, s.FileId);
        put32(sources, s.Addr);
        put32(sources, s.Size);
        u32 n = (u32)s.Name.size();
        put32(sources, n);
        sources.insert(sources.end(), s.Name.begin(), s.Name.end());
    }
}

}
