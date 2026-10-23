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

#ifndef WB_CONTAINERS_H
#define WB_CONTAINERS_H

#include <string>
#include <vector>
#include "types.h"
#include "GameArchive.h"

// Resolver for the common DS container formats in NitroFS, next to the game archive (GameArchive.h),
// so provenance and RAM contents can name the file inside a container.
//
// NARC  "NARC" header, then BTAF (start and end of each file, relative to GMIF's data), BTNF (a file
//       name table like NitroFS's; often only the root, with no names) and GMIF (the files' data).
// SDAT  "SDAT" header with block offsets: SYMB (names, optional), INFO (which file each sequence, bank,
//       wave archive and stream uses), FAT (offset and size of each file, from the SDAT's start), FILE.
//
// A container file may be packed whole in a BIOS compression format (a ".carc"); its members are then
// known by content only. A member may be packed on its own, and may itself be a container: nested
// members are listed in the top container as "sub.narc/0001.ncgr", up to MaxDepth levels.

namespace WBArc
{

using namespace melonDS;

struct ContainerMember
{
    std::string Name;          // path inside the container
    std::string Kind;          // from the data's magic: NCGR, BMD0, SSEQ ... ("" when unknown)
    u32 Offset = NoOffset;     // stored bytes, from the container file's start in the ROM; NoOffset: not addressable
    u32 StoredSize = 0;
    std::string Form = "stored";   // or the BIOS compression it is packed in: lz10, lz11, huffman, rle
    u32 Size = 0;              // as unpacked
    int Parent = -1;           // the member it is nested in
    int Depth = 0;             // 0 for the container's own files
    bool Expanded = false;     // a container whose files are listed after it: found in RAM through them
    bool Ok = false;           // content available and indexed
    int DuplicateOf = -1;      // global id of an earlier member with the same content
    u32 Global = 0;            // id in the content index
    u32 Blob = 0, BlobOffset = 0;

    static constexpr u32 NoOffset = 0xFFFFFFFF;
};

struct Container
{
    u32 FileId = 0;
    std::string Path;          // NitroFS path
    std::string Format;        // narc or sdat
    std::string Packed;        // "", or the compression the whole file is packed in
    u32 RomStart = 0, RomSize = 0;
    u32 Size = 0;              // as unpacked
    u32 FirstGlobal = 0;       // global id of Members[0]
    std::vector<ContainerMember> Members;
    std::vector<int> ByOffset; // addressable members by Offset, the deeper first among equal offsets
    int Nested = 0;            // members inside nested containers
};

class ContainerIndex
{
public:
    struct Input { u32 FileId; std::string Path; u32 RomStart; std::vector<u8> Bytes; };
    // worth reading: starts like a container, or like data in a BIOS compression format
    static bool Candidate(const u8* p, u32 len);
    void Build(std::vector<Input> files);

    const std::vector<Container>& Containers() const { return C; }
    const Container* ByFile(u32 fileId) const;
    int IndexOf(u32 fileId) const;
    // the deepest member whose stored bytes hold this offset (from the container's start); -1 for
    // container headers and tables, and for containers packed whole
    int MemberAt(const Container& c, u32 off, std::string* part = nullptr, u32* partOffset = nullptr) const;
    // "0001.ncgr, stored data +0x10" for an offset in a container file; empty outside every member
    std::string Describe(u32 fileId, u32 off) const;
    // "/a.narc:0003.ncgr" (case-insensitive, '\' or '/', the leading '/' optional)
    bool Find(const std::string& path, int& container, int& member) const;
    std::string FullName(int container, int member) const;

    std::pair<int, int> FromGlobal(int global) const;     // {container, member}, {-1, -1} if none
    // inner is nested, at any depth, in outer (both member indexes of c)
    bool Inside(const Container& c, int outer, int inner) const;
    const u8* Content(const ContainerMember& m) const { return Blobs[m.Blob].data() + m.BlobOffset; }
    u64 ContentBytes() const { return Total; }
    u32 MemberCount() const { return (u32)Global.size(); }

    // splits RAM [addr, addr+size) into runs equal to member contents; Match::Member is a global id
    std::vector<Match> Resolve(const u8* mem, u32 memBase, u32 memLen, u32 addr, u32 size, size_t maxMatches = 32) const
    {
        return Idx.Resolve(mem, memBase, memLen, addr, size, maxMatches);
    }
    // where a member's content is in RAM (whole or in parts); Match::Member is its global id
    std::vector<Match> Locate(int container, int member, const u8* mem, u32 memBase, u32 memLen, size_t maxMatches = 16) const;

    std::string Summary;       // one line for status displays

    int MaxDepth = 8;
    u64 MaxTotal = 512ull << 20;    // content kept in all
    u32 MaxIndexed = 16u << 20;     // larger members are listed but not searched for by content

private:
    void AddMembers(Container& c, u32 blob, u32 base, u32 len, int parent, int depth, u32 romBase);
    std::vector<Container> C;
    std::vector<std::vector<u8>> Blobs;   // container files (unpacked) and unpacked members; never resized after Build
    std::vector<std::pair<int, int>> Global;
    std::vector<std::pair<u32, int>> ById;   // {file id, container}, sorted
    u64 Total = 0;
    ContentIndex Idx;
};

// the files of a NARC or an SDAT: name, offset and size within it. False if it is malformed.
struct ContainerEntry { std::string Name; u32 Offset; u32 Size; };
bool ReadNarc(const u8* p, u32 len, std::vector<ContainerEntry>& out, std::string& error);
bool ReadSdat(const u8* p, u32 len, std::vector<ContainerEntry>& out, std::string& error);
// NCGR for "RGCN", BMD0, SSEQ ...: the kind of a file from its first bytes ("" when they are no magic)
std::string KindOf(const u8* p, u32 len);

}

#endif
