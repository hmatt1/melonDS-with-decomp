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
#include <cstdarg>
#include <cstdio>
#include <cstring>


#include "WBGfx.h"

namespace WBGfx
{

namespace
{

// bounds-checked little-endian reads: 0 past the end
struct Bytes
{
    const u8* P;
    u32 Len;
    bool Has(u32 off, u32 n) const { return off <= Len && n <= Len - off; }
    u32 U8(u32 o) const { return o < Len ? P[o] : 0; }
    u32 U16(u32 o) const { return Has(o, 2) ? (u32)P[o] | ((u32)P[o + 1] << 8) : 0; }
    u32 U32(u32 o) const { return Has(o, 4) ? U16(o) | (U16(o + 2) << 16) : 0; }
    std::string Magic(u32 o) const { return Has(o, 4) ? std::string((const char*)P + o, 4) : std::string(); }
};

std::string Fmt(const char* f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string Reversed(std::string s)
{
    std::reverse(s.begin(), s.end());
    return s;
}

void Stop(Description& d, u32 at, const std::string& why)
{
    if (d.UnreadableFrom == 0 || at < d.UnreadableFrom)
    {
        d.UnreadableFrom = std::max<u32>(at, 1);
        d.Problem = why;
    }
}

void AddPart(Description& d, u32 off, u32 size, const char* kind, int index, const std::string& label)
{
    if (size == 0 || off >= d.Size) return;
    size = std::min(size, d.Size - off);
    Part p;
    p.Offset = off;
    p.Size = size;
    p.Kind = kind;
    p.Index = index;
    p.Label = label;
    d.Parts.push_back(p);
}

int TexelBits(int format)
{
    static const int bits[8] = {0, 8, 2, 4, 8, 2, 8, 16};
    return bits[format & 7];
}

void ParsePltt(Description& d, const Bytes& b, u32 base, u32 end)
{
    // u32 depth (3: 4bpp, 4: 8bpp), u32 extended-palette flag, u32 data size, u32 data offset from +8
    u32 depth = b.U32(base + 8);
    u32 size = b.U32(base + 0x10);
    u32 rel = b.U32(base + 0x14);
    u32 start = rel ? base + 8 + rel : base + 0x18;
    if (rel > end || start >= end || start < base + 0x18) { Stop(d, base + 0x14, "PLTT's palette data offset is outside the section"); return; }
    u32 avail = end - start;
    if (size > avail) { Stop(d, end, Fmt("PLTT says 0x%X bytes of colours, the section holds 0x%X", size, avail)); size = avail; }
    d.PalOffset = start;
    d.PalSize = size & ~1u;
    d.PalBpp = depth == 4 ? 8 : 4;
    AddPart(d, base, start - base, "section", -1, "PLTT header");
    u32 per = d.PalBpp == 4 ? 32 : 512;
    for (u32 i = 0; i * per < d.PalSize && i < 4096; i++)
        AddPart(d, start + i * per, std::min(per, d.PalSize - i * per), "palette", (int)i, Fmt("palette %u", i));
}

void ParsePcmp(Description& d, const Bytes& b, u32 base, u32 end)
{
    u32 count = b.U16(base + 8);
    u32 rel = b.U32(base + 0x0C);
    u32 start = base + 8 + rel;
    AddPart(d, base, std::min<u32>(end - base, 0x10), "section", -1, "PCMP header");
    if (rel > end || start > end || count > (end - start) / 2) { Stop(d, base + 8, "PCMP's palette ids are outside the section"); return; }
    for (u32 i = 0; i < count; i++) d.PalIds.push_back((u16)b.U16(start + i * 2));
    AddPart(d, start, count * 2, "other", -1, "PCMP palette ids");
}

void ParseChar(Description& d, const Bytes& b, u32 base, u32 end)
{
    u32 ty = b.U16(base + 8), tx = b.U16(base + 0x0A);
    u32 depth = b.U32(base + 0x0C);
    d.Mapping = b.U32(base + 0x10);
    d.Linear = (b.U32(base + 0x14) & 1) != 0;
    u32 size = b.U32(base + 0x18);
    u32 rel = b.U32(base + 0x1C);
    u32 start = rel ? base + 8 + rel : base + 0x20;
    d.CharBpp = depth == 4 ? 8 : 4;
    d.TilesX = tx == 0xFFFF ? 0 : (int)tx;
    d.TilesY = ty == 0xFFFF ? 0 : (int)ty;
    if (rel > end || start >= end || start < base + 0x20) { Stop(d, base + 0x1C, "CHAR's tile data offset is outside the section"); return; }
    u32 avail = end - start;
    if (size > avail) { Stop(d, end, Fmt("CHAR says 0x%X bytes of tiles, the section holds 0x%X", size, avail)); size = avail; }
    d.CharOffset = start;
    d.CharSize = size;
    AddPart(d, base, start - base, "section", -1, "CHAR header");
    AddPart(d, start, size, "tiles", -1, d.Linear ? "bitmap" : Fmt("%u tiles, %dbpp", size / (8u * d.CharBpp), d.CharBpp));
}

void ParseScrn(Description& d, const Bytes& b, u32 base, u32 end)
{
    d.MapWidth = (int)b.U16(base + 8);
    d.MapHeight = (int)b.U16(base + 0x0A);
    d.MapColorMode = (int)b.U16(base + 0x0C);
    d.MapFormat = (int)b.U16(base + 0x0E);
    u32 size = b.U32(base + 0x10);
    u32 start = base + 0x14;
    if (start > end) { Stop(d, base + 0x10, "SCRN is too short"); return; }
    u32 avail = end - start;
    if (size > avail) { Stop(d, end, Fmt("SCRN says 0x%X bytes of map, the section holds 0x%X", size, avail)); size = avail; }
    d.MapOffset = start;
    d.MapSize = size;
    AddPart(d, base, 0x14, "section", -1, "SCRN header");
    AddPart(d, start, size, "map", -1, Fmt("%dx%d map", d.MapWidth, d.MapHeight));
}

void DecodeOam(Oam& o)
{
    static const int dims[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };
    u16 a0 = o.Attr[0], a1 = o.Attr[1], a2 = o.Attr[2];
    o.Y = (s8)(a0 & 0xFF);
    o.Affine = (a0 >> 8) & 1;
    o.Bpp8 = (a0 >> 13) & 1;
    int shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
    if (shape == 3) shape = 0;
    o.Width = dims[shape][size][0];
    o.Height = dims[shape][size][1];
    o.X = a1 & 0x1FF;
    if (o.X >= 0x100) o.X -= 0x200;
    o.HFlip = !o.Affine && ((a1 >> 12) & 1);
    o.VFlip = !o.Affine && ((a1 >> 13) & 1);
    o.Tile = a2 & 0x3FF;
    o.Palette = (a2 >> 12) & 0xF;
}

void ParseCebk(Description& d, const Bytes& b, u32 base, u32 end)
{
    u32 n = b.U16(base + 8);
    u32 attr = b.U16(base + 0x0A);
    u32 rel = b.U32(base + 0x0C);
    d.CellMapping = b.U32(base + 0x10);
    u32 table = base + 8 + rel;
    u32 entry = attr == 1 ? 16 : 8;
    if (rel > end || table > end || n > 4096 || n * entry > end - table) { Stop(d, base + 8, "CEBK's cell table is outside the section"); return; }
    AddPart(d, base, table - base, "section", -1, "CEBK header");
    u32 oamBase = table + n * entry;
    u32 oamEnd = oamBase;
    for (u32 i = 0; i < n; i++)
    {
        u32 e = table + i * entry;
        Cell c;
        c.Offset = e;
        u32 count = b.U16(e);
        u32 oamOff = b.U32(e + 4);
        c.OamOffset = oamBase + oamOff;
        AddPart(d, e, entry, "cells", (int)i, Fmt("cell %u", i));
        if (oamOff > end || c.OamOffset > end || count > 128 || count * 6 > end - c.OamOffset)
        {
            Stop(d, e, Fmt("cell %u's OAM entries are outside the section", i));
            d.Cells.push_back(c);
            continue;
        }
        for (u32 k = 0; k < count; k++)
        {
            Oam o;
            for (int a = 0; a < 3; a++) o.Attr[a] = (u16)b.U16(c.OamOffset + k * 6 + a * 2);
            DecodeOam(o);
            c.Oams.push_back(o);
        }
        AddPart(d, c.OamOffset, count * 6, "oam", (int)i, Fmt("cell %u OAM", i));
        oamEnd = std::max(oamEnd, c.OamOffset + count * 6);
        d.Cells.push_back(c);
    }
}

// a TEX0 dictionary: names and fixed-size entries. Returns the entries' start, or 0
u32 ParseDict(Description& d, const Bytes& b, u32 at, u32 end, u32 wantEntry, std::vector<std::string>& names, u32& entrySize, const char* what)
{
    // u8 0, u8 count, u16 size; an 8-byte lookup tree header and count + 1 four-byte nodes; then
    // u16 entry size, u16 entries' size, the entries, and a 16-byte name for each
    u32 num = b.U8(at + 1);
    u32 info = at + 12 + 4 * num;
    if (!b.Has(at, 12) || info + 4 > end) { Stop(d, at, Fmt("the %s dictionary is unreadable", what)); return 0; }
    entrySize = b.U16(info);
    if (entrySize < wantEntry || entrySize > 64) { Stop(d, info, Fmt("the %s dictionary's entry size is %u", what, entrySize)); return 0; }
    u32 entries = info + 4;
    u32 namesAt = entries + num * entrySize;
    if (namesAt > end || num * 16 > end - namesAt) { Stop(d, at, Fmt("the %s dictionary runs past the section", what)); return 0; }
    for (u32 i = 0; i < num; i++)
    {
        const char* s = (const char*)b.P + namesAt + i * 16;
        names.push_back(std::string(s, strnlen(s, 16)));
    }
    AddPart(d, at, namesAt + num * 16 - at, "dictionary", -1, Fmt("%s dictionary (%u)", what, num));
    return entries;
}

void ParseTex0(Description& d, const Bytes& b, u32 base, u32 end)
{
    d.Tex0Offset = base;
    if (end - base < 0x3C) { Stop(d, base, "TEX0 is too short"); return; }
    AddPart(d, base, 0x3C, "section", -1, "TEX0 header");
    u32 texDict = b.U16(base + 0x0E);
    u32 texData = b.U32(base + 0x14), texDataSize = b.U16(base + 0x0C) << 3;
    u32 cmpData = b.U32(base + 0x24), cmpIndex = b.U32(base + 0x28), cmpSize = b.U16(base + 0x1C) << 3;
    u32 palDataSize = b.U32(base + 0x30) << 3, palDict = b.U32(base + 0x34), palData = b.U32(base + 0x38);
    auto inside = [&](u32 off, u32 size) { return off <= end - base && size <= end - base - off; };

    std::vector<std::string> names;
    u32 esize = 0;
    if (texDict && inside(texDict, 12))
        if (u32 entries = ParseDict(d, b, base + texDict, end, 8, names, esize, "texture"))
            for (size_t i = 0; i < names.size(); i++)
            {
                Texture t;
                t.Name = names[i];
                t.Param = b.U32(entries + (u32)i * esize);
                t.Format = (t.Param >> 26) & 7;
                t.Width = 8 << ((t.Param >> 20) & 7);
                t.Height = 8 << ((t.Param >> 23) & 7);
                t.Color0Transparent = (t.Param >> 29) & 1;
                u32 off = (t.Param & 0xFFFF) << 3;
                t.Size = (u32)((u64)t.Width * t.Height * TexelBits(t.Format) / 8);
                std::string label = Fmt("texture '%s' (%s %dx%d)", t.Name.c_str(), TextureFormatName(t.Format), t.Width, t.Height);
                if (t.Format == 5)
                {
                    t.Offset = base + cmpData + off;
                    t.IndexOffset = base + cmpIndex + off / 2;
                    t.IndexSize = t.Size / 2;
                    if (!inside(cmpData + off, t.Size) || !inside(cmpIndex + off / 2, t.IndexSize))
                        Stop(d, base + texDict, Fmt("texture '%s' is outside TEX0", t.Name.c_str()));
                    else
                    {
                        AddPart(d, t.Offset, t.Size, "texture_4x4", (int)i, label);
                        AddPart(d, t.IndexOffset, t.IndexSize, "texture_index", (int)i, label + " palette indexes");
                    }
                }
                else
                {
                    t.Offset = base + texData + off;
                    if (t.Format == 0 || !inside(texData + off, t.Size))
                        Stop(d, base + texDict, Fmt("texture '%s' is outside TEX0", t.Name.c_str()));
                    else AddPart(d, t.Offset, t.Size, "texture", (int)i, label);
                }
                d.Textures.push_back(t);
            }
    (void)texDataSize;
    (void)cmpSize;
    names.clear();
    if (palDict && inside(palDict, 12))
        if (u32 entries = ParseDict(d, b, base + palDict, end, 4, names, esize, "palette"))
        {
            std::vector<u32> offs;
            for (size_t i = 0; i < names.size(); i++) offs.push_back(b.U16(entries + (u32)i * esize) << 3);
            u32 dataEnd = inside(palData, palDataSize) ? palData + palDataSize : end - base;
            for (size_t i = 0; i < names.size(); i++)
            {
                TexPalette tp;
                tp.Name = names[i];
                u32 next = dataEnd;
                for (u32 o : offs)
                    if (palData + o > palData + offs[i] && palData + o < next) next = palData + o;
                tp.Offset = base + palData + offs[i];
                tp.Size = palData + offs[i] < next ? next - palData - offs[i] : 0;
                if (!inside(palData + offs[i], tp.Size)) { Stop(d, base + palDict, Fmt("palette '%s' is outside TEX0", tp.Name.c_str())); tp.Size = 0; }
                else AddPart(d, tp.Offset, tp.Size, "tex_palette", (int)i, Fmt("palette '%s'", tp.Name.c_str()));
                d.TexPalettes.push_back(tp);
            }
        }
}

}

const char* TextureFormatName(int format)
{
    static const char* names[8] = {"none", "A3I5", "4-colour", "16-colour", "256-colour", "4x4", "A5I3", "direct"};
    return names[format & 7];
}

Description Describe(const u8* p, u32 len)
{
    Description d;
    d.Size = len;
    Bytes b{p, len};
    std::string raw = b.Magic(0);
    if (raw == "RLCN") d.Format = "NCLR";
    else if (raw == "RGCN") d.Format = "NCGR";
    else if (raw == "RCSN") d.Format = "NSCR";
    else if (raw == "RECN") d.Format = "NCER";
    else if (raw == "BTX0" || raw == "BMD0") d.Format = raw;
    else return d;
    if (len < 16) { Stop(d, 0, "shorter than a Nitro file header"); return d; }
    u32 hsize = b.U16(12), nsec = b.U16(14), fsize = b.U32(8);
    AddPart(d, 0, std::max<u32>(16, std::min<u32>(hsize, 0x40)), "header", -1, d.Format + " header");
    if (b.U16(4) != 0xFEFF) Stop(d, 4, "the byte order mark is not 0xFEFF");
    u32 limit = len;
    if (fsize >= 16 && fsize < len) limit = fsize;
    if (fsize > len) Stop(d, len, Fmt("the header says 0x%X bytes; only 0x%X are here", fsize, len));
    if (hsize < 16 || hsize > limit) { Stop(d, 12, "the header size is wrong"); return d; }
    if (nsec > 64) { Stop(d, 14, Fmt("%u sections", nsec)); return d; }

    if (d.Format == "BTX0" || d.Format == "BMD0")
    {
        for (u32 i = 0; i < nsec; i++)
        {
            u32 so = b.U32(16 + i * 4);
            if (!b.Has(16 + i * 4, 4) || so < 16 || so + 8 > limit) { Stop(d, 16 + i * 4, Fmt("section %u's offset is outside the file", i)); continue; }
            std::string m = b.Magic(so);
            u32 ssize = b.U32(so + 4);
            u32 end = ssize >= 8 && ssize <= limit - so ? so + ssize : limit;
            if (end == limit && ssize != limit - so) Stop(d, so + 4, m + "'s size runs past the file");
            if (m == "TEX0") ParseTex0(d, b, so, end);
            else AddPart(d, so, end - so, "section", -1, m == "MDL0" ? std::string("MDL0 (models; not decoded)") : m);
        }
        return d;
    }

    u32 off = hsize;
    for (u32 i = 0; i < nsec; i++)
    {
        if (off + 8 > limit) { Stop(d, off, Fmt("section %u is missing", i)); break; }
        std::string m = Reversed(b.Magic(off));
        u32 ssize = b.U32(off + 4);
        if (ssize < 8 || ssize > limit - off)
        {
            Stop(d, off + 4, m + "'s size runs past the file");
            ssize = limit - off;
        }
        u32 end = off + ssize;
        if (m == "PLTT") ParsePltt(d, b, off, end);
        else if (m == "PCMP") ParsePcmp(d, b, off, end);
        else if (m == "CHAR") ParseChar(d, b, off, end);
        else if (m == "SCRN") ParseScrn(d, b, off, end);
        else if (m == "CEBK") ParseCebk(d, b, off, end);
        else AddPart(d, off, ssize, "other", -1, m);
        off = end;
    }
    return d;
}

// ---------------------------------------------------------------- decoding

static int Up5(int v) { return (v << 3) | (v >> 2); }

QRgb Bgr555(u16 c, bool opaque)
{
    return qRgba(Up5(c & 31), Up5((c >> 5) & 31), Up5((c >> 10) & 31), opaque ? 255 : 0);
}

std::vector<QRgb> Colors(const u8* p, u32 len, u32 offset, u32 count)
{
    Bytes b{p, len};
    std::vector<QRgb> out;
    for (u32 i = 0; i < count && b.Has(offset + i * 2, 2); i++) out.push_back(Bgr555((u16)b.U16(offset + i * 2)));
    return out;
}

std::vector<QRgb> PaletteColors(const Description& d, const u8* p, u32 len)
{
    if (d.Format != "NCLR") return {};
    return Colors(p, len, d.PalOffset, d.PalSize / 2);
}

QImage RenderPalette(const std::vector<QRgb>& colors, int cell)
{
    if (colors.empty()) return QImage();
    int rows = ((int)colors.size() + 15) / 16;
    cell = std::max(1, std::min(cell, 64));
    QImage img(16 * cell, rows * cell, QImage::Format_ARGB32);
    img.fill(0);
    for (int i = 0; i < (int)colors.size(); i++)
        for (int y = 0; y < cell; y++)
            for (int x = 0; x < cell; x++) img.setPixel((i % 16) * cell + x, (i / 16) * cell + y, colors[i]);
    return img;
}

static QRgb PalAt(const std::vector<QRgb>& pal, int i)
{
    if (i >= 0 && i < (int)pal.size()) return pal[i];
    // past the palette: grey ramps, so the shapes still show
    int v = (i * 37) & 0xFF;
    return qRgb(v, v, v);
}

QImage DecodeRaw(const u8* data, u32 size, int bpp, bool tiled, int widthPx, const std::vector<QRgb>& pal, int palOffset, bool transparent0)
{
    if (!data || !size || (bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16)) return QImage();
    widthPx = std::max(8, std::min(widthPx, 1024));
    if (tiled) widthPx &= ~7;
    u64 pixels = (u64)size * 8 / bpp;
    int height = (int)std::min<u64>((pixels + widthPx - 1) / widthPx, 4096);
    if (tiled) height = (height + 7) & ~7;
    QImage img(widthPx, std::max(height, 1), QImage::Format_ARGB32);
    img.fill(0);
    Bytes b{data, size};
    int tilesPerRow = widthPx / 8;
    for (u64 i = 0; i < pixels && i < (u64)widthPx * height; i++)
    {
        int x, y;
        if (tiled)
        {
            u64 t = i / 64, in = i % 64;
            x = (int)((t % tilesPerRow) * 8 + in % 8);
            y = (int)((t / tilesPerRow) * 8 + in / 8);
        }
        else { x = (int)(i % widthPx); y = (int)(i / widthPx); }
        if (y >= height) break;
        QRgb c;
        if (bpp == 16) c = Bgr555((u16)b.U16((u32)i * 2));
        else
        {
            u32 bit = (u32)(i * bpp);
            int v = (b.U8(bit / 8) >> (bit % 8)) & ((1 << bpp) - 1);
            if (v == 0 && transparent0) continue;
            c = PalAt(pal, palOffset + v);
        }
        img.setPixel(x, y, c);
    }
    return img;
}

QImage RenderTiles(const Description& g, const u8* p, u32 len, const std::vector<QRgb>& pal, int palIndex, int widthTiles)
{
    if (g.Format != "NCGR" || !g.CharSize || g.CharOffset >= len) return QImage();
    u32 size = std::min(g.CharSize, len - g.CharOffset);
    u32 tiles = size / (8u * g.CharBpp);
    int w = widthTiles > 0 ? widthTiles : g.TilesX > 0 ? g.TilesX : (int)std::min<u32>(32, std::max<u32>(tiles, 1));
    int off = g.CharBpp == 4 ? palIndex * 16 : 0;
    return DecodeRaw(p + g.CharOffset, size, g.CharBpp, !g.Linear, w * 8, pal, off, false);
}

bool TileBytes(const Description& g, int tile, u32& offset, u32& size)
{
    if (g.Format != "NCGR" || tile < 0 || g.Linear) return false;
    size = 8u * g.CharBpp;
    if ((u64)(tile + 1) * size > g.CharSize) return false;
    offset = g.CharOffset + (u32)tile * size;
    return true;
}

QImage RenderTile(const Description& g, const u8* p, u32 len, int tile, const std::vector<QRgb>& pal, int palIndex)
{
    u32 off, size;
    if (!TileBytes(g, tile, off, size) || off + size > len) return QImage();
    return DecodeRaw(p + off, size, g.CharBpp, true, 8, pal, g.CharBpp == 4 ? palIndex * 16 : 0, false);
}

// one 8x8 character into img at (x, y)
static void DrawChar(QImage& img, int x, int y, const u8* p, u32 len, u32 off, int bpp, bool hflip, bool vflip,
                     const std::vector<QRgb>& pal, int palOffset)
{
    Bytes b{p, len};
    for (int py = 0; py < 8; py++)
        for (int px = 0; px < 8; px++)
        {
            int i = py * 8 + px;
            u32 bit = (u32)(i * bpp);
            int v = (b.U8(off + bit / 8) >> (bit % 8)) & ((1 << bpp) - 1);
            if (v == 0) continue;
            int dx = x + (hflip ? 7 - px : px), dy = y + (vflip ? 7 - py : py);
            if (dx >= 0 && dy >= 0 && dx < img.width() && dy < img.height()) img.setPixel(dx, dy, PalAt(pal, palOffset + v));
        }
}

QImage RenderMap(const Description& s, const u8* sp, u32 slen, const Description& g, const u8* gp, u32 glen, const std::vector<QRgb>& pal)
{
    if (s.Format != "NSCR" || g.Format != "NCGR" || s.MapWidth < 8 || s.MapHeight < 8) return QImage();
    int w = std::min(s.MapWidth, 2048), h = std::min(s.MapHeight, 2048);
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(0);
    Bytes b{sp, slen};
    int cols = w / 8;
    bool affine = s.MapFormat == 1;
    u32 entries = affine ? s.MapSize : s.MapSize / 2;
    u32 tileBytes = 8u * g.CharBpp;
    for (u32 k = 0; k < entries && (int)(k / cols) * 8 < h; k++)
    {
        u32 e = affine ? b.U8(s.MapOffset + k) : b.U16(s.MapOffset + k * 2);
        u32 tile = affine ? e : e & 0x3FF;
        bool hf = !affine && (e >> 10) & 1, vf = !affine && (e >> 11) & 1;
        int palNum = affine ? 0 : (int)(e >> 12);
        u32 off = g.CharOffset + tile * tileBytes;
        if ((u64)tile * tileBytes + tileBytes > g.CharSize) continue;
        int po = g.CharBpp == 4 ? palNum * 16 : (s.MapColorMode == 2 ? palNum * 256 : 0);
        DrawChar(img, (int)(k % cols) * 8, (int)(k / cols) * 8, gp, glen, off, g.CharBpp, hf, vf, pal, po);
    }
    return img;
}

QImage RenderCell(const Description& c, int cell, const Description& g, const u8* gp, u32 glen, const std::vector<QRgb>& pal, QPoint* origin)
{
    if (c.Format != "NCER" || g.Format != "NCGR" || cell < 0 || cell >= (int)c.Cells.size()) return QImage();
    const Cell& ce = c.Cells[cell];
    if (ce.Oams.empty()) return QImage();
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    for (const Oam& o : ce.Oams)
    {
        x0 = std::min(x0, o.X); y0 = std::min(y0, o.Y);
        x1 = std::max(x1, o.X + o.Width); y1 = std::max(y1, o.Y + o.Height);
    }
    QImage img(x1 - x0, y1 - y0, QImage::Format_ARGB32);
    img.fill(0);
    u32 shift = c.CellMapping >= 1 && c.CellMapping <= 4 ? c.CellMapping - 1 : 0;
    bool oneD = c.CellMapping >= 1;
    // the first OAM entry is drawn on top
    for (int k = (int)ce.Oams.size() - 1; k >= 0; k--)
    {
        const Oam& o = ce.Oams[k];
        int bpp = o.Bpp8 ? 8 : 4;
        u32 tileBytes = 8u * bpp;
        int tw = o.Width / 8, th = o.Height / 8;
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++)
            {
                u32 rel = oneD ? ((u32)o.Tile << (5 + shift)) + (u32)(ty * tw + tx) * tileBytes
                               : ((u32)o.Tile + (u32)ty * 32 + (u32)tx * (bpp / 4)) * 32;
                if (rel + tileBytes > g.CharSize) continue;
                int dx = o.X - x0 + (o.HFlip ? (tw - 1 - tx) : tx) * 8;
                int dy = o.Y - y0 + (o.VFlip ? (th - 1 - ty) : ty) * 8;
                DrawChar(img, dx, dy, gp, glen, g.CharOffset + rel, bpp, o.HFlip, o.VFlip, pal, bpp == 4 ? o.Palette * 16 : 0);
            }
    }
    if (origin) *origin = QPoint(-x0, -y0);
    return img;
}

int TexturePaletteFor(const Description& d, int texture)
{
    if (d.TexPalettes.empty()) return -1;
    if (texture >= 0 && texture < (int)d.Textures.size())
    {
        const std::string& n = d.Textures[texture].Name;
        for (size_t i = 0; i < d.TexPalettes.size(); i++)
            if (d.TexPalettes[i].Name == n || d.TexPalettes[i].Name == n + "_pl") return (int)i;
    }
    return 0;
}

bool TexelBytes(const Description& d, int texture, int x, int y, u32& offset, u32& size)
{
    if (texture < 0 || texture >= (int)d.Textures.size()) return false;
    const Texture& t = d.Textures[texture];
    if (x < 0 || y < 0 || x >= t.Width || y >= t.Height || !t.Format) return false;
    if (t.Format == 5)
    {
        offset = t.Offset + (u32)((y / 4) * (t.Width / 4) + x / 4) * 4;
        size = 4;
        return true;
    }
    int bits = TexelBits(t.Format);
    offset = t.Offset + (u32)(((u64)y * t.Width + x) * bits / 8);
    size = std::max(1, bits / 8);
    return true;
}

QImage RenderTexture(const Description& d, const u8* p, u32 len, int texture, int palette)
{
    if (texture < 0 || texture >= (int)d.Textures.size()) return QImage();
    const Texture& t = d.Textures[texture];
    if (!t.Format || t.Width > 1024 || t.Height > 1024) return QImage();
    if (palette < 0) palette = TexturePaletteFor(d, texture);
    std::vector<QRgb> pal;
    u32 palOff = 0, palSize = 0;
    if (palette >= 0 && palette < (int)d.TexPalettes.size())
    {
        palOff = d.TexPalettes[palette].Offset;
        palSize = d.TexPalettes[palette].Size;
        pal = Colors(p, len, palOff, palSize / 2);
    }
    Bytes b{p, len};
    QImage img(t.Width, t.Height, QImage::Format_ARGB32);
    img.fill(0);
    auto withAlpha = [](QRgb c, int a) { return qRgba(qRed(c), qGreen(c), qBlue(c), a); };
    if (t.Format == 5)
    {
        int bw = t.Width / 4;
        for (int by = 0; by < t.Height / 4; by++)
            for (int bx = 0; bx < bw; bx++)
            {
                u32 k = (u32)(by * bw + bx);
                u32 texels = b.U32(t.Offset + k * 4);
                u32 idx = b.U16(t.IndexOffset + k * 2);
                u32 base = palOff + (idx & 0x3FFF) * 4;
                int mode = idx >> 14;
                QRgb c[4];
                for (int i = 0; i < 4; i++) c[i] = Bgr555((u16)b.U16(base + i * 2));
                auto mix = [](QRgb a, QRgb b2, int wa, int wb, int div)
                {
                    return qRgb((qRed(a) * wa + qRed(b2) * wb) / div, (qGreen(a) * wa + qGreen(b2) * wb) / div,
                                (qBlue(a) * wa + qBlue(b2) * wb) / div);
                };
                bool transparent3 = mode == 0 || mode == 1;
                if (mode == 1) c[2] = mix(c[0], c[1], 1, 1, 2);
                if (mode == 3) { c[2] = mix(c[0], c[1], 5, 3, 8); c[3] = mix(c[0], c[1], 3, 5, 8); }
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                    {
                        int v = (texels >> ((y * 4 + x) * 2)) & 3;
                        if (v == 3 && transparent3) continue;
                        img.setPixel(bx * 4 + x, by * 4 + y, c[v]);
                    }
            }
        return img;
    }
    for (int y = 0; y < t.Height; y++)
        for (int x = 0; x < t.Width; x++)
        {
            u32 i = (u32)(y * t.Width + x);
            QRgb c = 0;
            switch (t.Format)
            {
            case 1: { u32 v = b.U8(t.Offset + i); int a = (int)(v >> 5); c = withAlpha(PalAt(pal, v & 31), Up5(a * 4 + a / 2)); break; }
            case 2: { int v = (b.U8(t.Offset + i / 4) >> ((i % 4) * 2)) & 3; if (v == 0 && t.Color0Transparent) continue; c = PalAt(pal, v); break; }
            case 3: { int v = (b.U8(t.Offset + i / 2) >> ((i % 2) * 4)) & 15; if (v == 0 && t.Color0Transparent) continue; c = PalAt(pal, v); break; }
            case 4: { int v = (int)b.U8(t.Offset + i); if (v == 0 && t.Color0Transparent) continue; c = PalAt(pal, v); break; }
            case 6: { u32 v = b.U8(t.Offset + i); c = withAlpha(PalAt(pal, v & 7), Up5(v >> 3)); break; }
            case 7: { u32 v = b.U16(t.Offset + i * 2); c = Bgr555((u16)v, (v >> 15) != 0); break; }
            }
            img.setPixel(x, y, c);
        }
    return img;
}

// ---------------------------------------------------------------- what a byte is

FieldInfo DescribeAt(const Description& d, const u8* p, u32 len, u32 off)
{
    FieldInfo f;
    if (!d.IsGraphics() || off >= d.Size) return f;
    Bytes b{p, len};
    if (d.UnreadableFrom && off >= d.UnreadableFrom)
    {
        f.Part = d.Format;
        f.Text = Fmt("%s: unreadable from +0x%X (%s)", d.Format.c_str(), d.UnreadableFrom, d.Problem.c_str());
    }
    if (off < 16)
    {
        static const struct { u32 End; const char* Name; } fields[] = {
            {4, "magic"}, {6, "byte order mark"}, {8, "version"}, {12, "file size"}, {14, "header size"}, {16, "section count"}};
        for (auto& fl : fields)
            if (off < fl.End) { f.Part = d.Format + " header"; f.Text = d.Format + " header, " + fl.Name; return f; }
    }
    if (d.Format == "NCLR" && off >= d.PalOffset && off < d.PalOffset + d.PalSize)
    {
        u32 i = (off - d.PalOffset) / 2;
        u32 per = d.PalBpp == 4 ? 16 : 256;
        f.Part = "PLTT";
        f.HasColor = true;
        f.Color = (u16)b.U16(d.PalOffset + i * 2);
        f.Palette = (int)(i / per);
        QRgb c = Bgr555(f.Color);
        f.Text = Fmt("NCLR palette %u, colour %u: 0x%04X = #%02X%02X%02X", i / per, i % per, f.Color, qRed(c), qGreen(c), qBlue(c));
        return f;
    }
    if (d.Format == "NCGR" && off >= d.CharOffset && off < d.CharOffset + d.CharSize)
    {
        u32 rel = off - d.CharOffset;
        f.Part = "CHAR";
        if (d.Linear)
        {
            int w = d.TilesX > 0 ? d.TilesX * 8 : 256;
            u32 px = rel * 8 / d.CharBpp;
            f.Text = Fmt("NCGR CHAR (bitmap), pixel (%u, %u)", px % w, px / w);
        }
        else
        {
            u32 tb = 8u * d.CharBpp;
            f.Tile = (int)(rel / tb);
            f.Text = Fmt("NCGR CHAR, tile %u, row %u", rel / tb, (rel % tb) / d.CharBpp);
        }
        return f;
    }
    if (d.Format == "NSCR" && off >= d.MapOffset && off < d.MapOffset + d.MapSize)
    {
        u32 rel = off - d.MapOffset;
        int cols = std::max(1, d.MapWidth / 8);
        f.Part = "SCRN";
        if (d.MapFormat == 1)
        {
            f.Tile = (int)b.U8(off);
            f.Text = Fmt("NSCR entry (%u,%u): tile 0x%X", rel % cols, rel / cols, f.Tile);
            return f;
        }
        u32 k = rel / 2;
        u32 e = b.U16(d.MapOffset + k * 2);
        f.Tile = (int)(e & 0x3FF);
        f.Palette = (int)(e >> 12);
        std::string flags;
        if (e & 0x400) flags += ", H-flip";
        if (e & 0x800) flags += ", V-flip";
        f.Text = Fmt("NSCR entry (%u,%u): tile 0x%X%s, palette %u", k % cols, k / cols, e & 0x3FF, flags.c_str(), e >> 12);
        return f;
    }
    if (d.Format == "NCER")
        for (size_t i = 0; i < d.Cells.size(); i++)
        {
            const Cell& c = d.Cells[i];
            u32 oamEnd = c.OamOffset + (u32)c.Oams.size() * 6;
            if (off >= c.OamOffset && off < oamEnd)
            {
                u32 k = (off - c.OamOffset) / 6, a = (off - c.OamOffset) % 6 / 2;
                const Oam& o = c.Oams[k];
                f.Part = "CEBK";
                f.Tile = o.Tile;
                f.Palette = o.Palette;
                f.Text = Fmt("NCER cell %u, OAM %u, attr%u: x %d, y %d, %dx%d, tile 0x%X, palette %d%s%s", (u32)i, k, a, o.X, o.Y,
                             o.Width, o.Height, o.Tile, o.Palette, o.HFlip ? ", H-flip" : "", o.VFlip ? ", V-flip" : "");
                return f;
            }
            if (off >= c.Offset && off < c.Offset + 8)
            {
                f.Part = "CEBK";
                f.Text = Fmt("NCER cell %u: %u OAM entries", (u32)i, (u32)c.Oams.size());
                return f;
            }
        }
    for (size_t i = 0; i < d.Textures.size(); i++)
    {
        const Texture& t = d.Textures[i];
        if (off >= t.Offset && off < t.Offset + t.Size)
        {
            f.Part = "TEX0";
            f.Texture = (int)i;
            u32 rel = off - t.Offset;
            if (t.Format == 5)
            {
                u32 k = rel / 4;
                int bw = std::max(1, t.Width / 4);
                f.Text = Fmt("TEX0 texture '%s' (4x4 %dx%d), block (%u, %u)", t.Name.c_str(), t.Width, t.Height, k % bw * 4, k / bw * 4);
            }
            else
            {
                u32 px = (u32)((u64)rel * 8 / TexelBits(t.Format));
                f.Text = Fmt("TEX0 texture '%s' (%s %dx%d), texel (%u, %u)", t.Name.c_str(), TextureFormatName(t.Format),
                             t.Width, t.Height, px % t.Width, px / t.Width);
            }
            return f;
        }
        if (t.Format == 5 && off >= t.IndexOffset && off < t.IndexOffset + t.IndexSize)
        {
            u32 k = (off - t.IndexOffset) / 2;
            u32 v = b.U16(t.IndexOffset + k * 2);
            f.Part = "TEX0";
            f.Texture = (int)i;
            f.Text = Fmt("TEX0 texture '%s' 4x4 block %u: palette +0x%X, mode %u", t.Name.c_str(), k, (v & 0x3FFF) * 4, v >> 14);
            return f;
        }
    }
    for (size_t i = 0; i < d.TexPalettes.size(); i++)
    {
        const TexPalette& tp = d.TexPalettes[i];
        if (off >= tp.Offset && off < tp.Offset + tp.Size)
        {
            u32 k = (off - tp.Offset) / 2;
            f.Part = "TEX0";
            f.HasColor = true;
            f.Color = (u16)b.U16(tp.Offset + k * 2);
            f.Palette = (int)i;
            QRgb c = Bgr555(f.Color);
            f.Text = Fmt("TEX0 palette '%s', colour %u: 0x%04X = #%02X%02X%02X", tp.Name.c_str(), k, f.Color, qRed(c), qGreen(c), qBlue(c));
            return f;
        }
    }
    if (!f.Text.empty()) return f;
    for (const Part& pt : d.Parts)
        if (off >= pt.Offset && off < pt.Offset + pt.Size)
        {
            f.Part = pt.Label;
            f.Text = d.Format + " " + pt.Label;
            return f;
        }
    f.Part = d.Format;
    f.Text = d.Format + " (between sections)";
    return f;
}

}
