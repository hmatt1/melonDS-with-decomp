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
#include "Containers.h"

namespace WBArc
{

namespace
{
u16 Rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
u32 Rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

std::string Lower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '\\') c = '/';
    }
    return s;
}

std::string Hex(u32 v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

// a NUL-terminated name at p[off], at most 255 characters; empty if it runs past len
std::string Str(const u8* p, u32 len, u32 off)
{
    std::string s;
    for (u32 i = off; i < len && p[i] && s.size() < 255; i++) s += (char)p[i];
    return s;
}

bool Magic(const u8* p, u32 len, const char* m) { return len >= 4 && !memcmp(p, m, 4); }

// the file extension for a kind
std::string ExtOf(const std::string& kind)
{
    static const std::map<std::string, std::string> g3d = {
        {"BMD0", "nsbmd"}, {"BTX0", "nsbtx"}, {"BCA0", "nsbca"}, {"BTP0", "nsbtp"},
        {"BTA0", "nsbta"}, {"BMA0", "nsbma"}, {"BVA0", "nsbva"}};
    auto it = g3d.find(kind);
    if (it != g3d.end()) return it->second;
    return kind.empty() ? "bin" : Lower(kind);
}

std::string Numbered(u32 i, const std::string& ext)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%04u.", i);
    return buf + ext;
}

// BIOS-compressed data starts with its type byte (or an "LZ77" tag)
bool LooksPacked(const u8* p, u32 len)
{
    if (len < 8) return false;
    if (!memcmp(p, "LZ77", 4)) return true;
    return p[0] == 0x10 || p[0] == 0x11 || p[0] == 0x24 || p[0] == 0x28 || p[0] == 0x30;
}

u64 Fnv(const u8* p, u32 n)
{
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
}

std::string KindOf(const u8* p, u32 len)
{
    if (len < 4) return std::string();
    std::string s((const char*)p, 4);
    bool letter = false;
    for (char c : s)
    {
        if (c >= 'A' && c <= 'Z') letter = true;
        else if (!(c >= '0' && c <= '9')) return std::string();
    }
    if (!letter) return std::string();
    // the 2D graphics formats store their magic reversed: "RGCN" is an NCGR
    if (s[3] == 'N' && s[0] != 'N') std::reverse(s.begin(), s.end());
    return s;
}

bool ReadNarc(const u8* p, u32 len, std::vector<ContainerEntry>& out, std::string& error)
{
    out.clear();
    if (len < 16 || !Magic(p, len, "NARC")) { error = "not a NARC"; return false; }
    u32 pos = Rd16(p + 12);
    u32 nsec = Rd16(p + 14);
    u32 btaf = 0, btafSize = 0, btnf = 0, btnfSize = 0, gmif = 0, gmifSize = 0;
    for (u32 s = 0; s < nsec && s < 16; s++)
    {
        if ((u64)pos + 8 > len) break;
        u32 size = Rd32(p + pos + 4);
        if (size < 8 || (u64)pos + size > len) { error = "section " + std::to_string(s) + " runs past the file"; return false; }
        if (Magic(p + pos, 4, "BTAF")) { btaf = pos; btafSize = size; }
        else if (Magic(p + pos, 4, "BTNF")) { btnf = pos; btnfSize = size; }
        else if (Magic(p + pos, 4, "GMIF")) { gmif = pos; gmifSize = size; }
        pos += size;
    }
    if (!btaf || !gmif) { error = "no BTAF or GMIF section"; return false; }
    if (btafSize < 12) { error = "BTAF too small"; return false; }
    u32 count = Rd16(p + btaf + 8);
    if (12 + (u64)count * 8 > btafSize) { error = "BTAF holds fewer entries than it counts"; return false; }

    // names: the file name table, when it has any
    std::vector<std::string> names(count);
    if (btnf && btnfSize >= 16)
    {
        const u8* fnt = p + btnf + 8;
        u32 flen = btnfSize - 8;
        u32 ndirs = Rd16(fnt + 6);
        if (ndirs && ndirs <= 4096 && (u64)ndirs * 8 <= flen)
        {
            std::function<void(u32, const std::string&, int)> walk = [&](u32 dir, const std::string& prefix, int depth)
            {
                u32 at = Rd32(fnt + dir * 8);
                u32 id = Rd16(fnt + dir * 8 + 4);
                while (at < flen)
                {
                    u8 b = fnt[at++];
                    if (!b) break;
                    u32 l = b & 0x7F;
                    if ((u64)at + l > flen) break;
                    std::string name((const char*)fnt + at, l);
                    at += l;
                    if (b & 0x80)
                    {
                        if (at + 2 > flen) break;
                        u32 sub = Rd16(fnt + at) & 0xFFF;
                        at += 2;
                        if (sub < ndirs && depth < 16) walk(sub, prefix + name + "/", depth + 1);
                    }
                    else
                    {
                        if (id < count) names[id] = prefix + name;
                        id++;
                    }
                }
            };
            walk(0, "", 0);
        }
    }

    u32 data = gmif + 8, dataLen = gmifSize - 8;
    for (u32 i = 0; i < count; i++)
    {
        u32 s = Rd32(p + btaf + 12 + i * 8), e = Rd32(p + btaf + 16 + i * 8);
        if (s > e || e > dataLen) { error = "file " + std::to_string(i) + " lies outside GMIF"; return false; }
        out.push_back({names[i], data + s, e - s});
    }
    return true;
}

bool ReadSdat(const u8* p, u32 len, std::vector<ContainerEntry>& out, std::string& error)
{
    out.clear();
    if (len < 0x40 || !Magic(p, len, "SDAT")) { error = "not an SDAT"; return false; }
    u32 symb = Rd32(p + 0x10), info = Rd32(p + 0x18), fat = Rd32(p + 0x20), fatSize = Rd32(p + 0x24);
    if ((u64)fat + 12 > len || !Magic(p + fat, 4, "FAT ")) { error = "no FAT block"; return false; }
    u32 count = Rd32(p + fat + 8);
    if (fatSize < 12 || (u64)count * 16 > fatSize - 12 || (u64)fat + 12 + (u64)count * 16 > len) { error = "FAT holds fewer entries than it counts"; return false; }

    // which kind of file each FAT entry is, and the slot in its INFO record that names it
    static const int Types[] = {0, 1, 2, 3, 7};          // sequence, sequence archive, bank, wave archive, stream
    static const char* const Ext[8] = {"sseq", "ssar", "sbnk", "swar", "", "", "", "strm"};
    std::vector<int> type(count, -1), slot(count, -1);
    if (info && (u64)info + 8 + 32 <= len && Magic(p + info, 4, "INFO"))
        for (int t : Types)
        {
            u64 rec = (u64)info + Rd32(p + info + 8 + t * 4);
            if (rec + 4 > len) continue;
            u32 n = Rd32(p + rec);
            if (n > 0x10000 || rec + 4 + (u64)n * 4 > len) continue;
            for (u32 i = 0; i < n; i++)
            {
                u64 ent = (u64)info + Rd32(p + rec + 4 + i * 4);
                if (ent == info || ent + 2 > len) continue;
                u32 fid = Rd16(p + ent);
                if (fid < count && type[fid] < 0) { type[fid] = t; slot[fid] = (int)i; }
            }
        }
    // symbol names, when the SDAT keeps them
    std::map<std::pair<int, int>, std::string> sym;
    if (symb && (u64)symb + 8 + 32 <= len && Magic(p + symb, 4, "SYMB"))
        for (int t : Types)
        {
            u32 ro = Rd32(p + symb + 8 + t * 4);
            u64 rec = (u64)symb + ro;
            if (!ro || rec + 4 > len) continue;
            u32 n = Rd32(p + rec);
            u32 stride = t == 1 ? 8 : 4;     // sequence archives: the name, then the archive's own table
            if (n > 0x10000 || rec + 4 + (u64)n * stride > len) continue;
            for (u32 i = 0; i < n; i++)
            {
                u32 no = Rd32(p + rec + 4 + i * stride);
                if (no) sym[{t, (int)i}] = Str(p, len, symb + no);
            }
        }

    for (u32 i = 0; i < count; i++)
    {
        u32 off = Rd32(p + fat + 12 + i * 16), size = Rd32(p + fat + 16 + i * 16);
        if ((u64)off + size > len) continue;
        std::string name;
        if (type[i] >= 0)
        {
            auto it = sym.find({type[i], slot[i]});
            std::string ext = Ext[type[i]];
            name = it != sym.end() && !it->second.empty() ? it->second + "." + ext : Numbered(i, ext);
        }
        out.push_back({name, off, size});
    }
    return true;
}

bool ContainerIndex::Candidate(const u8* p, u32 len)
{
    return Magic(p, len, "NARC") || Magic(p, len, "SDAT") || LooksPacked(p, len);
}

void ContainerIndex::AddMembers(Container& c, u32 blob, u32 base, u32 len, int parent, int depth, u32 romBase)
{
    std::vector<ContainerEntry> entries;
    std::string err;
    const u8* p = Blobs[blob].data() + base;
    bool narc = Magic(p, len, "NARC");
    if (!(narc ? ReadNarc(p, len, entries, err) : ReadSdat(p, len, entries, err))) return;
    std::string prefix = parent >= 0 ? c.Members[parent].Name + "/" : std::string();
    for (u32 i = 0; i < entries.size(); i++)
    {
        const ContainerEntry& e = entries[i];
        ContainerMember m;
        m.Offset = romBase != ContainerMember::NoOffset ? romBase + e.Offset : ContainerMember::NoOffset;
        m.StoredSize = e.Size;
        m.Parent = parent;
        m.Depth = depth;
        m.Blob = blob;
        m.BlobOffset = base + e.Offset;
        m.Size = e.Size;
        const u8* d = Blobs[blob].data() + base + e.Offset;
        if (LooksPacked(d, e.Size))
        {
            std::vector<u8> out;
            std::string form;
            if (UnpackBios(d, e.Size, out, form) && Total + out.size() <= MaxTotal)
            {
                m.Form = form;
                m.Size = (u32)out.size();
                Total += out.size();
                m.Blob = (u32)Blobs.size();
                m.BlobOffset = 0;
                Blobs.push_back(std::move(out));
            }
        }
        m.Ok = true;
        m.Kind = KindOf(Content(m), m.Size);
        m.Name = prefix + (e.Name.empty() ? Numbered(i, ExtOf(m.Kind)) : e.Name);
        c.Members.push_back(m);
        int idx = (int)c.Members.size() - 1;
        if ((m.Kind == "NARC" || m.Kind == "SDAT") && depth + 1 <= MaxDepth)
        {
            size_t before = c.Members.size();
            AddMembers(c, m.Blob, m.BlobOffset, m.Size, idx, depth + 1, m.Form == "stored" ? m.Offset : ContainerMember::NoOffset);
            c.Members[idx].Expanded = c.Members.size() > before;
        }
    }
}

void ContainerIndex::Build(std::vector<Input> files)
{
    C.clear();
    Blobs.clear();
    Global.clear();
    Total = 0;
    Idx = ContentIndex();
    u32 unreadable = 0;
    for (Input& in : files)
    {
        u32 len = (u32)in.Bytes.size();
        Container c;
        c.FileId = in.FileId;
        c.Path = in.Path;
        c.RomStart = in.RomStart;
        c.RomSize = len;
        std::vector<u8> bytes;
        if (Magic(in.Bytes.data(), len, "NARC") || Magic(in.Bytes.data(), len, "SDAT"))
            bytes = std::move(in.Bytes);
        else
        {
            std::string form;
            if (!LooksPacked(in.Bytes.data(), len) || !UnpackBios(in.Bytes.data(), len, bytes, form)) continue;
            if (!Magic(bytes.data(), (u32)bytes.size(), "NARC") && !Magic(bytes.data(), (u32)bytes.size(), "SDAT")) continue;
            c.Packed = form;
        }
        if (Total + bytes.size() > MaxTotal) { unreadable++; continue; }
        c.Format = Magic(bytes.data(), (u32)bytes.size(), "NARC") ? "narc" : "sdat";
        c.Size = (u32)bytes.size();
        Total += bytes.size();
        u32 blob = (u32)Blobs.size();
        Blobs.push_back(std::move(bytes));
        AddMembers(c, blob, 0, c.Size, -1, 0, c.Packed.empty() ? 0 : ContainerMember::NoOffset);
        if (c.Members.empty())
        {
            // a header and nothing readable after it
            std::vector<ContainerEntry> e;
            std::string err;
            const u8* p = Blobs[blob].data();
            if (!(c.Format == "narc" ? ReadNarc(p, c.Size, e, err) : ReadSdat(p, c.Size, e, err))) { unreadable++; Total -= c.Size; Blobs.pop_back(); continue; }
        }
        C.push_back(std::move(c));
    }

    // the contents don't move from here on
    ById.clear();
    std::map<std::pair<u32, u64>, std::vector<int>> bySum;
    for (size_t ci = 0; ci < C.size(); ci++)
    {
        Container& c = C[ci];
        ById.push_back({c.FileId, (int)ci});
        c.FirstGlobal = (u32)Global.size();
        for (size_t mi = 0; mi < c.Members.size(); mi++)
        {
            ContainerMember& m = c.Members[mi];
            m.Global = (u32)Global.size();
            Global.push_back({(int)ci, (int)mi});
            if (m.Offset != ContainerMember::NoOffset) c.ByOffset.push_back((int)mi);
            if (m.Depth) c.Nested++;
            // a nested container's bytes are found through its files, so the files are named
            if (!m.Ok || m.Expanded || m.Size < ContentIndex::Window || m.Size > MaxIndexed) continue;
            auto& v = bySum[{m.Size, Fnv(Content(m), m.Size)}];
            for (int o : v)
            {
                auto [oc, om] = Global[o];
                if (!memcmp(Content(C[oc].Members[om]), Content(m), m.Size)) { m.DuplicateOf = o; break; }
            }
            if (m.DuplicateOf < 0) v.push_back((int)m.Global);
        }
        std::sort(c.ByOffset.begin(), c.ByOffset.end(), [&c](int a, int b)
        {
            const ContainerMember& x = c.Members[a];
            const ContainerMember& y = c.Members[b];
            return x.Offset != y.Offset ? x.Offset < y.Offset : x.Depth < y.Depth;
        });
    }
    std::sort(ById.begin(), ById.end());
    for (const Container& c : C)
        for (const ContainerMember& m : c.Members)
        {
            if (m.DuplicateOf >= 0) Idx.AddCopy(m.DuplicateOf, (int)m.Global);
            else if (m.Ok && !m.Expanded && m.Size >= ContentIndex::Window && m.Size <= MaxIndexed) Idx.Add((int)m.Global, Content(m), m.Size);
        }
    Idx.Finish();

    u32 narcs = 0, sdats = 0, packed = 0, nested = 0;
    for (const Container& c : C)
    {
        (c.Format == "narc" ? narcs : sdats)++;
        if (!c.Packed.empty()) packed++;
        nested += c.Nested;
    }
    Summary = std::to_string(narcs) + " NARC, " + std::to_string(sdats) + " SDAT";
    if (packed) Summary += " (" + std::to_string(packed) + " packed whole)";
    Summary += ": " + std::to_string(Global.size()) + " files";
    if (nested) Summary += ", " + std::to_string(nested) + " of them nested";
    Summary += " (" + std::to_string(Total >> 20) + " MB)";
    if (unreadable) Summary += ", " + std::to_string(unreadable) + " unreadable";
}

int ContainerIndex::IndexOf(u32 fileId) const
{
    auto it = std::lower_bound(ById.begin(), ById.end(), std::make_pair(fileId, -1));
    return it != ById.end() && it->first == fileId ? it->second : -1;
}

const Container* ContainerIndex::ByFile(u32 fileId) const
{
    int i = IndexOf(fileId);
    return i < 0 ? nullptr : &C[i];
}

int ContainerIndex::MemberAt(const Container& c, u32 off, std::string* part, u32* partOffset) const
{
    auto it = std::upper_bound(c.ByOffset.begin(), c.ByOffset.end(), off, [&c](u32 v, int i) { return v < c.Members[i].Offset; });
    if (it == c.ByOffset.begin()) return -1;
    // the last member starting at or before off; if it doesn't reach off, a container it is nested in may
    for (int i = *std::prev(it); i >= 0; i = c.Members[i].Parent)
    {
        const ContainerMember& m = c.Members[i];
        if (off >= m.Offset && off < m.Offset + m.StoredSize)
        {
            if (part) *part = m.Form == "stored" ? "stored data" : "packed data";
            if (partOffset) *partOffset = off - m.Offset;
            return i;
        }
    }
    return -1;
}

std::string ContainerIndex::Describe(u32 fileId, u32 off) const
{
    const Container* c = ByFile(fileId);
    if (!c) return std::string();
    std::string part;
    u32 po = 0;
    int i = MemberAt(*c, off, &part, &po);
    if (i < 0) return std::string();
    return c->Members[i].Name + ", " + part + " +" + Hex(po);
}

bool ContainerIndex::Find(const std::string& path, int& container, int& member) const
{
    std::string want = Lower(path);
    while (!want.empty() && want[0] == '/') want.erase(0, 1);
    size_t colon = want.find(':');
    if (colon == std::string::npos) return false;
    std::string file = want.substr(0, colon), name = want.substr(colon + 1);
    for (size_t ci = 0; ci < C.size(); ci++)
    {
        std::string p = Lower(C[ci].Path);
        while (!p.empty() && p[0] == '/') p.erase(0, 1);
        if (p != file) continue;
        for (size_t mi = 0; mi < C[ci].Members.size(); mi++)
            if (Lower(C[ci].Members[mi].Name) == name) { container = (int)ci; member = (int)mi; return true; }
    }
    return false;
}

std::string ContainerIndex::FullName(int container, int member) const
{
    if (container < 0 || (size_t)container >= C.size()) return std::string();
    const Container& c = C[container];
    if (member < 0 || (size_t)member >= c.Members.size()) return c.Path;
    return c.Path + ":" + c.Members[member].Name;
}

bool ContainerIndex::Inside(const Container& c, int outer, int inner) const
{
    if (outer < 0 || inner < 0 || (size_t)inner >= c.Members.size()) return false;
    for (int i = c.Members[inner].Parent; i >= 0; i = c.Members[i].Parent)
        if (i == outer) return true;
    return false;
}

std::pair<int, int> ContainerIndex::FromGlobal(int global) const
{
    if (global < 0 || (size_t)global >= Global.size()) return {-1, -1};
    return Global[global];
}

std::vector<Match> ContainerIndex::Locate(int container, int member, const u8* mem, u32 memBase, u32 memLen, size_t maxMatches) const
{
    if (container < 0 || (size_t)container >= C.size() || member < 0 || (size_t)member >= C[container].Members.size()) return {};
    const ContainerMember& m = C[container].Members[member];
    if (!m.Ok || !m.Size) return {};
    return ContentIndex::Find((int)m.Global, Content(m), m.Size, Idx.MinMatch, mem, memBase, memLen, maxMatches);
}

}
