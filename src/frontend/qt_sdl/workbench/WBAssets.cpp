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

#include "WBAssets.h"
#include "Workbench.h"
#include "NDS.h"
#include "GPU.h"

namespace WBAsset
{

namespace
{

std::string Lower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '\\') c = '/';
    }
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    return s;
}

bool GraphicsMagic(const u8* p, u32 len)
{
    if (len < 16) return false;
    static const char* magics[] = {"RLCN", "RGCN", "RCSN", "RECN", "BTX0", "BMD0"};
    for (const char* m : magics)
        if (!memcmp(p, m, 4)) return true;
    return false;
}

bool LooksPacked(const u8* p, u32 len)
{
    if (len < 8) return false;
    if (!memcmp(p, "LZ77", 4)) return true;
    u8 t = p[0];
    u32 size = p[1] | (p[2] << 8) | (p[3] << 16);
    return (t == 0x10 || t == 0x11 || t == 0x24 || t == 0x28 || t == 0x30) && size >= 16 && size <= (8u << 20);
}

// "/gfx.narc:bg/title.ncgr" -> ("/gfx.narc:bg", "title")
void Split(const std::string& path, std::string& folder, std::string& stem)
{
    std::string l = Lower(path);
    size_t cut = l.find_last_of("/:");
    folder = cut == std::string::npos ? std::string() : l.substr(0, cut);
    stem = cut == std::string::npos ? l : l.substr(cut + 1);
    size_t dot = stem.find('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
}

}

bool Catalog::Candidate(const u8* p, u32 len)
{
    return GraphicsMagic(p, len) || (LooksPacked(p, len) && len <= (4u << 20));
}

void Catalog::Build(std::vector<RomInput> rom, std::shared_ptr<const WBArc::ContainerIndex> containers,
                    std::shared_ptr<const WBArc::ArchiveIndex> archive)
{
    Cont = containers;
    Arch = archive;
    for (RomInput& in : rom)
    {
        File f;
        f.Path = in.Path;
        f.FileId = in.FileId;
        f.From = File::RomFile;
        if (!GraphicsMagic(in.Bytes.data(), (u32)in.Bytes.size()))
        {
            std::vector<u8> out;
            std::string form;
            if (!WBArc::UnpackBios(in.Bytes.data(), (u32)in.Bytes.size(), out, form) || !GraphicsMagic(out.data(), (u32)out.size()))
                continue;
            f.Form = form;
            in.Bytes.swap(out);
        }
        Owned.push_back(std::move(in.Bytes));
        f.Data = Owned.back().data();
        f.Size = (u32)Owned.back().size();
        F.push_back(f);
    }
    if (Cont)
        for (size_t ci = 0; ci < Cont->Containers().size(); ci++)
        {
            const WBArc::Container& c = Cont->Containers()[ci];
            for (size_t mi = 0; mi < c.Members.size(); mi++)
            {
                const WBArc::ContainerMember& m = c.Members[mi];
                if (!m.Ok || m.Size < 16) continue;
                const u8* p = Cont->Content(m);
                if (!GraphicsMagic(p, m.Size)) continue;
                File f;
                f.Path = Cont->FullName((int)ci, (int)mi);
                f.From = File::Container;
                f.FileId = c.FileId;
                f.ContainerIndex = (int)ci;
                f.Member = (int)mi;
                f.Global = (int)m.Global;
                f.Data = p;
                f.Size = m.Size;
                F.push_back(f);
            }
        }
    if (Arch)
        for (size_t i = 0; i < Arch->Members().size(); i++)
        {
            const WBArc::Member& m = Arch->Members()[i];
            if (!m.Ok || m.Size < 16) continue;
            const u8* p = Arch->Content(m);
            if (!GraphicsMagic(p, m.Size)) continue;
            File f;
            f.Path = Arch->ArchivePath + ":" + m.Path;
            f.From = File::Archive;
            f.FileId = Arch->ArchiveFileId;
            f.ArchiveMember = (int)i;
            f.Data = p;
            f.Size = m.Size;
            F.push_back(f);
        }
    size_t counts[6] = {0, 0, 0, 0, 0, 0};
    static const char* kinds[6] = {"NCLR", "NCGR", "NSCR", "NCER", "BTX0", "BMD0"};
    for (size_t i = 0; i < F.size(); i++)
    {
        F[i].Desc = WBGfx::Describe(F[i].Data, F[i].Size);
        F[i].Kind = F[i].Desc.Format;
        for (int k = 0; k < 6; k++)
            if (F[i].Kind == kinds[k]) counts[k]++;
        if (F[i].Size >= Idx.MinMatch) Idx.Add((int)i, F[i].Data, F[i].Size);
    }
    Idx.Finish();
    Summary = std::to_string(F.size()) + " graphics files";
    std::string parts;
    for (int k = 0; k < 6; k++)
        if (counts[k]) parts += (parts.empty() ? "" : ", ") + std::to_string(counts[k]) + " " + kinds[k];
    if (!parts.empty()) Summary += " (" + parts + ")";
}

int Catalog::Find(const std::string& path) const
{
    std::string want = Lower(path);
    for (size_t i = 0; i < F.size(); i++)
        if (Lower(F[i].Path) == want) return (int)i;
    return -1;
}

int Catalog::First(const char* kind) const
{
    for (size_t i = 0; i < F.size(); i++)
        if (F[i].Kind == kind) return (int)i;
    return -1;
}

int Catalog::Pair(int i, const char* kind, bool sameStemOnly) const
{
    if (i < 0 || i >= (int)F.size()) return -1;
    std::string folder, stem;
    Split(F[i].Path, folder, stem);
    int best = -1, bestScore = -1;
    for (size_t j = 0; j < F.size(); j++)
    {
        if ((int)j == i || F[j].Kind != kind) continue;
        std::string f2, s2;
        Split(F[j].Path, f2, s2);
        bool sameStem = s2 == stem;
        if (sameStemOnly && !(sameStem && f2 == folder)) continue;
        bool sameBox = F[j].From == F[i].From && F[j].ContainerIndex == F[i].ContainerIndex && (F[j].ArchiveMember < 0) == (F[i].ArchiveMember < 0);
        int score = (sameStem ? 4 : 0) + (f2 == folder ? 2 : 0) + (sameBox ? 1 : 0);
        if (score > bestScore) { best = (int)j; bestScore = score; }
    }
    return best;
}

std::vector<WBArc::Match> Catalog::Locate(int i, const u8* mem, u32 memBase, u32 memLen, size_t maxMatches) const
{
    if (i < 0 || i >= (int)F.size()) return {};
    return WBArc::ContentIndex::Find(i, F[i].Data, F[i].Size, 32, mem, memBase, memLen, maxMatches);
}

// ---------------------------------------------------------------- the emulator's memory

static bool MainRam(WB::Workbench& wb, const u8*& mem, u32& len)
{
    NDS* nds = wb.GetNDS();
    if (!nds || !nds->MainRAM) return false;
    mem = nds->MainRAM;
    len = nds->MainRAMMask + 1;
    return true;
}

std::vector<RamRun> RamRuns(WB::Workbench& wb, const Catalog& c, u32 addr, u32 size, size_t max)
{
    std::vector<RamRun> out;
    const u8* mem;
    u32 len;
    if ((addr >> 24) != 0x02 || !MainRam(wb, mem, len)) return out;
    u32 a = 0x02000000 + (addr & (len - 1));
    u32 n = std::min<u32>(std::max<u32>(size, 1), 0x02000000 + len - a);
    for (const WBArc::Match& m : c.Resolve(mem, 0x02000000, len, a, n, max))
        out.push_back({m.Addr, m.Size, m.Member, m.MemberOffset});
    return out;
}

std::vector<u8> ReadMainRam(WB::Workbench& wb, u32 addr, u32 size)
{
    const u8* mem;
    u32 len;
    if ((addr >> 24) != 0x02 || !MainRam(wb, mem, len)) return {};
    u32 off = addr & (len - 1);
    size = std::min<u32>(size, len - off);
    return std::vector<u8>(mem + off, mem + off + size);
}

std::vector<u8> ReadPaletteRam(WB::Workbench& wb)
{
    NDS* nds = wb.GetNDS();
    if (!nds) return {};
    return std::vector<u8>(nds->GPU.Palette, nds->GPU.Palette + sizeof(nds->GPU.Palette));
}

std::vector<std::string> FindInVideoMemory(WB::Workbench& wb, const File& f, const u8* data, u32 size)
{
    std::vector<std::string> out;
    NDS* nds = wb.GetNDS();
    if (!nds || !data) return out;
    static const char* bankNames = "ABCDEFGHI";
    // the first window of n bytes from off that is not all zero
    auto window = [&](u32 off, u32 end, u32 n, u32& at) -> bool
    {
        for (at = off; at + n <= std::min(end, size); at += n)
        {
            bool zero = true;
            for (u32 i = 0; i < n && zero; i++) zero = data[at + i] == 0;
            if (!zero) return true;
        }
        return false;
    };
    auto inBanks = [&](u32 at, u32 n, u32 back, const std::string& what)
    {
        for (int b = 0; b < 9 && out.size() < 8; b++)
        {
            const u8* bank = nds->GPU.VRAM[b];
            u32 blen = nds->GPU.VRAMMask[b] + 1;
            const u8* hit = std::search(bank, bank + blen, data + at, data + at + n);
            if (hit == bank + blen) continue;
            u32 o = (u32)(hit - bank);
            char buf[160];
            snprintf(buf, sizeof(buf), "%s in VRAM bank %c +0x%X", what.c_str(), bankNames[b], o >= back ? o - back : o);
            out.push_back(buf);
        }
    };
    const WBGfx::Description& d = f.Desc;
    u32 at;
    if (d.Format == "NCLR")
    {
        static const char* regions[4] = {"BG main", "OBJ main", "BG sub", "OBJ sub"};
        for (u32 p = 0; p * 32 < d.PalSize && out.size() < 8; p++)
        {
            u32 off = d.PalOffset + p * 32;
            if (off + 32 > size || !window(off, off + 32, 32, at)) continue;
            const u8* pal = nds->GPU.Palette;
            const u8* end = pal + sizeof(nds->GPU.Palette);
            for (const u8* hit = std::search(pal, end, data + off, data + off + 32); hit != end && out.size() < 8;
                 hit = std::search(hit + 2, end, data + off, data + off + 32))
            {
                u32 o = (u32)(hit - pal);
                if (o & 1) continue;
                char buf[128];
                snprintf(buf, sizeof(buf), "palette %u in palette RAM 0x%08X (%s, palette %u)", p, 0x05000000 + o, regions[o / 512], (o % 512) / 32);
                out.push_back(buf);
            }
            if (!out.empty()) break;
        }
        if (out.empty() && window(d.PalOffset, d.PalOffset + d.PalSize, 32, at)) inBanks(at, 32, at - d.PalOffset, "palettes (extended)");
    }
    else if (d.Format == "NCGR" && window(d.CharOffset, d.CharOffset + d.CharSize, 64, at))
        inBanks(at, 64, at - d.CharOffset, "tiles");
    else if (d.Format == "NSCR" && d.MapSize >= 8 && window(d.MapOffset, d.MapOffset + d.MapSize, std::min<u32>(32, d.MapSize & ~1u), at))
        inBanks(at, std::min<u32>(32, d.MapSize & ~1u), at - d.MapOffset, "map");
    else if (d.Format == "BTX0" || d.Format == "BMD0")
    {
        for (size_t i = 0; i < d.Textures.size() && i < 16 && out.size() < 8; i++)
        {
            const WBGfx::Texture& t = d.Textures[i];
            u32 n = std::min<u32>(32, t.Size);
            if (n >= 8 && window(t.Offset, t.Offset + t.Size, n, at)) inBanks(at, n, at - t.Offset, "texture '" + t.Name + "'");
        }
        for (size_t i = 0; i < d.TexPalettes.size() && i < 16 && out.size() < 8; i++)
        {
            const WBGfx::TexPalette& tp = d.TexPalettes[i];
            u32 n = std::min<u32>(16, tp.Size);
            if (n >= 8 && window(tp.Offset, tp.Offset + tp.Size, n, at)) inBanks(at, n, at - tp.Offset, "palette '" + tp.Name + "'");
        }
    }
    return out;
}

bool Load(WB::Workbench& wb, const Catalog* cat, const std::string& path, u32 addr, u32 size, Asset& out, std::string& error)
{
    out = Asset();
    if (!path.empty())
    {
        int i = cat ? cat->Find(path) : -1;
        if (i < 0) { error = cat ? "no graphics file " + path : "the graphics files are not indexed yet"; return false; }
        const File& f = cat->Files()[i];
        out.Bytes.assign(f.Data, f.Data + f.Size);
        out.Desc = f.Desc;
        out.File = i;
        out.Path = f.Path;
        return true;
    }
    if ((addr >> 24) != 0x02) { error = "give a graphics file's path, or an address in main RAM"; return false; }
    if (size == 0)
    {
        std::vector<RamRun> runs = cat ? RamRuns(wb, *cat, addr, 1, 4) : std::vector<RamRun>();
        for (const RamRun& r : runs)
        {
            if (addr < r.Addr || addr >= r.Addr + r.Size || r.File < 0) continue;
            const File& f = cat->Files()[r.File];
            u32 start = r.Addr - r.FileOffset;
            out.Bytes = ReadMainRam(wb, start, f.Size);
            out.Desc = WBGfx::Describe(out.Bytes.data(), (u32)out.Bytes.size());
            out.File = r.File;
            out.RamAddr = start;
            out.Path = f.Path;
            return true;
        }
        error = "no graphics file's copy holds that address; give a size to view the bytes as they are";
        return false;
    }
    out.Bytes = ReadMainRam(wb, addr, std::min<u32>(size, 4u << 20));
    if (out.Bytes.empty()) { error = "cannot read that range"; return false; }
    out.Desc = WBGfx::Describe(out.Bytes.data(), (u32)out.Bytes.size());
    out.RamAddr = addr;
    return true;
}

// ---------------------------------------------------------------- rendering

static std::vector<QRgb> Grey(int n)
{
    std::vector<QRgb> g;
    for (int i = 0; i < n; i++)
    {
        int v = n > 1 ? i * 255 / (n - 1) : 0;
        g.push_back(qRgb(v, v, v));
    }
    return g;
}

QImage Render(const Catalog* cat, const Asset& a, const RenderOptions& o, const std::vector<u8>& livePalette,
              RenderInfo& info, std::string& error)
{
    const WBGfx::Description& d = a.Desc;
    const u8* p = a.Bytes.data();
    u32 len = (u32)a.Bytes.size();
    std::string part = o.Part;
    if (part.empty())
        part = d.Format == "NCLR" ? "palette" : d.Format == "NCGR" ? "tiles" : d.Format == "NSCR" ? "map"
             : d.Format == "NCER" ? "cell" : (d.Format == "BTX0" || d.Format == "BMD0") ? "texture" : "raw";

    // the colours for tiles, maps and cells: a palette given, the same-named one, the one the game
    // has in palette RAM now, then the first one there is
    auto palette = [&](bool obj, int colors) -> std::vector<QRgb>
    {
        if ((o.PalettePath == "@bg" || o.PalettePath == "@obj") && livePalette.size() >= 2048)
        {
            bool o2 = o.PalettePath == "@obj";
            info.Palette = o2 ? "palette RAM (OBJ main)" : "palette RAM (BG main)";
            return WBGfx::Colors(livePalette.data(), (u32)livePalette.size(), o2 ? 512 : 0, 256);
        }
        if (cat && !o.PalettePath.empty() && o.PalettePath[0] != '@')
        {
            int i = cat->Find(o.PalettePath);
            if (i < 0 || cat->Files()[i].Kind != "NCLR") { error = "no palette " + o.PalettePath; return {}; }
            info.Palette = cat->Files()[i].Path;
            return WBGfx::PaletteColors(cat->Files()[i].Desc, cat->Files()[i].Data, cat->Files()[i].Size);
        }
        if (cat && a.File >= 0)
        {
            int i = cat->Pair(a.File, "NCLR", true);
            if (i >= 0)
            {
                info.Palette = cat->Files()[i].Path;
                return WBGfx::PaletteColors(cat->Files()[i].Desc, cat->Files()[i].Data, cat->Files()[i].Size);
            }
        }
        if (livePalette.size() >= 2048)
        {
            info.Palette = obj ? "palette RAM (OBJ main)" : "palette RAM (BG main)";
            return WBGfx::Colors(livePalette.data(), (u32)livePalette.size(), obj ? 512 : 0, 256);
        }
        if (cat)
        {
            int i = a.File >= 0 ? cat->Pair(a.File, "NCLR") : cat->First("NCLR");
            if (i >= 0)
            {
                info.Palette = cat->Files()[i].Path;
                return WBGfx::PaletteColors(cat->Files()[i].Desc, cat->Files()[i].Data, cat->Files()[i].Size);
            }
        }
        info.Palette = "greyscale";
        return Grey(colors);
    };
    auto tiles = [&]() -> const File*
    {
        if (!cat) return nullptr;
        int i = a.File >= 0 ? cat->Pair(a.File, "NCGR") : cat->First("NCGR");
        if (i < 0) return nullptr;
        info.Tiles = cat->Files()[i].Path;
        return &cat->Files()[i];
    };

    if (part == "raw")
    {
        std::vector<QRgb> pal = palette(false, o.Bpp == 2 ? 4 : o.Bpp == 4 ? 16 : 256);
        if (!error.empty()) return QImage();
        QImage img = WBGfx::DecodeRaw(p, len, o.Bpp, o.Tiled, o.Width, pal, o.PaletteOffset, false);
        if (img.isNull()) error = "nothing to show (bpp must be 2, 4, 8 or 16)";
        return img;
    }
    if (part == "palette")
    {
        std::vector<QRgb> c = d.Format == "NCLR" ? WBGfx::PaletteColors(d, p, len) : std::vector<QRgb>();
        if (c.empty() && (d.Format == "BTX0" || d.Format == "BMD0") && !d.TexPalettes.empty())
        {
            int i = std::clamp(o.Index, 0, (int)d.TexPalettes.size() - 1);
            c = WBGfx::Colors(p, len, d.TexPalettes[i].Offset, d.TexPalettes[i].Size / 2);
        }
        if (c.empty()) { error = "no palette in this " + (d.Format.empty() ? std::string("data") : d.Format); return QImage(); }
        return WBGfx::RenderPalette(c, 8);
    }
    if (part == "tiles")
    {
        if (d.Format != "NCGR") { error = "tiles need an NCGR"; return QImage(); }
        std::vector<QRgb> pal = palette(false, d.CharBpp == 4 ? 16 : 256);
        if (!error.empty()) return QImage();
        return WBGfx::RenderTiles(d, p, len, pal, std::max(0, o.Palette));
    }
    if (part == "map")
    {
        if (d.Format != "NSCR") { error = "a map needs an NSCR"; return QImage(); }
        const File* g = tiles();
        if (!g) { error = "no NCGR to draw this map with"; return QImage(); }
        std::vector<QRgb> pal = palette(false, 256);
        if (!error.empty()) return QImage();
        return WBGfx::RenderMap(d, p, len, g->Desc, g->Data, g->Size, pal);
    }
    if (part == "cell")
    {
        if (d.Format != "NCER") { error = "cells need an NCER"; return QImage(); }
        if (o.Index < 0 || o.Index >= (int)d.Cells.size()) { error = "no cell " + std::to_string(o.Index); return QImage(); }
        const File* g = tiles();
        if (!g) { error = "no NCGR to draw these cells with"; return QImage(); }
        std::vector<QRgb> pal = palette(true, 256);
        if (!error.empty()) return QImage();
        return WBGfx::RenderCell(d, o.Index, g->Desc, g->Data, g->Size, pal);
    }
    if (part == "texture")
    {
        if (d.Textures.empty()) { error = "no textures in this " + (d.Format.empty() ? std::string("data") : d.Format); return QImage(); }
        if (o.Index < 0 || o.Index >= (int)d.Textures.size()) { error = "no texture " + std::to_string(o.Index); return QImage(); }
        int pi = o.Palette >= 0 ? o.Palette : WBGfx::TexturePaletteFor(d, o.Index);
        if (pi >= 0 && pi < (int)d.TexPalettes.size()) info.Palette = d.TexPalettes[pi].Name;
        QImage img = WBGfx::RenderTexture(d, p, len, o.Index, pi);
        if (img.isNull()) error = "texture " + std::to_string(o.Index) + " cannot be drawn";
        return img;
    }
    error = "unknown part '" + part + "' (palette, tiles, map, cell, texture or raw)";
    return QImage();
}

}
