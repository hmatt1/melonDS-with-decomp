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

#ifndef WB_ASSETS_H
#define WB_ASSETS_H

#include <memory>
#include <string>
#include <vector>
#include <QImage>
#include "types.h"
#include "GameArchive.h"
#include "Containers.h"
#include "WBGfx.h"

namespace melonDS::WB { class Workbench; }

// The game's graphics files (WBGfx.h): the ones in NitroFS (stored, or packed whole in a BIOS
// format), inside NARC and SDAT containers, and inside the game archive. Built in the background
// when a game loads; RAM copies are found by content, like the archive's and containers' files.
namespace WBAsset
{

using namespace melonDS;

struct File
{
    std::string Path;          // "/gfx/a.ncgr", "/gfx.narc:bg/a.ncgr", "/data/vfs.zip:Graphics/a.nsbtx"
    std::string Kind;          // NCLR, NCGR, NSCR, NCER, BTX0, BMD0
    enum Source { RomFile, Container, Archive } From = RomFile;
    u32 FileId = 0;            // the NitroFS file (the container's, or the archive's)
    int ContainerIndex = -1, Member = -1, Global = -1;     // Container
    int ArchiveMember = -1;    // Archive
    std::string Form;          // "" stored, or the compression the NitroFS file is packed in
    const u8* Data = nullptr;  // unpacked content (owned by the catalog or the index it came from)
    u32 Size = 0;
    WBGfx::Description Desc;
};

class Catalog
{
public:
    struct RomInput { u32 FileId; std::string Path; std::vector<u8> Bytes; };
    // worth copying from the ROM: a graphics file, or something packed that might unpack to one
    static bool Candidate(const u8* p, u32 len);
    void Build(std::vector<RomInput> rom, std::shared_ptr<const WBArc::ContainerIndex> containers,
               std::shared_ptr<const WBArc::ArchiveIndex> archive);

    const std::vector<File>& Files() const { return F; }
    int Find(const std::string& path) const;   // case-insensitive, '\' or '/', the leading '/' optional
    // the file to pair with i of this kind (NCLR for tiles, NCGR for a map or cells): the same name
    // stem in the same container or folder first; -1 if none. sameStemOnly: nothing else will do
    int Pair(int i, const char* kind, bool sameStemOnly = false) const;
    int First(const char* kind) const;

    // RAM runs equal to graphics files' contents (Match::Member is a file index)
    std::vector<WBArc::Match> Resolve(const u8* mem, u32 memBase, u32 memLen, u32 addr, u32 size, size_t maxMatches = 16) const
    {
        return Idx.Resolve(mem, memBase, memLen, addr, size, maxMatches);
    }
    // where file i is in RAM, largest copies first
    std::vector<WBArc::Match> Locate(int i, const u8* mem, u32 memBase, u32 memLen, size_t maxMatches = 8) const;

    std::string Summary;

private:
    std::vector<File> F;
    std::vector<std::vector<u8>> Owned;
    std::shared_ptr<const WBArc::ContainerIndex> Cont;
    std::shared_ptr<const WBArc::ArchiveIndex> Arch;
    WBArc::ContentIndex Idx;
};

// ---------------------------------------------------------------- the emulator's memory (emulation thread)

// a graphics file's copy in main RAM
struct RamRun { u32 Addr = 0, Size = 0; int File = -1; u32 FileOffset = 0; };
std::vector<RamRun> RamRuns(WB::Workbench& wb, const Catalog& c, u32 addr, u32 size, size_t max = 16);
// main RAM bytes [addr, addr+size), clipped to main RAM; empty outside it
std::vector<u8> ReadMainRam(WB::Workbench& wb, u32 addr, u32 size);
// the 2 KB of standard palette RAM (BG main, OBJ main, BG sub, OBJ sub, 512 bytes each)
std::vector<u8> ReadPaletteRam(WB::Workbench& wb);
// where bytes of a graphics file are in VRAM banks and palette RAM now ("VRAM bank C +0x2000",
// "palette RAM 0x05000020 (BG main, palette 1)")
std::vector<std::string> FindInVideoMemory(WB::Workbench& wb, const File& f, const u8* data, u32 size);

// ---------------------------------------------------------------- rendering (any thread)

struct Asset
{
    std::vector<u8> Bytes;
    WBGfx::Description Desc;
    int File = -1;              // catalog index, -1 for a RAM range that is no known file
    u32 RamAddr = 0;            // where Bytes were read from RAM, 0 when from the ROM
    std::string Path;
};

struct RenderOptions
{
    std::string Part;           // palette, tiles, map, cell, texture, raw; empty: the file's own
    int Index = 0;              // cell or texture
    int Palette = -1;           // 4bpp palette number, or texture palette; -1: the default
    std::string PalettePath;    // an NCLR to use, "@bg" or "@obj" for palette RAM; empty: pair one
    // raw ("View as")
    int Bpp = 4;
    bool Tiled = true;
    int Width = 256;
    int PaletteOffset = 0;      // in colours
};

// what the palette came from, for the panel and asset.render
struct RenderInfo { std::string Palette; std::string Tiles; };

// path: a catalog file (its ROM content). Else RAM: the graphics file whose copy holds addr (read
// whole from RAM) when size is 0, or [addr, addr+size) as it is. Emulation thread (reads RAM)
bool Load(WB::Workbench& wb, const Catalog* cat, const std::string& path, u32 addr, u32 size, Asset& out, std::string& error);

// livePalette: palette RAM (ReadPaletteRam), the fallback before the catalog's first palette
QImage Render(const Catalog* cat, const Asset& a, const RenderOptions& o, const std::vector<u8>& livePalette,
              RenderInfo& info, std::string& error);

}

#endif
