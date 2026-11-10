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

#ifndef WB_GAMEARCHIVE_H
#define WB_GAMEARCHIVE_H

#include <algorithm>
#include <string>
#include <vector>
#include "types.h"

namespace melonDS::WB { class NitroROM; }

// Resolver for game archives stored inside NitroFS, so provenance and RAM contents can name the
// archived file instead of stopping at the archive.
//
// Octane engine VFS (TrackMania DS and other Firebrand Games titles): three files in the NitroFS root.
//   vfs.zip  a standard ZIP (stored or deflated members); the game reads only the bytes it needs
//   vfs.bin  index: magic 00 00 FF FF 00 00 FF FF, then <entries> records of 4 x u16, then a u32 per file
//            (the offset of that file's central directory record), then the name table.
//            file:      name offset, next sibling (0xFFFF = last), 0xFFFF, file index (= ZIP order)
//            directory: name offset, next sibling,                 first child (= own index + 1), 0x7FFF
//   vfs.dat  "entries, files, name table size" as text
namespace WBArc
{

using namespace melonDS;

struct Member
{
    std::string Path;          // as stored in the ZIP
    u32 Index = 0;             // central directory order, the game's file index
    int VfsEntry = -1;         // record in vfs.bin, -1 without one
    u32 CdOffset = 0;          // record offset within the central directory
    u32 HeaderOffset = 0;      // local header, archive-relative
    u32 DataOffset = 0;        // compressed data, archive-relative
    u32 PackedSize = 0, Size = 0, Crc = 0;
    u16 Method = 0;            // 0 stored, 8 deflate
    bool Ok = false;           // content available and the CRC matched
    int DuplicateOf = -1;      // same content as this earlier member
    u64 ContentOffset = 0;
};

// RAM bytes [Addr, Addr+Size) equal Member's content from MemberOffset
struct Match
{
    u32 Addr = 0, Size = 0;
    int Member = -1;
    u32 MemberOffset = 0;
    std::vector<int> Alternatives;   // members with the same bytes there (identical files, shared data)
};

// Finds RAM runs equal to indexed byte strings (an archive's files, ROM files and their unpacked forms).
// Contents are not copied: they must stay valid and unchanged while the index is used.
class ContentIndex
{
public:
    // id: the caller's number for this content, reported as Match::Member
    void Add(int id, const u8* data, u32 size);
    // copy has the same bytes as original: matches of original list it among the alternatives
    void AddCopy(int original, int copy);
    void Finish();                         // after the last Add

    // splits RAM [addr, addr+size) into runs equal to indexed contents. Runs extend past the range
    // to their full length; runs shorter than MinMatch are ignored.
    std::vector<Match> Resolve(const u8* mem, u32 memBase, u32 memLen, u32 addr, u32 size, size_t maxMatches = 32) const;
    // grows a match of content c at offset off with RAM address q in both directions: [s, e)
    static void Extend(const u8* mem, u32 memBase, u32 memLen, u32 q, const u8* c, u32 csize, u32 off, u32& s, u32& e);
    // windows of runs and short repeating patterns match everywhere: not worth indexing
    static bool Varied(const u8* p);
    // where content c (reported as id) is in RAM, whole or in parts of at least minMatch bytes, largest first
    static std::vector<Match> Find(int id, const u8* c, u32 csize, u32 minMatch, const u8* mem, u32 memBase, u32 memLen, size_t maxMatches);

    u32 MinMatch = 32;
    static constexpr u32 Window = 16;      // bytes hashed per key
    static constexpr u32 Stride = 32;      // content offsets indexed

private:
    struct Entry { const u8* Data = nullptr; u32 Size = 0; };
    struct Key { u32 Hash; u32 Id; u32 Offset; };
    std::vector<Entry> E;                  // by id
    std::vector<Key> Keys;                 // sorted by hash
    std::vector<std::vector<int>> Copies;  // by id: contents with the same bytes
    // one bit per hash bucket that has a key: RAM windows are hashed at every byte offset (a rolling
    // hash), and almost all of them are rejected here without searching Keys
    std::vector<u64> Filter;
    u32 FilterShift = 32;
    bool MayHave(u32 h) const
    {
        u32 b = (h * 0x9E3779B1u) >> FilterShift;
        return (Filter[b >> 6] >> (b & 63)) & 1;
    }
};

// unpacks data in one of the BIOS compression formats: LZ77 (type 0x10, and its 0x11
// variant), Huffman (0x24, 0x28) or run length (0x30), optionally behind an "LZ77" tag.
// Fails unless the stream decodes to exactly the size its header declares and ends near the input's end.
bool UnpackBios(const u8* in, u32 len, std::vector<u8>& out, std::string& form);

class ArchiveIndex
{
public:
    // Octane VFS detection: vfs.zip, vfs.bin and vfs.dat in the same NitroFS directory
    static bool DetectVfs(const melonDS::WB::NitroROM& rom, u32& zipFile, u32& binFile, u32& datFile);

    // parses the ZIP, inflates every member, checks vfs.bin against it and indexes the contents.
    // The inputs are only read during the call.
    bool Build(const u8* zip, u32 zipLen, const u8* vfsBin, u32 binLen, const u8* vfsDat, u32 datLen, std::string& error);

    std::string Kind = "octane_vfs";
    std::string ArchivePath, IndexPath;    // NitroFS paths
    u32 ArchiveFileId = 0xFFFFFFFF;
    u32 ArchiveRomStart = 0;               // ROM offset of the archive file
    u32 CdStart = 0, CdSize = 0;           // central directory, archive-relative
    u32 VfsAgree = 0;                      // vfs.bin paths that match the ZIP's names
    std::string Summary;                   // one line for status displays

    const std::vector<Member>& Members() const { return M; }
    const u8* Content(const Member& m) const { return Data.data() + m.ContentOffset; }
    u64 ContentBytes() const { return Data.size(); }

    // member whose local header, data or central directory record holds this archive offset (-1: none)
    int MemberAt(u32 archOffset, std::string* part = nullptr, u32* partOffset = nullptr) const;
    // "Graphics/x.nsbmd, compressed data +0x10" for an archive offset; empty outside every member
    std::string Describe(u32 archOffset) const;
    // case-insensitive; a leading '/' is ignored
    int FindPath(const std::string& path) const;

    // splits RAM [addr, addr+size) into runs equal to member contents (ContentIndex::Resolve)
    std::vector<Match> Resolve(const u8* mem, u32 memBase, u32 memLen, u32 addr, u32 size, size_t maxMatches = 32) const
    {
        return Idx.Resolve(mem, memBase, memLen, addr, size, maxMatches);
    }
    // where a member's content is in RAM (whole or in parts)
    std::vector<Match> Locate(int member, const u8* mem, u32 memBase, u32 memLen, size_t maxMatches = 16) const;

private:
    std::vector<Member> M;
    std::vector<u8> Data;
    ContentIndex Idx;                      // ids are member indices
    std::vector<int> ByHeader;             // members by local header offset
};

// the unpacked form of a plain NitroFS file
struct RomFileForm
{
    u32 FileId = 0;
    std::string Path;
    std::string Form;          // "lz10", "lz11", "huffman", "rle"
    u32 Size = 0;
};

// The unpacked forms of plain NitroFS files (everything but the game archive, whose files ArchiveIndex
// covers) in a BIOS compression format, so content labelling can name the file when game code unpacks
// it through a path that loses taint. Files stored as they are need no index: copies keep their labels.
class RomFileIndex
{
public:
    // Raw: index the bytes as they are (the static modules, which the game copies elsewhere: autoload,
    // relocation). Raw entries never make a file pending (Indexed is false): they are only searched with
    // the others, and when a savestate's memory is labelled again by content
    struct Input { u32 FileId; std::string Path; std::vector<u8> Bytes; bool Raw = false; };
    void Build(std::vector<Input> files);

    const std::vector<RomFileForm>& Forms() const { return F; }
    bool Indexed(u32 fileId) const { return std::binary_search(Ids.begin(), Ids.end(), fileId); }
    u64 Bytes() const { return Total; }
    // Match::Member is a form index
    std::vector<Match> Resolve(const u8* mem, u32 memBase, u32 memLen, u32 addr, u32 size, size_t maxMatches = 32) const
    {
        return Idx.Resolve(mem, memBase, memLen, addr, size, maxMatches);
    }

    u64 MaxFileSize = 4u << 20;  // larger files are skipped, to bound memory

private:
    std::vector<RomFileForm> F;
    std::vector<std::vector<u8>> Content;  // per form
    std::vector<u32> Ids;                  // sorted file ids with a form
    u64 Total = 0;
    ContentIndex Idx;
};

}

#endif
