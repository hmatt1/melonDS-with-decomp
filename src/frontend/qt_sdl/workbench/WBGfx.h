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

#ifndef WB_GFX_H
#define WB_GFX_H

#include <string>
#include <vector>
#include <QImage>
#include "types.h"

// The DS graphics file formats, from public format documentation (GBATEK's hardware formats and
// the community's descriptions of the Nitro file layouts): which part of the file each byte is,
// and the images they hold.
//
// Every Nitro file starts with a 16-byte header: magic (the 2D formats store theirs reversed:
// "RLCN" is an NCLR), byte order mark 0xFEFF, version, file size, header size, section count. The
// 2D formats' sections follow one after another, each with a magic and a size; BTX0 and BMD0 list
// their sections' offsets after the header.
//
// NCLR  PLTT: palettes, BGR555, 16 colours (4bpp) or 256 (8bpp) each; PCMP: which palette ids
// NCGR  CHAR: tiles, 4bpp or 8bpp, as 8x8 characters or a linear bitmap
// NSCR  SCRN: a tilemap: 16-bit entries (tile 0-9, H-flip 10, V-flip 11, palette 12-15), or 8-bit
//       for affine maps
// NCER  CEBK: cells, each a list of OAM entries (3 x 16 bits) with the OBJ mapping mode
// NSBTX BTX0 and NSBMD BMD0: TEX0 holds textures in the 3D engine's formats (A3I5, 4, 16 and
//       256 colours, 4x4 compressed, A5I3, direct colour) and their palettes, named in dictionaries
//
// Nothing here reads outside the bytes it is given. A file that stops making sense is described as
// far as it was understood, with UnreadableFrom set.

namespace WBGfx
{

using namespace melonDS;

// a range of the file and what it is
struct Part
{
    u32 Offset = 0, Size = 0;
    std::string Kind;       // header, section, palette, tiles, map, cells, oam, texture, texture_4x4, texture_index,
                            // tex_palette, dictionary, other
    int Index = -1;         // palette, cell, texture ...; -1 for the whole block
    std::string Label;      // "PLTT", "palette 2", "texture 'car' (A3I5 32x32)"
};

struct Texture
{
    std::string Name;
    u32 Param = 0;          // TEXIMAGE_PARAM as stored
    int Format = 0;         // 1 A3I5, 2 4-colour, 3 16-colour, 4 256-colour, 5 4x4, 6 A5I3, 7 direct
    int Width = 0, Height = 0;
    bool Color0Transparent = false;
    u32 Offset = 0, Size = 0;           // texel data, from the file's start
    u32 IndexOffset = 0, IndexSize = 0; // 4x4: palette index data
};

struct TexPalette
{
    std::string Name;
    u32 Offset = 0, Size = 0;   // from the file's start (to the next palette, or the end of the block)
};

struct Oam
{
    u16 Attr[3] = {0, 0, 0};
    int X = 0, Y = 0, Width = 8, Height = 8;
    int Tile = 0, Palette = 0;
    bool HFlip = false, VFlip = false, Bpp8 = false, Affine = false;
};

struct Cell
{
    u32 Offset = 0;         // its table entry
    u32 OamOffset = 0;      // its OAM entries, from the file's start
    std::vector<Oam> Oams;
};

struct Description
{
    std::string Format;     // NCLR, NCGR, NSCR, NCER, BTX0, BMD0; empty when not a graphics file
    u32 Size = 0;           // bytes given
    u32 UnreadableFrom = 0; // 0: everything understood; else where it stopped making sense
    std::string Problem;    // why it stopped
    std::vector<Part> Parts;

    // NCLR
    u32 PalOffset = 0, PalSize = 0;
    int PalBpp = 4;                     // 4: 16 colours per palette, 8: 256
    std::vector<u16> PalIds;            // PCMP
    // NCGR
    u32 CharOffset = 0, CharSize = 0;
    int CharBpp = 4;
    int TilesX = 0, TilesY = 0;         // 0 when the file leaves them out (0xFFFF)
    bool Linear = false;
    u32 Mapping = 0;
    // NSCR
    u32 MapOffset = 0, MapSize = 0;
    int MapWidth = 0, MapHeight = 0;    // pixels
    int MapColorMode = 0, MapFormat = 0;    // colour mode 0 16x16, 1 256x1, 2 256x16; format 0 text, 1 affine, 2 extended affine
    // NCER
    std::vector<Cell> Cells;
    u32 CellMapping = 0;                // 0 2D, 1-4 1D 32K/64K/128K/256K
    // TEX0
    u32 Tex0Offset = 0;
    std::vector<Texture> Textures;
    std::vector<TexPalette> TexPalettes;

    bool IsGraphics() const { return !Format.empty(); }
};

Description Describe(const u8* p, u32 len);

// what the byte at off is, as one line ("NCGR CHAR, tile 37, row 3"); Color/Tile when it is a
// colour or a tile (for the Inspector's swatch and thumbnail)
struct FieldInfo
{
    std::string Part;
    std::string Text;
    bool HasColor = false;
    u16 Color = 0;
    int Tile = -1;          // NCGR tile, or NSCR entry's tile
    int Palette = -1;       // the palette the entry or colour belongs to
    int Texture = -1;
};
FieldInfo DescribeAt(const Description& d, const u8* p, u32 len, u32 off);

// ---------------------------------------------------------------- decoding

QRgb Bgr555(u16 c, bool opaque = true);
std::vector<QRgb> Colors(const u8* p, u32 len, u32 offset, u32 count);    // BGR555 words
// all colours of an NCLR (empty when it isn't one)
std::vector<QRgb> PaletteColors(const Description& d, const u8* p, u32 len);

// palette swatches: 16 per row, cell pixels square
QImage RenderPalette(const std::vector<QRgb>& colors, int cell = 8);

// raw tiles or bitmap: bpp 2, 4, 8 or 16 (BGR555); tiled: 8x8 characters, else linear rows
// widthPx wide. palOffset: first colour used (4bpp: palette number * 16)
QImage DecodeRaw(const u8* data, u32 size, int bpp, bool tiled, int widthPx, const std::vector<QRgb>& pal,
                 int palOffset, bool transparent0);

// an NCGR's tiles with a palette (4bpp: palIndex picks the 16 colours)
QImage RenderTiles(const Description& g, const u8* p, u32 len, const std::vector<QRgb>& pal, int palIndex, int widthTiles = 0);
// one tile, 8x8
QImage RenderTile(const Description& g, const u8* p, u32 len, int tile, const std::vector<QRgb>& pal, int palIndex);
// an NSCR with its NCGR and palette colours
QImage RenderMap(const Description& s, const u8* sp, u32 slen, const Description& g, const u8* gp, u32 glen,
                 const std::vector<QRgb>& pal);
// one NCER cell with its NCGR and palette colours; origin: where (0, 0) is in the image
QImage RenderCell(const Description& c, int cell, const Description& g, const u8* gp, u32 glen,
                  const std::vector<QRgb>& pal, QPoint* origin = nullptr);
// a TEX0 texture with one of its palettes (-1: the same-named palette, else the first)
QImage RenderTexture(const Description& d, const u8* p, u32 len, int texture, int palette = -1);
int TexturePaletteFor(const Description& d, int texture);   // same-named (with or without _pl), else 0; -1 if none

// the byte range a pixel of a rendered image came from (for click-through); false outside
bool TileBytes(const Description& g, int tile, u32& offset, u32& size);
bool TexelBytes(const Description& d, int texture, int x, int y, u32& offset, u32& size);

const char* TextureFormatName(int format);

}

#endif
