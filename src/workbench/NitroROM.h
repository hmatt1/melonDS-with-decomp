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

#ifndef WB_NITROROM_H
#define WB_NITROROM_H

#include <string>
#include <vector>
#include "types.h"

namespace melonDS::WB
{

// Special file ids used alongside the NitroFS FAT ids.
constexpr u32 FileNone = 0xFFFFFFFF;
constexpr u32 FileHeader = 0x10000;
constexpr u32 FileARM9 = 0x10001;
constexpr u32 FileARM7 = 0x10002;
constexpr u32 FileFNT = 0x10003;
constexpr u32 FileFAT = 0x10004;
constexpr u32 FileOVT9 = 0x10005;
constexpr u32 FileOVT7 = 0x10006;
constexpr u32 FileBanner = 0x10007;

struct RomFile
{
    u32 Id;            // FAT index, or one of the special ids above
    u32 Start, End;    // ROM offsets, End exclusive
    std::string Path;  // NitroFS path ("/data/model.nsbmd") or "arm9.bin", "overlay9_0003.bin", ...
    int OverlayId = -1;
    int OverlayCpu = -1;
};

struct OverlayInfo
{
    u32 Id;
    u32 Cpu;           // 0 = ARM9, 1 = ARM7
    u32 RamStart;
    u32 RamSize;
    u32 BssSize;
    u32 SinitStart, SinitEnd;
    u32 FileId;
    u32 Flags;         // bit 24: compressed, low 24 bits: compressed size
    bool Compressed() const { return Flags & 0x01000000; }
    u32 RamEnd() const { return RamStart + RamSize + BssSize; }
};

struct ModuleInfo
{
    std::string Name;  // "arm9", "arm7", "ov9_0003", "itcm", "dtcm"
    std::string Kind;  // static, overlay, autoload
    u32 Cpu;
    u32 LoadAddress;
    u32 Size;
    u32 BssSize;
    u32 FileId;
    int OverlayId = -1;
    bool Compressed = false;
};

// Unpacks the backward LZ format (BLZ) of compressed arm9.bin and overlays: an
// uncompressed head, then LZ data that is decoded from the end towards the start, with a footer giving
// the compressed length, the footer's own length and how much the data grows. False if it is malformed.
bool UnpackBackwardLz(const u8* in, u32 len, std::vector<u8>& out);

// Static description of the loaded game ROM: header, NitroFS files, overlay tables and modules.
class NitroROM
{
public:
    bool Load(const u8* rom, u32 romlen, u32 filelen);
    void Clear();

    bool Valid() const { return IsValid; }
    const u8* Data() const { return ROM; }
    u32 Length() const { return ROMLen; }

    const RomFile* FileForOffset(u32 romoffset) const;
    const RomFile* FileById(u32 id) const;
    const RomFile* FileByPath(const std::string& path) const;
    const OverlayInfo* Overlay(u32 cpu, u32 id) const;
    const OverlayInfo* OverlayForFile(u32 fileid) const;

    std::string Sha1Hex() const;

    std::vector<RomFile> Files;          // sorted by Start
    std::vector<OverlayInfo> Overlays9, Overlays7;
    std::vector<ModuleInfo> Modules;

    u8 Sha1[20] {};
    char Title[13] {};
    char GameCode[5] {};
    u32 ARM9RomOffset = 0, ARM9RamAddress = 0, ARM9Size = 0, ARM9Entry = 0;
    u32 ARM7RomOffset = 0, ARM7RamAddress = 0, ARM7Size = 0, ARM7Entry = 0;

    // module parameters found inside arm9.bin (0 when not found)
    u32 ModuleParamsAddr = 0;
    u32 AutoloadListStart = 0, AutoloadListEnd = 0, AutoloadStart = 0;
    u32 StaticBssStart = 0, StaticBssEnd = 0, CompressedStaticEnd = 0;
    u32 BuildVersion = 0;

private:
    void ParseFNT(u32 dirid, const std::string& prefix, int depth);
    void FindModuleParams();

    const u8* ROM = nullptr;
    u32 ROMLen = 0;
    bool IsValid = false;
    std::vector<std::string> FatNames;   // index = FAT id
    std::vector<u32> SortedIndex;
};

}

#endif
