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

#include "FlowDisasm.h"

#include <map>
#include <set>
#include <utility>

namespace melonDS::WB
{

namespace
{

struct QFlow
{
    FlowKind Kind = FlowKind::Priority;
    u32 Dest = 0;
    u32 Source = 0;
    bool Thumb = false;
    FlowReach Reach = FlowReach::Entry;

    bool operator<(const QFlow& o) const
    {
        if (Kind != o.Kind) return (int)Kind < (int)o.Kind;
        return Dest < o.Dest;
    }
};

bool Aligned(u32 addr, bool thumb)
{
    return thumb ? (addr & 1) == 0 : (addr & 3) == 0;
}

bool SameInsn(const FlowInsn& a, u32 raw, u32 size, bool thumb)
{
    u32 mask = size >= 4 ? 0xFFFFFFFFu : 0xFFFFu;
    return a.Thumb == thumb && a.Size == size && (a.Raw & mask) == (raw & mask);
}

const FlowInsn* Covering(const std::map<u32, FlowInsn>& m, u32 addr)
{
    auto it = m.upper_bound(addr);
    if (it == m.begin()) return nullptr;
    --it;
    if (addr < it->second.Addr + it->second.Size) return &it->second;
    return nullptr;
}

// An instruction already in this batch overlaps [addr, addr + size).
bool Overlaps(const std::map<u32, FlowInsn>& m, u32 addr, u32 size)
{
    auto it = m.lower_bound(addr);
    if (it != m.end() && it->first < addr + size) return true;
    if (it != m.begin())
    {
        --it;
        if (it->second.Addr + it->second.Size > addr) return true;
    }
    return false;
}

class Walker
{
public:
    FlowImage& Image;
    FlowOptions Opt;
    std::atomic<bool>* Cancel = nullptr;
    FlowDisasmResult Result;
    std::set<QFlow> Seeds, Priority, Processed;

    bool Stopped() const
    {
        return Result.Incomplete || (Cancel && Cancel->load(std::memory_order_relaxed));
    }

    void Note(FlowNote::Kind kind, u32 addr, const char* text)
    {
        Result.NoteCount++;
        if (Result.Notes.size() >= 32) return;
        Result.Notes.push_back({kind, addr, text});
    }

    // Same kind and destination collapse. A second path that wants the other mode is reported.
    bool Enqueue(std::set<QFlow>& q, QFlow flow)
    {
        auto it = q.find(flow);
        if (it != q.end())
        {
            if (it->Thumb != flow.Thumb)
                Note(FlowNote::Context, flow.Dest, "reached in both ARM and Thumb mode");
            return false;
        }
        q.insert(flow);
        return true;
    }

    bool Decode(u32 addr, bool thumb, DisasmInsn& in, u32& raw, bool& memory)
    {
        memory = false;
        raw = 0;
        u32 need = thumb ? 2u : 4u;
        u32 lo = 0;
        if (!Image.Read(addr, need, lo)) { memory = true; return false; }
        raw = lo;
        if (thumb)
        {
            bool prefix = (lo & 0xF800) == 0xF000;
            u32 hi = 0;
            if (Image.Read(addr + 2, 2, hi)) raw |= (hi & 0xFFFF) << 16;
            else if (prefix) return false;
        }
        Disassemble(addr, raw, thumb, in);
        if (thumb && in.Size == 2) raw &= 0xFFFF;
        return in.Valid;
    }

    void DisassembleBlock(QFlow incoming, std::map<u32, FlowInsn>& have, std::vector<FlowInsn>& order, std::vector<QFlow>& recorded, std::set<QFlow>& branches)
    {
        u32 addr = incoming.Dest;
        bool thumb = incoming.Thumb;
        if (!Image.Initialized(addr))
        {
            Note(FlowNote::Memory, addr, "not in initialized memory");
            return;
        }
        bool first = true;
        u32 prev = incoming.Source;
        u32 repeatRaw = 0, repeatN = 0;
        while (!Stopped())
        {
            if (!Image.Restricted(addr)) return;
            if (!Aligned(addr, thumb))
            {
                Note(FlowNote::Alignment, addr, "not instruction-aligned");
                return;
            }
            if (Image.DefinedData(addr))
            {
                Note(FlowNote::Data, addr, "defined data");
                return;
            }
            if (Image.Offcut(addr))
            {
                Note(FlowNote::Offcut, addr, "landed in the middle of an instruction");
                return;
            }
            if (const FlowInsn* covered = Covering(have, addr))
                if (covered->Addr != addr)
                {
                    Note(FlowNote::Offcut, addr, "landed in the middle of an instruction");
                    return;
                }

            bool memory = false;
            DisasmInsn in;
            u32 raw = 0;
            bool decoded = Decode(addr, thumb, in, raw, memory);
            if (!decoded)
            {
                Note(memory ? FlowNote::Memory : FlowNote::Parse, addr,
                     memory ? "instruction does not fit in initialized memory" : "does not decode");
                return;
            }

            auto got = have.find(addr);
            if (got != have.end())
            {
                if (!SameInsn(got->second, raw, in.Size, thumb))
                    Note(FlowNote::Prototype, addr, "decoded differently from an instruction already in this batch");
                return;
            }
            FlowInsn known;
            if (Image.InstructionAt(addr, known))
            {
                if (!SameInsn(known, raw, in.Size, thumb))
                    Note(FlowNote::Prototype, addr, "decoded differently from an instruction already known");
                return;
            }
            if (Overlaps(have, addr, in.Size))
            {
                Note(FlowNote::Offcut, addr, "overlaps an instruction already in this batch");
                return;
            }

            bool repeat = false;
            if (raw == repeatRaw) repeatN++;
            else { repeatRaw = raw; repeatN = 1; }
            if (Opt.RepeatLimit && repeatN > Opt.RepeatLimit)
            {
                Note(FlowNote::Repeat, addr, "the same instruction repeats");
                repeat = true;
            }

            bool terminal = false;
            InsnEdges edges = EdgesOf(addr, in, false);
            if (Opt.FollowFlow)
            {
                for (const InsnEdges::Target& t : edges.Targets)
                {
                    if (!Aligned(t.Addr, t.Thumb))
                    {
                        Note(FlowNote::Alignment, t.Addr, "target is not instruction-aligned");
                        continue;
                    }
                    if (t.Call && Image.NoReturn(t.Addr)) { terminal = true; continue; }
                    QFlow f;
                    f.Dest = t.Addr;
                    f.Source = addr;
                    f.Thumb = t.Thumb;
                    if (t.Call)
                    {
                        f.Kind = FlowKind::Call;
                        f.Reach = FlowReach::Call;
                        recorded.push_back(f);
                    }
                    else
                    {
                        f.Kind = FlowKind::Branch;
                        f.Reach = FlowReach::Branch;
                        Enqueue(branches, f);
                        recorded.push_back(f);
                    }
                }
            }
            if (terminal) edges.Falls = false;

            FlowInsn added;
            added.Addr = addr;
            added.Size = in.Size ? in.Size : (thumb ? 2u : 4u);
            added.Thumb = thumb;
            added.Raw = raw;
            added.From = first ? incoming.Source : prev;
            added.Reach = first ? incoming.Reach : FlowReach::Continue;
            added.Literal = in.LiteralLoad && in.HasRef;
            added.LiteralAddr = in.Ref;
            have[addr] = added;
            order.push_back(added);
            first = false;
            prev = addr;

            if (Result.Added.size() + order.size() >= Opt.MaxInstructions)
            {
                Note(FlowNote::Limit, addr, "stopped at the instruction limit");
                Result.Incomplete = true;
                return;
            }
            if (repeat || !edges.Falls) return;

            u32 next = edges.Next;
            if ((u32)have.size() >= Opt.MaxPerSet || !Image.Initialized(next))
            {
                QFlow f;
                f.Kind = FlowKind::Priority;
                f.Dest = next;
                f.Source = addr;
                f.Thumb = thumb;
                f.Reach = FlowReach::Continue;
                recorded.push_back(f);
                return;
            }
            if (in.IsCall)
            {
                QFlow f;
                f.Kind = FlowKind::CallFallthrough;
                f.Dest = next;
                f.Source = addr;
                f.Thumb = thumb;
                f.Reach = FlowReach::AfterCall;
                Enqueue(branches, f);
                recorded.push_back(f);
                return;
            }
            addr = next;
            thumb = added.Thumb;
        }
    }

    void Run(FlowSeed start)
    {
        Seeds.clear();
        Priority.clear();
        QFlow seed;
        seed.Kind = FlowKind::Priority;
        seed.Dest = start.Addr;
        seed.Source = start.From;
        seed.Thumb = start.Thumb;
        seed.Reach = start.Reach;
        Seeds.insert(seed);

        while (!Stopped())
        {
            std::set<QFlow> branches;
            Processed.clear();
            if (Priority.empty())
            {
                if (Seeds.empty()) return;
                auto it = Seeds.begin();
                Priority.insert(*it);
                Seeds.erase(it);
            }

            std::map<u32, FlowInsn> have;
            std::vector<FlowInsn> order;
            std::vector<QFlow> recorded;
            while ((u32)have.size() < Opt.MaxPerSet && !Stopped())
            {
                std::set<QFlow>* q = !Priority.empty() ? &Priority : !branches.empty() ? &branches : nullptr;
                if (!q) break;
                QFlow flow = *q->begin();
                q->erase(q->begin());
                if (!Image.Restricted(flow.Dest)) continue;
                Processed.insert(flow);

                FlowInsn existing;
                if (Image.InstructionAt(flow.Dest, existing)) continue;
                if (Image.DefinedData(flow.Dest) || Image.Offcut(flow.Dest))
                {
                    Note(Image.Offcut(flow.Dest) ? FlowNote::Offcut : FlowNote::Data, flow.Dest,
                         Image.Offcut(flow.Dest) ? "landed in the middle of an instruction" : "defined data");
                    continue;
                }
                if (!Aligned(flow.Dest, flow.Thumb))
                {
                    Note(FlowNote::Alignment, flow.Dest, "not instruction-aligned");
                    continue;
                }
                DisassembleBlock(flow, have, order, recorded, branches);
            }

            if (!order.empty())
            {
                Result.Added.insert(Result.Added.end(), order.begin(), order.end());
                Image.Commit(order);
            }
            if (Stopped()) return;

            // Flows whose instruction was kept. A call waits in the seed queue. Anything else this
            // batch did not already follow becomes a priority flow, so a straight line split by the
            // batch limit resumes before ordinary branches.
            for (const QFlow& flow : recorded)
            {
                if (!have.count(flow.Source)) continue;
                if (Processed.count(flow) && flow.Kind != FlowKind::Call) continue;
                // Kind is kept, so a straight-line continuation (priority) stays ahead of a branch
                // that this batch did not reach, and a call still waits in the seed queue.
                if (flow.Kind == FlowKind::Call) Enqueue(Seeds, flow);
                else Enqueue(Priority, flow);
            }
        }
    }
};

}

InsnEdges EdgesOf(u32 addr, const DisasmInsn& in, bool noreturn)
{
    InsnEdges e;
    e.Next = addr + (in.Size ? in.Size : (in.Thumb ? 2u : 4u));
    if (!in.Valid) return e;
    bool terminal = false;
    if (in.HasTarget && (in.IsBranch || in.IsCall))
    {
        if (in.IsCall && noreturn) terminal = true;
        else e.Targets.push_back({in.Target, in.TargetThumb, in.IsCall});
    }
    if (terminal) return e;
    if (in.IsCall) { e.Falls = true; return e; }
    if (in.IsBranch && !in.Conditional) return e;
    e.Falls = true;
    return e;
}

FlowDisasmResult Disassemble(FlowImage& image, FlowSeed start, const FlowOptions& opt, std::atomic<bool>* cancel)
{
    Walker w{image, opt, cancel};
    if (cancel && cancel->load(std::memory_order_relaxed)) { w.Result.Incomplete = true; return w.Result; }
    w.Run(start);
    if (cancel && cancel->load(std::memory_order_relaxed)) w.Result.Incomplete = true;
    return w.Result;
}

FlowDisasmResult DisassembleSet(FlowImage& image, const std::vector<FlowSeed>& starts, const FlowOptions& opt, std::atomic<bool>* cancel)
{
    FlowDisasmResult all;
    for (const FlowSeed& s : starts)
    {
        if (cancel && cancel->load(std::memory_order_relaxed)) { all.Incomplete = true; break; }
        if ((u32)all.Added.size() >= opt.MaxInstructions) { all.Incomplete = true; all.NoteCount++; break; }
        FlowInsn have;
        if (image.InstructionAt(s.Addr, have)) continue;
        FlowOptions step = opt;
        step.MaxInstructions = opt.MaxInstructions - (u32)all.Added.size();
        FlowDisasmResult one = Disassemble(image, s, step, cancel);
        all.Added.insert(all.Added.end(), one.Added.begin(), one.Added.end());
        all.NoteCount += one.NoteCount;
        for (const FlowNote& n : one.Notes)
            if (all.Notes.size() < 32) all.Notes.push_back(n);
        if (one.Incomplete) { all.Incomplete = true; break; }
    }
    return all;
}

}
