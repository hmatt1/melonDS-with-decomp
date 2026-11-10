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

#ifndef WB_SPANS_H
#define WB_SPANS_H

#include <algorithm>
#include <set>
#include <vector>

#include "types.h"

// Given the functions already stored and the entries the recording has seen, which automatic
// functions shrink, which entries are new functions, and which entries sit inside a function
// the person owns. A walk that stopped at the size cap is open.
namespace WBSpan
{

struct Span
{
    melonDS::u32 Addr = 0, Size = 0;
    bool Automatic = false;
};

struct Entry
{
    melonDS::u32 Addr = 0, Size = 0;
    bool Open = false;
};

struct Resize
{
    melonDS::u32 Addr = 0, NewSize = 0;
};

struct Notch
{
    melonDS::u32 FuncAddr = 0, EntryAddr = 0;
};

struct Decision
{
    std::vector<Resize> Resizes;
    std::vector<Entry> Create;
    std::vector<Notch> Notches;
    std::vector<melonDS::u32> Open;
};

inline Decision Decide(std::vector<Span> spans, std::vector<Entry> entries)
{
    auto byAddr = [](const auto& a, const auto& b) { return a.Addr < b.Addr; };
    std::sort(spans.begin(), spans.end(), byAddr);
    std::sort(entries.begin(), entries.end(), byAddr);
    entries.erase(std::unique(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.Addr == b.Addr; }), entries.end());

    Decision d;
    std::set<melonDS::u32> fresh;
    auto dropOpen = [&](melonDS::u32 addr)
    {
        d.Open.erase(std::remove(d.Open.begin(), d.Open.end(), addr), d.Open.end());
    };
    for (const Entry& e : entries)
    {
        Span* start = nullptr;
        Span* owner = nullptr;
        for (Span& s : spans)
        {
            if (s.Addr == e.Addr) start = &s;
            if (s.Addr < e.Addr && e.Addr < s.Addr + s.Size && (!owner || s.Addr > owner->Addr)) owner = &s;
        }
        if (start)
        {
            if (start->Automatic && e.Open) d.Open.push_back(e.Addr);
            continue;
        }
        if (owner)
        {
            if (!owner->Automatic)
            {
                d.Notches.push_back({owner->Addr, e.Addr});
                continue;
            }
            melonDS::u32 ns = e.Addr - owner->Addr;
            owner->Size = ns;
            dropOpen(owner->Addr);
            if (fresh.count(owner->Addr))
            {
                for (Entry& c : d.Create)
                    if (c.Addr == owner->Addr) c.Size = ns;
            }
            else
                d.Resizes.push_back({owner->Addr, ns});
        }
        melonDS::u32 size = e.Size ? e.Size : 1;
        for (const Span& s : spans)
            if (s.Addr > e.Addr) { size = std::min(size, s.Addr - e.Addr); break; }
        Entry c = e;
        c.Size = size;
        d.Create.push_back(c);
        if (e.Open && size == e.Size) d.Open.push_back(e.Addr);
        spans.push_back({e.Addr, size, true});
        fresh.insert(e.Addr);
        std::sort(spans.begin(), spans.end(), byAddr);
    }
    return d;
}

}

#endif
