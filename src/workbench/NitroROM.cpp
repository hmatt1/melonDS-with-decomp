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
#include "NitroROM.h"
#include "../NDS_Header.h"

extern "C" {
#include "../sha1/sha1.h"
}

namespace melonDS::WB
{

static u32 Rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 Rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }
// [off, off + size) inside len bytes, without the sum wrapping past 4 GB
static bool Within(u32 off, u32 size, u32 len) { return (u64)off + size <= len; }

bool UnpackBackwardLz(const u8* in, u32 len, std::vector<u8>& out)
{
    out.clear();
    if (len < 8) return false;
    u32 encLen = Rd32(in + len - 8) & 0x00FFFFFF;    // footer included
    u32 hdrLen = in[len - 5];
    u32 grow = Rd32(in + len - 4);
    if (hdrLen < 8 || encLen < hdrLen || encLen > len || grow > (16u << 20)) return false;
    u32 head = len - encLen;
    u32 packed = encLen - hdrLen;
    u32 rawLen = encLen + grow;                      // what the compressed region unpacks to
    out.assign(in, in + head);
    out.resize((size_t)head + rawLen);
    // read backwards from the end of the packed bytes, write backwards from the end of the output
    s64 src = (s64)head + packed;
    s64 dst = (s64)head + rawLen;
    const s64 srcEnd = head, dstEnd = head;
    while (dst > dstEnd)
    {
        if (src <= srcEnd) break;
        u8 flags = in[--src];
        for (int b = 7; b >= 0 && dst > dstEnd; b--)
        {
            if (!(flags & (1 << b)))
            {
                if (src <= srcEnd) return false;
                out[--dst] = in[--src];
                continue;
            }
            if (src - 2 < srcEnd) return false;
            u32 hi = in[--src];
            u32 lo = in[--src];
            u32 n = (hi >> 4) + 3;
            u32 disp = (((hi & 0xF) << 8) | lo) + 3;
            for (u32 i = 0; i < n && dst > dstEnd; i++)
            {
                if (dst - 1 + disp >= (s64)out.size()) return false;
                out[dst - 1] = out[dst - 1 + disp];
                dst--;
            }
        }
    }
    // the compressed region ends at the head: anything left over means a malformed stream
    return dst == dstEnd;
}

void NitroROM::Clear()
{
    ROM = nullptr;
    ROMLen = 0;
    IsValid = false;
    Files.clear();
    Overlays9.clear();
    Overlays7.clear();
    Modules.clear();
    FatNames.clear();
    memset(Sha1, 0, sizeof(Sha1));
    memset(Title, 0, sizeof(Title));
    memset(GameCode, 0, sizeof(GameCode));
    ModuleParamsAddr = 0;
    AutoloadListStart = AutoloadListEnd = AutoloadStart = 0;
    StaticBssStart = StaticBssEnd = CompressedStaticEnd = BuildVersion = 0;
}

bool NitroROM::Load(const u8* rom, u32 romlen, u32 filelen)
{
    Clear();
    if (!rom || romlen < 0x200) return false;

    ROM = rom;
    ROMLen = romlen;
    u32 hashlen = (filelen && filelen <= romlen) ? filelen : romlen;

    SHA1_CTX ctx;
    SHA1Init(&ctx);
    // SHA1Update takes a 32-bit length; feed in chunks to be safe
    for (u32 pos = 0; pos < hashlen; )
    {
        u32 chunk = std::min<u32>(hashlen - pos, 0x100000);
        SHA1Update(&ctx, rom + pos, chunk);
        pos += chunk;
    }
    SHA1Final(Sha1, &ctx);

    NDSHeader hdr;
    memcpy(&hdr, rom, sizeof(hdr));
    memcpy(Title, hdr.GameTitle, 12);
    memcpy(GameCode, hdr.GameCode, 4);

    ARM9RomOffset = hdr.ARM9ROMOffset; ARM9RamAddress = hdr.ARM9RAMAddress; ARM9Size = hdr.ARM9Size; ARM9Entry = hdr.ARM9EntryAddress;
    ARM7RomOffset = hdr.ARM7ROMOffset; ARM7RamAddress = hdr.ARM7RAMAddress; ARM7Size = hdr.ARM7Size; ARM7Entry = hdr.ARM7EntryAddress;

    auto addSpecial = [&](u32 id, u32 start, u32 size, const char* name)
    {
        if (!size || start >= romlen) return;
        RomFile f;
        f.Id = id;
        f.Start = start;
        f.End = std::min(start + size, romlen);
        f.Path = name;
        Files.push_back(f);
    };

    addSpecial(FileHeader, 0, 0x200, "header.bin");
    addSpecial(FileARM9, hdr.ARM9ROMOffset, hdr.ARM9Size, "arm9.bin");
    addSpecial(FileARM7, hdr.ARM7ROMOffset, hdr.ARM7Size, "arm7.bin");
    addSpecial(FileFNT, hdr.FNTOffset, hdr.FNTSize, "fnt.bin");
    addSpecial(FileFAT, hdr.FATOffset, hdr.FATSize, "fat.bin");
    addSpecial(FileOVT9, hdr.ARM9OverlayOffset, hdr.ARM9OverlaySize, "y9.bin");
    addSpecial(FileOVT7, hdr.ARM7OverlayOffset, hdr.ARM7OverlaySize, "y7.bin");
    addSpecial(FileBanner, hdr.BannerOffset, hdr.BannerOffset ? 0x840 : 0, "banner.bin");

    // FAT
    u32 numfat = 0;
    if (hdr.FATOffset && Within(hdr.FATOffset, hdr.FATSize, romlen))
        numfat = hdr.FATSize / 8;
    FatNames.assign(numfat, std::string());

    // FNT gives the names
    if (hdr.FNTOffset && hdr.FNTSize >= 8 && Within(hdr.FNTOffset, hdr.FNTSize, romlen))
        ParseFNT(0xF000, "", 0);

    // overlay tables
    auto parseOVT = [&](u32 off, u32 size, u32 cpu, std::vector<OverlayInfo>& out)
    {
        if (!off || !size || !Within(off, size, romlen)) return;
        for (u32 i = 0; i + 32 <= size; i += 32)
        {
            const u8* e = rom + off + i;
            OverlayInfo o;
            o.Id = Rd32(e + 0);
            o.Cpu = cpu;
            o.RamStart = Rd32(e + 4);
            o.RamSize = Rd32(e + 8);
            o.BssSize = Rd32(e + 12);
            o.SinitStart = Rd32(e + 16);
            o.SinitEnd = Rd32(e + 20);
            o.FileId = Rd32(e + 24);
            o.Flags = Rd32(e + 28);
            out.push_back(o);
        }
    };
    parseOVT(hdr.ARM9OverlayOffset, hdr.ARM9OverlaySize, 0, Overlays9);
    parseOVT(hdr.ARM7OverlayOffset, hdr.ARM7OverlaySize, 1, Overlays7);

    for (u32 i = 0; i < numfat; i++)
    {
        const u8* e = rom + hdr.FATOffset + i*8;
        u32 start = Rd32(e), end = Rd32(e + 4);
        if (end <= start || start >= romlen) continue;
        RomFile f;
        f.Id = i;
        f.Start = start;
        f.End = std::min(end, romlen);
        f.Path = FatNames[i];
        if (f.Path.empty())
        {
            char buf[48];
            snprintf(buf, sizeof(buf), "file_%04X.bin", i);
            f.Path = buf;
        }
        for (const OverlayInfo& o : Overlays9)
            if (o.FileId == i)
            {
                char buf[48];
                snprintf(buf, sizeof(buf), "overlay9_%04u.bin", o.Id);
                f.Path = buf;
                f.OverlayId = (int)o.Id;
                f.OverlayCpu = 0;
            }
        for (const OverlayInfo& o : Overlays7)
            if (o.FileId == i)
            {
                char buf[48];
                snprintf(buf, sizeof(buf), "overlay7_%04u.bin", o.Id);
                f.Path = buf;
                f.OverlayId = (int)o.Id;
                f.OverlayCpu = 1;
            }
        Files.push_back(f);
    }

    std::stable_sort(Files.begin(), Files.end(), [](const RomFile& a, const RomFile& b)
    {
        if (a.Start != b.Start) return a.Start < b.Start;
        return a.End > b.End;
    });

    FindModuleParams();

    // modules
    {
        ModuleInfo m;
        m.Name = "arm9"; m.Kind = "static"; m.Cpu = 0;
        m.LoadAddress = ARM9RamAddress; m.Size = ARM9Size; m.BssSize = 0; m.FileId = FileARM9;
        m.Compressed = CompressedStaticEnd != 0;
        // the static module ends where the autoload blocks (ITCM/DTCM code and data) begin
        if (ModuleParamsAddr && AutoloadStart > ARM9RamAddress && AutoloadStart - ARM9RamAddress <= ARM9Size)
            m.Size = AutoloadStart - ARM9RamAddress;
        if (StaticBssEnd > StaticBssStart) m.BssSize = StaticBssEnd - StaticBssStart;
        Modules.push_back(m);
        m = ModuleInfo();
        m.Name = "arm7"; m.Kind = "static"; m.Cpu = 1;
        m.LoadAddress = ARM7RamAddress; m.Size = ARM7Size; m.BssSize = 0; m.FileId = FileARM7;
        Modules.push_back(m);
        for (const OverlayInfo& o : Overlays9)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "ov9_%04u", o.Id);
            m = ModuleInfo();
            m.Name = buf; m.Kind = "overlay"; m.Cpu = 0;
            m.LoadAddress = o.RamStart; m.Size = o.RamSize; m.BssSize = o.BssSize; m.FileId = o.FileId;
            m.OverlayId = (int)o.Id; m.Compressed = o.Compressed();
            Modules.push_back(m);
        }
        for (const OverlayInfo& o : Overlays7)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "ov7_%04u", o.Id);
            m = ModuleInfo();
            m.Name = buf; m.Kind = "overlay"; m.Cpu = 1;
            m.LoadAddress = o.RamStart; m.Size = o.RamSize; m.BssSize = o.BssSize; m.FileId = o.FileId;
            m.OverlayId = (int)o.Id; m.Compressed = o.Compressed();
            Modules.push_back(m);
        }

        // autoload blocks (ITCM/DTCM code and data copied out of arm9.bin) when the static module is uncompressed
        if (ModuleParamsAddr && !CompressedStaticEnd && AutoloadListStart >= ARM9RamAddress && AutoloadListEnd > AutoloadListStart)
        {
            u32 listoff = AutoloadListStart - ARM9RamAddress;
            u32 listend = AutoloadListEnd - ARM9RamAddress;
            if (listend <= ARM9Size)
            {
                int n = 0;
                for (u32 p = listoff; p + 12 <= listend; p += 12, n++)
                {
                    const u8* e = rom + ARM9RomOffset + p;
                    u32 addr = Rd32(e), size = Rd32(e + 4), bss = Rd32(e + 8);
                    m = ModuleInfo();
                    if ((addr >> 24) == 0x01 || addr < 0x02000000) m.Name = "itcm";
                    else if (((addr >> 24) == 0x02 && addr >= 0x027C0000) || (addr >> 24) == 0x0B) m.Name = "dtcm";
                    else { char buf[32]; snprintf(buf, sizeof(buf), "autoload_%d", n); m.Name = buf; }
                    for (const ModuleInfo& prev : Modules)
                        if (prev.Name == m.Name) { char buf[32]; snprintf(buf, sizeof(buf), "%s_%d", m.Name.c_str(), n); m.Name = buf; break; }
                    m.Kind = "autoload"; m.Cpu = 0;
                    m.LoadAddress = addr; m.Size = size; m.BssSize = bss; m.FileId = FileARM9;
                    Modules.push_back(m);
                }
            }
        }
    }

    IsValid = true;
    return true;
}

void NitroROM::ParseFNT(u32 dirid, const std::string& prefix, int depth)
{
    if (depth > 32) return;
    NDSHeader hdr;
    memcpy(&hdr, ROM, sizeof(hdr));
    const u8* fnt = ROM + hdr.FNTOffset;
    u32 fntsize = hdr.FNTSize;

    u32 idx = dirid & 0xFFF;
    if ((idx + 1) * 8 > fntsize) return;
    u32 subtable = Rd32(fnt + idx*8);
    u32 fileid = Rd16(fnt + idx*8 + 4);
    if (subtable >= fntsize) return;

    u32 pos = subtable;
    while (pos < fntsize)
    {
        u8 tl = fnt[pos++];
        if (tl == 0) break;
        u32 len = tl & 0x7F;
        if (pos + len > fntsize) break;
        std::string name((const char*)&fnt[pos], len);
        pos += len;
        if (tl & 0x80)
        {
            if (pos + 2 > fntsize) break;
            u32 sub = Rd16(fnt + pos);
            pos += 2;
            ParseFNT(sub, prefix + "/" + name, depth + 1);
        }
        else
        {
            if (fileid < FatNames.size())
                FatNames[fileid] = prefix + "/" + name;
            fileid++;
        }
    }
}

void NitroROM::FindModuleParams()
{
    if (!ARM9Size || !Within(ARM9RomOffset, ARM9Size, ROMLen)) return;
    const u8* arm9 = ROM + ARM9RomOffset;
    u32 p = 0xFFFFFFFF;

    // the footer that follows arm9.bin in the ROM points at the module parameters
    if (Within(ARM9RomOffset, ARM9Size + 8, ROMLen) && Rd32(arm9 + ARM9Size) == 0xDEC00621)
    {
        u32 off = Rd32(arm9 + ARM9Size + 4);
        if (Within(off, 0x24, ARM9Size) && Rd32(arm9 + off + 0x1C) == 0xDEC00621) p = off;
    }
    // otherwise search for the nitrocode words at the end of the module params
    if (p == 0xFFFFFFFF)
    {
        for (u32 i = 0x1C; i + 8 <= ARM9Size; i += 4)
            if (Rd32(arm9 + i) == 0xDEC00621 && Rd32(arm9 + i + 4) == 0x2106C0DE) { p = i - 0x1C; break; }
    }
    if (p == 0xFFFFFFFF) return;

    ModuleParamsAddr = ARM9RamAddress + p;
    AutoloadListStart = Rd32(arm9 + p + 0x00);
    AutoloadListEnd = Rd32(arm9 + p + 0x04);
    AutoloadStart = Rd32(arm9 + p + 0x08);
    StaticBssStart = Rd32(arm9 + p + 0x0C);
    StaticBssEnd = Rd32(arm9 + p + 0x10);
    CompressedStaticEnd = Rd32(arm9 + p + 0x14);
    BuildVersion = Rd32(arm9 + p + 0x18);
}

const RomFile* NitroROM::FileForOffset(u32 off) const
{
    // Files is sorted by start; find the last file starting at or before off that contains it.
    // Prefer FAT files over the special containers (they never overlap in a sane ROM).
    auto it = std::upper_bound(Files.begin(), Files.end(), off, [](u32 v, const RomFile& f) { return v < f.Start; });
    while (it != Files.begin())
    {
        --it;
        if (off >= it->Start && off < it->End) return &*it;
        if (it->End <= off && (off - it->End) > 0x100000) break;
    }
    return nullptr;
}

const RomFile* NitroROM::FileById(u32 id) const
{
    for (const RomFile& f : Files)
        if (f.Id == id) return &f;
    return nullptr;
}

const RomFile* NitroROM::FileByPath(const std::string& path) const
{
    for (const RomFile& f : Files)
    {
        if (f.Path == path) return &f;
        if (!path.empty() && path[0] != '/' && f.Path.size() > 1 && f.Path.substr(1) == path) return &f;
    }
    return nullptr;
}

const OverlayInfo* NitroROM::Overlay(u32 cpu, u32 id) const
{
    const auto& list = cpu ? Overlays7 : Overlays9;
    for (const OverlayInfo& o : list)
        if (o.Id == id) return &o;
    return nullptr;
}

const OverlayInfo* NitroROM::OverlayForFile(u32 fileid) const
{
    for (const OverlayInfo& o : Overlays9) if (o.FileId == fileid) return &o;
    for (const OverlayInfo& o : Overlays7) if (o.FileId == fileid) return &o;
    return nullptr;
}

std::string NitroROM::Sha1Hex() const
{
    if (!IsValid) return "";
    char buf[41];
    for (int i = 0; i < 20; i++) snprintf(buf + i*2, 3, "%02x", Sha1[i]);
    return std::string(buf, 40);
}

}
