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
#include <functional>
#include <map>
#include <zlib.h>
#include "GameArchive.h"
#include "NitroROM.h"

namespace WBArc
{

using melonDS::WB::NitroROM;
using melonDS::WB::RomFile;

namespace
{
u16 Rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
u32 Rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

std::string Lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// a rolling hash of 16-byte windows (buzhash): the next window's hash follows from the last one's in
// constant time
struct BuzTable
{
    u32 T[256];
    BuzTable()
    {
        u32 s = 0x9E3779B9u;
        for (u32& t : T) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; t = s; }
    }
};
const BuzTable Buz;

inline u32 Rotl(u32 v, u32 n) { return (v << n) | (v >> (32 - n)); }

u32 HashWindow(const u8* p)
{
    u32 h = 0;
    for (u32 i = 0; i < 16; i++) h = Rotl(h, 1) ^ Buz.T[p[i]];
    return h;
}

// the hash of the window one byte on: drop out, take in
inline u32 RollWindow(u32 h, u8 out, u8 in)
{
    return Rotl(h, 1) ^ Rotl(Buz.T[out], 16) ^ Buz.T[in];
}

// windows of runs and short repeating patterns match everywhere: not worth indexing
bool Varied(const u8* p)
{
    u32 seen[8] = {};
    int n = 0;
    for (u32 i = 0; i < 16; i++)
    {
        u32& w = seen[p[i] >> 5];
        u32 bit = 1u << (p[i] & 31);
        if (!(w & bit)) { w |= bit; n++; }
    }
    return n >= 5;
}

std::string Hex(u32 v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}
}

bool ArchiveIndex::DetectVfs(const NitroROM& rom, u32& zipFile, u32& binFile, u32& datFile)
{
    zipFile = binFile = datFile = 0xFFFFFFFF;
    for (const RomFile& f : rom.Files)
    {
        if (f.Id >= 0xF000) continue;
        std::string p = Lower(f.Path);
        size_t slash = p.rfind('/');
        std::string dir = slash == std::string::npos ? "" : p.substr(0, slash + 1);
        std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
        if (name != "vfs.zip") continue;
        for (const RomFile& g : rom.Files)
        {
            std::string q = Lower(g.Path);
            if (q == dir + "vfs.bin") binFile = g.Id;
            if (q == dir + "vfs.dat") datFile = g.Id;
        }
        if (binFile != 0xFFFFFFFF && datFile != 0xFFFFFFFF)
        {
            zipFile = f.Id;
            return true;
        }
        binFile = datFile = 0xFFFFFFFF;
    }
    return false;
}

bool ArchiveIndex::Build(const u8* zip, u32 zipLen, const u8* vfsBin, u32 binLen, const u8* vfsDat, u32 datLen, std::string& error)
{
    M.clear();
    Data.clear();
    Idx = ContentIndex();
    ByHeader.clear();

    // end of central directory: the last "PK\5\6" within the comment window
    if (zipLen < 22) { error = "archive too small"; return false; }
    u32 eocd = 0xFFFFFFFF;
    u32 lowest = zipLen > 22 + 0xFFFF ? zipLen - 22 - 0xFFFF : 0;
    for (u32 p = zipLen - 22 + 1; p-- > lowest; )
        if (Rd32(zip + p) == 0x06054B50) { eocd = p; break; }
    if (eocd == 0xFFFFFFFF) { error = "no ZIP end of central directory record"; return false; }
    u32 count = Rd16(zip + eocd + 10);
    CdSize = Rd32(zip + eocd + 12);
    CdStart = Rd32(zip + eocd + 16);
    if ((u64)CdStart + CdSize > zipLen) { error = "central directory outside the archive"; return false; }

    u64 total = 0;
    u32 p = CdStart;
    for (u32 i = 0; i < count; i++)
    {
        if (p + 46 > CdStart + CdSize || Rd32(zip + p) != 0x02014B50) { error = "bad central directory record " + std::to_string(i); return false; }
        Member m;
        m.Index = i;
        m.CdOffset = p - CdStart;
        m.Method = Rd16(zip + p + 10);
        m.Crc = Rd32(zip + p + 16);
        m.PackedSize = Rd32(zip + p + 20);
        m.Size = Rd32(zip + p + 24);
        u32 nlen = Rd16(zip + p + 28), xlen = Rd16(zip + p + 30), clen = Rd16(zip + p + 32);
        m.HeaderOffset = Rd32(zip + p + 42);
        if (p + 46 + nlen > zipLen) { error = "bad central directory name"; return false; }
        m.Path.assign((const char*)zip + p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if ((u64)m.HeaderOffset + 30 <= zipLen && Rd32(zip + m.HeaderOffset) == 0x04034B50)
            m.DataOffset = m.HeaderOffset + 30 + Rd16(zip + m.HeaderOffset + 26) + Rd16(zip + m.HeaderOffset + 28);
        else
            m.DataOffset = m.HeaderOffset;
        if (!m.Path.empty() && m.Path.back() != '/') total += m.Size;
        M.push_back(m);
    }

    // inflate everything into one buffer
    if (total > 512u * 1024 * 1024) { error = "archive contents too large to index"; return false; }
    Data.resize((size_t)total);
    u64 at = 0;
    for (Member& m : M)
    {
        m.ContentOffset = at;
        if (m.Path.empty() || m.Path.back() == '/') continue;
        if ((u64)m.DataOffset + m.PackedSize > zipLen) continue;
        u8* out = Data.data() + at;
        bool ok = false;
        if (m.Method == 0 && m.PackedSize == m.Size)
        {
            memcpy(out, zip + m.DataOffset, m.Size);
            ok = true;
        }
        else if (m.Method == 8)
        {
            z_stream zs {};
            if (inflateInit2(&zs, -MAX_WBITS) == Z_OK)
            {
                zs.next_in = const_cast<Bytef*>(zip + m.DataOffset);
                zs.avail_in = m.PackedSize;
                zs.next_out = out;
                zs.avail_out = m.Size;
                int r = inflate(&zs, Z_FINISH);
                ok = (r == Z_STREAM_END) && zs.total_out == m.Size;
                inflateEnd(&zs);
            }
        }
        if (ok) ok = crc32(0, out, m.Size) == m.Crc;
        m.Ok = ok;
        at += m.Size;
    }

    // vfs.bin: rebuild each file's path from the linked tree and check it against the ZIP
    if (vfsBin && vfsDat && binLen >= 8 && Rd32(vfsBin) == 0xFFFF0000 && Rd32(vfsBin + 4) == 0xFFFF0000)
    {
        std::string dat((const char*)vfsDat, datLen);
        u32 nent = 0, nfiles = 0, ntab = 0;
        if (sscanf(dat.c_str(), "%u , %u , %u", &nent, &nfiles, &ntab) == 3)
        {
            // in 64 bits: counts from the text file can make the table sizes wrap past 4 GB
            u64 pot64 = 8 + (u64)nent * 8, names64 = pot64 + (u64)nfiles * 4;
            u32 pot = (u32)pot64, names = (u32)names64;
            if (names64 <= binLen)
            {
                std::vector<bool> seen(nent, false);
                auto name = [&](u32 off) -> std::string
                {
                    std::string s;
                    for (u32 i = names + off; i < binLen && vfsBin[i]; i++) s += (char)vfsBin[i];
                    return s;
                };
                std::function<void(u32, const std::string&, int)> walk = [&](u32 i, const std::string& prefix, int depth)
                {
                    while (i < nent && !seen[i] && depth < 64)
                    {
                        seen[i] = true;
                        const u8* e = vfsBin + 8 + i * 8;
                        u16 nameOff = Rd16(e), next = Rd16(e + 2), idx = Rd16(e + 6);
                        if (idx == 0x7FFF)
                            walk(i + 1, prefix + name(nameOff) + "/", depth + 1);
                        else if (idx < M.size() && idx < nfiles)    // its central directory offset is in the table
                        {
                            Member& m = M[idx];
                            m.VfsEntry = (int)i;
                            if (Lower(prefix + name(nameOff)) == Lower(m.Path) && Rd32(vfsBin + pot + idx * 4) == m.CdOffset)
                                VfsAgree++;
                        }
                        if (next == 0xFFFF) break;
                        i = next;
                    }
                };
                walk(0, "", 0);
            }
        }
    }

    // identical files share one set of keys
    std::map<std::pair<u32, u32>, std::vector<int>> bySum;
    for (Member& m : M)
    {
        if (!m.Ok || !m.Size) continue;
        auto& v = bySum[{m.Size, m.Crc}];
        for (int o : v)
            if (!memcmp(Content(M[o]), Content(m), m.Size)) { m.DuplicateOf = o; break; }
        if (m.DuplicateOf < 0) v.push_back((int)m.Index);
    }
    for (const Member& m : M)
    {
        if (m.DuplicateOf >= 0) Idx.AddCopy(m.DuplicateOf, (int)m.Index);
        else if (m.Ok) Idx.Add((int)m.Index, Content(m), m.Size);
    }
    Idx.Finish();

    for (const Member& m : M) ByHeader.push_back((int)m.Index);
    std::sort(ByHeader.begin(), ByHeader.end(), [this](int a, int b) { return M[a].HeaderOffset < M[b].HeaderOffset; });

    u32 files = 0, ok = 0;
    for (const Member& m : M) if (!m.Path.empty() && m.Path.back() != '/') { files++; if (m.Ok) ok++; }
    Summary = "Octane VFS: " + ArchivePath + ", " + std::to_string(files) + " files (" + std::to_string(Data.size() >> 20) + " MB unpacked)";
    if (ok != files) Summary += ", " + std::to_string(files - ok) + " unreadable";
    if (vfsBin) Summary += "; " + IndexPath + " agrees for " + std::to_string(VfsAgree);
    return true;
}

int ArchiveIndex::MemberAt(u32 off, std::string* part, u32* partOffset) const
{
    auto it = std::upper_bound(ByHeader.begin(), ByHeader.end(), off, [this](u32 v, int i) { return v < M[i].HeaderOffset; });
    if (it != ByHeader.begin())
    {
        const Member& m = M[*std::prev(it)];
        if (off < m.DataOffset)
        {
            if (part) *part = "local header";
            if (partOffset) *partOffset = off - m.HeaderOffset;
            return (int)m.Index;
        }
        if (off < m.DataOffset + m.PackedSize)
        {
            if (part) *part = m.Method == 0 ? "stored data" : "compressed data";
            if (partOffset) *partOffset = off - m.DataOffset;
            return (int)m.Index;
        }
    }
    if (off >= CdStart && off < CdStart + CdSize)
    {
        // records are in member order
        u32 rel = off - CdStart;
        auto jt = std::upper_bound(M.begin(), M.end(), rel, [](u32 v, const Member& m) { return v < m.CdOffset; });
        if (jt != M.begin())
        {
            if (part) *part = "central directory record";
            if (partOffset) *partOffset = rel - std::prev(jt)->CdOffset;
            return (int)std::prev(jt)->Index;
        }
    }
    return -1;
}

std::string ArchiveIndex::Describe(u32 off) const
{
    std::string part;
    u32 po = 0;
    int i = MemberAt(off, &part, &po);
    if (i < 0) return std::string();
    return M[i].Path + ", " + part + " +" + Hex(po);
}

int ArchiveIndex::FindPath(const std::string& path) const
{
    std::string want = Lower(path);
    while (!want.empty() && (want[0] == '/' || want[0] == '\\')) want.erase(0, 1);
    for (char& c : want) if (c == '\\') c = '/';
    for (const Member& m : M)
        if (Lower(m.Path) == want) return (int)m.Index;
    return -1;
}

void ContentIndex::Add(int id, const u8* data, u32 size)
{
    if (id < 0) return;
    if ((size_t)id >= E.size()) { E.resize(id + 1); Copies.resize(id + 1); }
    E[id] = Entry{data, size};
    for (u32 off = 0; off + Window <= size; off += Stride)
        if (Varied(data + off)) Keys.push_back({HashWindow(data + off), (u32)id, off});
}

void ContentIndex::AddCopy(int original, int copy)
{
    if (original < 0) return;
    if ((size_t)original >= Copies.size()) { E.resize(original + 1); Copies.resize(original + 1); }
    Copies[original].push_back(copy);
}

void ContentIndex::Finish()
{
    std::sort(Keys.begin(), Keys.end(), [](const Key& a, const Key& b) { return a.Hash < b.Hash; });
    // about 16 buckets per key: one window in 16 that has no key still passes
    u32 bits = 16;
    while (bits < 30 && (1ull << bits) < (u64)Keys.size() * 16) bits++;
    FilterShift = 32 - bits;
    Filter.assign((size_t)1 << (bits - 6), 0);
    for (const Key& k : Keys)
    {
        u32 b = (k.Hash * 0x9E3779B1u) >> FilterShift;
        Filter[b >> 6] |= 1ull << (b & 63);
    }
}

bool ContentIndex::Varied(const u8* p)
{
    return WBArc::Varied(p);
}

void ContentIndex::Extend(const u8* mem, u32 memBase, u32 memLen, u32 q, const u8* c, u32 csize, u32 off, u32& s, u32& e)
{
    u32 b = 0;
    while (q - b > memBase && off - b > 0 && mem[q - b - 1 - memBase] == c[off - b - 1]) b++;
    u32 f = 0;
    while (q + f < memBase + memLen && off + f < csize && mem[q + f - memBase] == c[off + f]) f++;
    s = q - b;
    e = q + f;
}

std::vector<Match> ContentIndex::Resolve(const u8* mem, u32 memBase, u32 memLen, u32 addr, u32 size, size_t maxMatches) const
{
    std::vector<Match> out;
    if (addr < memBase || addr >= memBase + memLen || Filter.empty()) return out;
    u32 end = std::min<u64>((u64)addr + std::max<u32>(size, 1), (u64)memBase + memLen);
    u32 p = addr;
    u32 h = 0, hq = 0;                     // the hash of the window at hq
    bool rolling = false;
    while (p < end && out.size() < maxMatches)
    {
        Match best;
        u32 bestS = 0, bestE = 0;
        for (u32 j = 0; j < Stride && p + j + Window <= memBase + memLen; j++)
        {
            u32 q = p + j;
            const u8* w = mem + (q - memBase);
            h = (rolling && hq + 1 == q) ? RollWindow(h, w[-1], w[Window - 1]) : HashWindow(w);
            hq = q;
            rolling = true;
            if (!MayHave(h) || !Varied(w)) continue;
            auto range = std::equal_range(Keys.begin(), Keys.end(), Key{h, 0, 0}, [](const Key& a, const Key& b) { return a.Hash < b.Hash; });
            int tried = 0;
            for (auto it = range.first; it != range.second && tried < 64; ++it, ++tried)
            {
                const Entry& c = E[it->Id];
                if (memcmp(w, c.Data + it->Offset, Window)) continue;
                u32 s, e;
                Extend(mem, memBase, memLen, q, c.Data, c.Size, it->Offset, s, e);
                if (e <= p || e - s < MinMatch) continue;
                u32 moff = it->Offset - (q - s);
                if (best.Member < 0 || e - s > bestE - bestS)
                {
                    best = Match{s, e - s, (int)it->Id, moff, {}};
                    bestS = s;
                    bestE = e;
                }
                else if (s == bestS && e == bestE && (int)it->Id != best.Member &&
                         std::find(best.Alternatives.begin(), best.Alternatives.end(), (int)it->Id) == best.Alternatives.end())
                    best.Alternatives.push_back((int)it->Id);
            }
            // later probes inside the same run find the same run again
            if (best.Member >= 0 && bestE > q + Window) break;
        }
        if (best.Member < 0) { p += Stride; continue; }
        size_t direct = best.Alternatives.size();
        for (int c : Copies[best.Member]) best.Alternatives.push_back(c);
        for (size_t k = 0; k < direct; k++)
            for (int c : Copies[best.Alternatives[k]]) best.Alternatives.push_back(c);
        // a longer run found from a later probe can swallow a shorter one reported before it
        while (!out.empty() && best.Addr <= out.back().Addr && bestE >= out.back().Addr + out.back().Size) out.pop_back();
        out.push_back(best);
        p = bestE;
    }
    return out;
}

std::vector<Match> ArchiveIndex::Locate(int member, const u8* mem, u32 memBase, u32 memLen, size_t maxMatches) const
{
    if (member < 0 || (size_t)member >= M.size()) return {};
    const Member& m = M[member];
    if (!m.Ok || !m.Size) return {};
    return ContentIndex::Find(member, Content(m), m.Size, Idx.MinMatch, mem, memBase, memLen, maxMatches);
}

std::vector<Match> ContentIndex::Find(int id, const u8* c, u32 csize, u32 minMatch, const u8* mem, u32 memBase, u32 memLen, size_t maxMatches)
{
    std::vector<Match> out;
    u32 want = std::min(csize, minMatch);

    // probe a distinctive window near several points of the file; partly overwritten copies still show up
    std::vector<u32> probes;
    if (csize < Window) probes.push_back(0);
    else
        for (u32 k = 0; k < 8; k++)
        {
            u32 o = (u32)((u64)(csize - Window) * k / 8);
            u32 lim = std::min(csize - Window, o + 512);
            while (o < lim && !WBArc::Varied(c + o)) o++;
            if (WBArc::Varied(c + o) && std::find(probes.begin(), probes.end(), o) == probes.end()) probes.push_back(o);
        }
    u32 plen = std::min(csize, Window);
    for (u32 o : probes)
    {
        std::boyer_moore_horspool_searcher searcher(c + o, c + o + plen);
        const u8* from = mem;
        const u8* stop = mem + memLen;
        while (out.size() < maxMatches)
        {
            const u8* hit = std::search(from, stop, searcher);
            if (hit == stop) break;
            u32 q = memBase + (u32)(hit - mem);
            u32 s, e;
            Extend(mem, memBase, memLen, q, c, csize, o, s, e);
            from = hit + 1;
            if (e - s < want) continue;
            bool dup = false;
            for (const Match& x : out)
                if (s < x.Addr + x.Size && e > x.Addr) { dup = true; break; }
            if (!dup) out.push_back(Match{s, e - s, id, o - (q - s), {}});
            from = mem + (e - memBase);
        }
    }
    std::sort(out.begin(), out.end(), [](const Match& a, const Match& b) { return a.Size != b.Size ? a.Size > b.Size : a.Addr < b.Addr; });
    return out;
}

namespace
{
bool UnpackLz(const u8* in, u32 len, u32 p, u32 size, bool lz11, std::vector<u8>& out, u32& used)
{
    out.clear();
    out.reserve(size);
    while (out.size() < size)
    {
        if (p >= len) return false;
        u8 flags = in[p++];
        for (int b = 7; b >= 0 && out.size() < size; b--)
        {
            if (!(flags & (1 << b)))
            {
                if (p >= len) return false;
                out.push_back(in[p++]);
                continue;
            }
            u32 n, disp;
            if (!lz11)
            {
                if (p + 2 > len) return false;
                n = (in[p] >> 4) + 3;
                disp = (((in[p] & 0xF) << 8) | in[p + 1]) + 1;
                p += 2;
            }
            else
            {
                if (p + 2 > len) return false;
                u32 ind = in[p] >> 4;
                if (ind == 0)
                {
                    if (p + 3 > len) return false;
                    n = (((in[p] & 0xF) << 4) | (in[p + 1] >> 4)) + 0x11;
                    disp = (((in[p + 1] & 0xF) << 8) | in[p + 2]) + 1;
                    p += 3;
                }
                else if (ind == 1)
                {
                    if (p + 4 > len) return false;
                    n = (((in[p] & 0xF) << 12) | (in[p + 1] << 4) | (in[p + 2] >> 4)) + 0x111;
                    disp = (((in[p + 2] & 0xF) << 8) | in[p + 3]) + 1;
                    p += 4;
                }
                else
                {
                    n = ind + 1;
                    disp = (((in[p] & 0xF) << 8) | in[p + 1]) + 1;
                    p += 2;
                }
            }
            if (disp > out.size()) return false;
            for (u32 i = 0; i < n && out.size() < size; i++) out.push_back(out[out.size() - disp]);
        }
    }
    used = p;
    return true;
}

bool UnpackRle(const u8* in, u32 len, u32 p, u32 size, std::vector<u8>& out, u32& used)
{
    out.clear();
    out.reserve(size);
    while (out.size() < size)
    {
        if (p >= len) return false;
        u8 f = in[p++];
        if (f & 0x80)
        {
            if (p >= len) return false;
            u8 v = in[p++];
            for (u32 i = 0; i < (u32)(f & 0x7F) + 3u && out.size() < size; i++) out.push_back(v);
        }
        else
            for (u32 i = 0; i < (u32)(f & 0x7F) + 1u && out.size() < size; i++)
            {
                if (p >= len) return false;
                out.push_back(in[p++]);
            }
    }
    used = p;
    return true;
}

// the tree is at in[t], its root at in[t + 1]; the bitstream (32-bit units, top bit first) after it
bool UnpackHuffman(const u8* in, u32 len, u32 t, u32 size, u32 bits, std::vector<u8>& out, u32& used)
{
    if (t >= len) return false;
    u32 treeEnd = t + ((u32)in[t] + 1) * 2;
    u32 p = treeEnd;
    out.clear();
    out.reserve(size);
    u32 node = t + 1;
    u8 cur = 0;
    u32 half = 0;
    while (out.size() < size)
    {
        if (p + 4 > len) return false;
        u32 word = Rd32(in + p);
        p += 4;
        for (int b = 31; b >= 0 && out.size() < size; b--)
        {
            if (node >= treeEnd) return false;
            u8 n = in[node];
            u32 bit = (word >> b) & 1;
            u32 child = (node & ~1u) + (n & 0x3F) * 2 + 2 + bit;
            if (child >= treeEnd) return false;
            if (n & (bit ? 0x40 : 0x80))
            {
                u8 v = in[child];
                if (bits == 8) out.push_back(v);
                else
                {
                    cur |= (u8)((v & 0xF) << (half * 4));
                    if (++half == 2) { out.push_back(cur); cur = 0; half = 0; }
                }
                node = t + 1;
            }
            else node = child;
        }
    }
    used = p;
    return true;
}
}

bool UnpackBios(const u8* in, u32 len, std::vector<u8>& out, std::string& form)
{
    u32 p = 0;
    if (len >= 8 && !memcmp(in, "LZ77", 4)) p = 4;
    if (len < p + 8) return false;
    u32 hdr = Rd32(in + p);
    u32 type = hdr & 0xFF, size = hdr >> 8;
    p += 4;
    if (size == 0 && (type == 0x10 || type == 0x11))
    {
        // a larger file: the size follows the header
        size = Rd32(in + p);
        p += 4;
    }
    if (size < ContentIndex::Window || size > (16u << 20)) return false;
    u32 used = 0;
    bool ok = false;
    switch (type)
    {
    case 0x10: ok = UnpackLz(in, len, p, size, false, out, used); form = "lz10"; break;
    case 0x11: ok = UnpackLz(in, len, p, size, true, out, used); form = "lz11"; break;
    case 0x24: ok = UnpackHuffman(in, len, p, size, 4, out, used); form = "huffman"; break;
    case 0x28: ok = UnpackHuffman(in, len, p, size, 8, out, used); form = "huffman"; break;
    case 0x30: ok = UnpackRle(in, len, p, size, out, used); form = "rle"; break;
    default: return false;
    }
    // data that merely starts like a header decodes into the rest of the file, or past it
    return ok && out.size() == size && used <= len && len - used < 32;
}

void RomFileIndex::Build(std::vector<Input> files)
{
    F.clear();
    Content.clear();
    Ids.clear();
    Total = 0;
    Idx = ContentIndex();
    for (Input& in : files)
    {
        if (in.Bytes.size() < ContentIndex::Window || in.Bytes.size() > MaxFileSize) continue;
        if (in.Raw)
        {
            F.push_back({in.FileId, in.Path, "raw", (u32)in.Bytes.size()});
            Content.push_back(std::move(in.Bytes));
            continue;
        }
        std::vector<u8> unpacked;
        std::string form;
        if (!UnpackBios(in.Bytes.data(), (u32)in.Bytes.size(), unpacked, form)) continue;
        F.push_back({in.FileId, in.Path, form, (u32)unpacked.size()});
        Content.push_back(std::move(unpacked));
        Ids.push_back(in.FileId);
    }
    // the contents don't move from here on
    for (size_t i = 0; i < F.size(); i++)
    {
        Idx.Add((int)i, Content[i].data(), (u32)Content[i].size());
        Total += Content[i].size();
    }
    Idx.Finish();
    std::sort(Ids.begin(), Ids.end());
}

}
