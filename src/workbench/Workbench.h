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

// DS Decomp Workbench core: recording engine, data origin tracking and debugger.
// Everything here runs on the emulation thread. Other threads reach it through
// Debugger::PostJob(), which runs work at a safe point (between frames, while
// paused, or while stopped at a breakpoint).

#ifndef WB_WORKBENCH_H
#define WB_WORKBENCH_H

#include <atomic>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "types.h"
#include "WBRecords.h"
#include "NitroROM.h"
#include "InstrOps.h"

namespace melonDS
{
class NDS;
class ARM;
}

namespace melonDS::WB
{

constexpr u32 PageShift = 12;
constexpr u32 PageSize = 1u << PageShift;
constexpr u32 BlockShift = 6;                 // 64-byte blocks for "last N frames" coverage
constexpr u32 BlocksPerPage = PageSize >> BlockShift;

inline constexpr u16 RegionId(Region r) { return (u16)r; }
inline bool IsOverlayRegion(u16 r) { return r >= (u16)Region::OverlayARM9Base && r < (u16)Region::RegionMax; }
std::string RegionLabel(u16 region);
u32 RegionSize(u16 region);

struct PhysAddr
{
    u16 Region = 0;     // WB::Region, 0 = unmapped / not recorded
    u32 Offset = 0;     // for overlay regions: main RAM offset (overlay records are aligned to main RAM)
    bool Recordable() const { return Region != 0 && Region != (u16)Region::IO9 && Region != (u16)Region::IO7 && Region != (u16)Region::GBASlot && Region != (u16)Region::WifiRAM; }
};

enum BlockKind { Block_Exec = 0, Block_Read, Block_Write, Block_DMA };

struct RecordPage
{
    ByteRecord Rec[PageSize];
    u32 BlockFrame[BlocksPerPage][4];   // last frame each 64-byte block saw exec/read/write/DMA
    u32 Readers[PageSize / 4][2];       // last two distinct CPU reader PCs per 32-bit word
    // 1 + the first frame each byte saw exec/read/write/DMA (0 = never). Set when the byte's counter is
    // still 0, so after the first access it costs nothing
    u32 First[PageSize][4];
};

struct LabelPage
{
    u32 L[PageSize];
};

// Sparse two-level table: region -> page index -> page.
template <typename T>
class SparseStore
{
public:
    ~SparseStore() { Clear(); }
    void Clear()
    {
        for (auto& v : Pages)
            for (T* p : v) delete p;
        for (auto& v : Pages) v.clear();
        Count = 0;
    }
    T* Get(u16 region, u32 page) const
    {
        if (region >= (u16)Region::RegionMax) return nullptr;
        const auto& v = Pages[region];
        return page < v.size() ? v[page] : nullptr;
    }
    T* GetOrAlloc(u16 region, u32 page)
    {
        if (region >= (u16)Region::RegionMax) return nullptr;
        auto& v = Pages[region];
        if (page >= v.size())
        {
            u32 max = (RegionSize(region) + PageSize - 1) >> PageShift;
            if (page >= max) return nullptr;
            v.resize(max, nullptr);
        }
        T*& p = v[page];
        if (!p)
        {
            p = new T();
            memset((void*)p, 0, sizeof(T));
            Count++;
        }
        return p;
    }
    template <typename F> void ForEach(F&& f) const
    {
        for (u16 r = 0; r < (u16)Region::RegionMax; r++)
            for (u32 i = 0; i < Pages[r].size(); i++)
                if (Pages[r][i]) f(r, i, Pages[r][i]);
    }
    u32 PageCount() const { return Count; }
private:
    std::vector<T*> Pages[(u16)Region::RegionMax];
    u32 Count = 0;
};

using RecordStore = SparseStore<RecordPage>;
using LabelStore = SparseStore<LabelPage>;

struct PageCache
{
    u32 VPage = 0xFFFFFFFF;
    u32 Gen = 0;
    RecordPage* Page = nullptr;
};

struct CallEdgeKey
{
    u32 CallSite, Callee;
    bool operator==(const CallEdgeKey& o) const { return CallSite == o.CallSite && Callee == o.Callee; }
};
struct CallEdgeKeyHash { size_t operator()(const CallEdgeKey& k) const { return ((u64)k.CallSite << 32 | k.Callee) * 0x9E3779B97F4A7C15ULL >> 16; } };

struct DataSource
{
    u32 Id;
    SourceKind Kind;
    std::string Name;
    bool Enabled = true;
    u32 FileId = FileNone;      // RomFile
    u32 Addr = 0, Size = 0;     // Range; RomFile: Addr = archived file index + 1 inside a game archive, 0 = the whole file
    u32 Cpu = 0, Reg = 0;       // Register
};

class Workbench;

// RAM bytes [Addr, Addr+Size) equal to a file inside a game archive (members: index + 1, identical files too)
struct SubFileRun { u32 Addr, Size, FileId; u32 Members[8]; u32 Count; };

// ---------------------------------------------------------------- data origin (taint)

class Origin
{
public:
    explicit Origin(Workbench& wb) : WB(wb) {}

    void Reset();
    void Enable(bool on);
    bool Enabled() const { return On; }

    // sources
    u32 AddSource(DataSource src);              // returns id
    bool RemoveSource(u32 id);
    const std::vector<DataSource>& Sources() const { return SourceList; }
    void SetAllRomFiles(bool on);
    bool AllRomFiles() const { return AllRomFilesOn; }
    // table lookups: a loaded value also takes the labels of the registers that formed its address
    void SetFollowAddresses(bool on) { FollowAddr = on; }
    bool FollowAddresses() const { return FollowAddr; }
    // 0 if ROM file tracking does not cover it. With the ROM offset read, bytes of a file inside a
    // game archive get a source of their own (Workbench::SubFile)
    u32 SourceForRomFile(u32 fileid, u32 romoff = 0xFFFFFFFF);
    u32 SourceFor(u32 fileid, u32 member);      // source of a ROM file or of a file archived in it (member = index + 1)
    const DataSource* FindSource(u32 id) const;

    // labels
    u32 LabelForSource(u32 srcid);
    u32 Union(u32 a, u32 b);
    const std::vector<u32>& LabelSources(u32 label) const;
    u32 LabelCount() const { return (u32)Labels.size(); }

    // How each label naming several sources was first made: the two labels it joined, and the
    // instruction, CPU, frame and kind of operation that joined them. Recorded only when a new label
    // is created, so it costs nothing per instruction. A = B = 0: the label names one source
    enum MergeOp : u8 { MergeNone = 0, MergeAlu, MergeAddress, MergeBytes, MergeMath, MergeCopy, MergeSource };
    struct LabelMerge { u32 A = 0, B = 0, PC = 0, Frame = 0; u8 Cpu = 0, Op = MergeNone; };
    const LabelMerge& MergeOf(u32 label) const;
    static const char* MergeOpName(u8 op);
    void SerializeMerges(std::vector<u8>& out) const;      // LabelMergeEntry records

    u32 GetMemLabel(PhysAddr pa) const;
    void SetMemLabel(PhysAddr pa, u32 label);
    void SetRangeLabel(u32 cpu, u32 addr, u32 size, u32 label, bool merge);

    // Content seeding for files inside a game archive, and for plain ROM files in a BIOS compression
    // format. Taint through an unpacker (zlib) is lossy and
    // leaks through state it keeps between files, so when a tracked archived file's unpacked bytes
    // appear in RAM they are labelled with that file directly, chunk by chunk (games often stream a
    // file through a small buffer). Pending files are those whose bytes were just read from the card;
    // unpackers write their output sequentially, so new bytes of contiguous write runs are matched
    // against the archive (Workbench::SubFileResolve) when a function returns.
    void TrackWriteRun(u32 cpu, u32 addr, u32 size)
    {
        if (!Pending.empty() && (addr >> 24) == 0x02) NoteWriteRun(cpu, addr, size);
    }
    void OnReturn(u32 cpu) { if (!Pending.empty()) CheckSeeds(cpu); }
    u32 SeededFiles() const { return Seeded; }

    // propagation, called from Workbench hooks
    // regs: the CPU's registers before the instruction (used when following table lookups)
    void PreExec(u32 cpu, u32 instr, bool thumb, bool condpass, const u32* regs = nullptr);
    void PostExec(u32 cpu);
    void OnRead(u32 cpu, u32 addr, u32 size, u32 fixedlabel, u32 value = 0);
    void OnWrite(u32 cpu, u32 addr, u32 size);
    void OnDMA(u32 cpu, u32 src, u32 dst, u32 size, u32 fixedlabel);
    void OnModeChange(u32 cpu, u32 oldmode, u32 newmode);
    void CopyRange(u32 cpu, u32 src, u32 srcsize, u32 dst, u32 dstsize);

    u32 RegLabel(u32 cpu, u32 reg) const { return Reg[cpu][reg]; }
    void SetRegLabel(u32 cpu, u32 reg, u32 label) { Reg[cpu][reg] = label; AddrReg[cpu][reg] = label; UpdateNonZero(cpu); }

    // byContent: pick the overlay at each address by comparing RAM with the overlays' images (after a
    // savestate load, when the recorded overlay ownership may be stale)
    void SeedStaticModules(bool byContent = false);

    // A savestate replaced memory, so the labels describe what was there before (UI_REQUIREMENTS SS-1).
    // They are dropped; with data origin on, what can be proven by content is labelled again: ROM
    // modules and overlays that still equal the ROM, archived and packed ROM files found in RAM.
    // Returns the bytes labelled again.
    melonDS::u64 RebuildAfterStateLoad();
    melonDS::u32 StateLoadedFrame() const { return LoadedFrame; }

    // serialization
    void Serialize(std::vector<u8>& labels, std::vector<u8>& sources) const;
    const LabelStore& MemLabels() const { return Mem; }

private:
    void UpdateNonZero(u32 cpu);
    u32 AccessLabel(u32 cpu, u32 addr, u32 size) const;

    Workbench& WB;
    bool On = false;
    bool AllRomFilesOn = true;
    bool FollowAddr = false;
    u32 AddrLabel[2] {};

    std::vector<DataSource> SourceList;
    u32 NextSourceId = 1;
    std::unordered_map<u64, u32> RomFileSource;   // (file id << 32 | archived file + 1) -> source id
    // Members: a file packed whole that holds archived files (FilePackedMembers), labelled per archived file
    struct PendingFile { u32 FileId, Member, Source, Frame; bool Members = false; };
    struct WriteRun { u32 Start = 0, End = 0, Checked = 0; bool Grown = false; };
    std::vector<PendingFile> Pending;
    WriteRun Runs[2][8];
    u32 RunNext[2] = {0, 0};
    u32 Seeded = 0;              // seeded runs
    std::vector<SubFileRun> SeedRuns;
    void NoteWriteRun(u32 cpu, u32 addr, u32 size);
    void CheckSeeds(u32 cpu);
    static u64 RomKey(u32 fileid, u32 member) { return ((u64)fileid << 32) | member; }
    std::unordered_map<u32, u32> SourceLabel;     // source id -> singleton label

    std::vector<std::vector<u32>> Labels;          // label -> sorted source ids, [0] = empty
    std::vector<LabelMerge> Made;                  // label -> how it was made (parallel to Labels)
    u8 OpCpu = 0, Op = MergeNone;                  // what the next Union is (UnionAt)
    u32 UnionAt(u32 a, u32 b, u32 cpu, u8 op) { OpCpu = (u8)cpu; Op = op; return Union(a, b); }
    // A label is its sources and where they were joined (MergeSite: CPU, kind of merge and instruction;
    // 0 for one source), so the same files joined in two places are two labels, each with its own history
    static u64 MergeSite(u32 cpu, u8 op, u32 pc) { return ((u64)(cpu & 1) << 40) | ((u64)op << 32) | pc; }
    std::map<std::pair<std::vector<u32>, u64>, u32> LabelIndex;
    struct UnionKey
    {
        u32 A, B;
        u64 Site;
        bool operator==(const UnionKey& o) const { return A == o.A && B == o.B && Site == o.Site; }
    };
    struct UnionKeyHash
    {
        size_t operator()(const UnionKey& k) const { return std::hash<u64>()((((u64)k.A << 32) | k.B) * 0x9E3779B97F4A7C15ull ^ k.Site); }
    };
    std::unordered_map<UnionKey, u32, UnionKeyHash> UnionCache;
    u32 ConstLabel = 0;

    LabelStore Mem;

    u32 Reg[2][16] {};
    u32 RegFIQ[2][7] {}, RegIRQ[2][2] {}, RegSVC[2][2] {}, RegABT[2][2] {}, RegUND[2][2] {};
    u16 NonZero[2] {};
    // following table lookups: each register also has an address label, the part of its label a load
    // through it takes. Pointer arithmetic keeps only the data operand's part, and a pointer loaded from
    // memory has none, so pointers don't spread their history to everything read through them
    u32 AddrReg[2][16] {};
    u32 PendingAddrLabel[2] {};
    // registers known to hold pointers: used as an address since written, or derived from one. These
    // contribute only their address label to arithmetic and stores, so pointers don't accumulate labels;
    // data that only looks like an address is never used as one and keeps its full label
    u16 Pointer[2] {};
    u16 PendingPointer[2] {};
    // the banked registers' address labels and pointer flags, as their labels (FIQ, IRQ, SVC, ABT, UND)
    u32 AddrFIQ[2][7] {}, AddrIRQ[2][2] {}, AddrSVC[2][2] {}, AddrABT[2][2] {}, AddrUND[2][2] {};
    u16 PointerBank[2][5] {};
    // the ARM9 math unit: labels of DIV_NUMER/DIV_DENOM (0x04000290-0x0400029F, words 0-3) and
    // SQRT_PARAM (0x040002B8-0x040002BF, words 4-5) reach its result registers
    u32 MathLabel[6] {};
    u32 LoadedFrame = 0;            // frame of the last savestate load, 0 = none
    u32 MathResultLabel(u32 addr);

    // per-instruction state
    InstrOps Ops[2];
    bool OpsActive[2] {};
    u32 PendingLabel[2] {};
    u32 LoadIdx[2] {}, StoreIdx[2] {};
    u32 StoreLabels[2][17] {};
};

// ---------------------------------------------------------------- debugger

struct Expr;

struct Breakpoint
{
    int Id;
    BreakKind Kind;
    u32 Addr, Size;
    int Cpu;            // 0, 1, or -1 for any
    std::string Condition;
    std::shared_ptr<Expr> Compiled;
    bool Enabled = true;
    bool Temporary = false;
    u32 HitCount = 0;
    std::string Author;
};

struct BreakHit
{
    int Id;
    u32 Cpu;
    u32 PC;
    u32 Frame;
    u32 AccessAddr = 0, AccessValue = 0, AccessSize = 0;
    bool Write = false;
};

struct BreakState
{
    u32 Cpu = 0;
    u32 PC = 0;
    std::string Reason;
    int BreakpointId = -1;
    u32 AccessAddr = 0, AccessValue = 0, AccessSize = 0;
    u32 Frame = 0;
    u32 HitPC = 0;      // watchpoints: the instruction that made the access
};

struct WBEvent
{
    enum Kind { BreakHitEvent, BreakLeftEvent, RecordingChangedEvent, OverlayLoadedEvent } Type;
    BreakState Break;
    BreakHit Hit;
};

struct EvalContext
{
    Workbench* WB;
    u32 Cpu;
    u32 AccessAddr, AccessValue, AccessSize;
    u32 Hits;
};

std::shared_ptr<Expr> CompileExpr(const std::string& text, std::string& error);
s64 EvalExpr(const Expr& e, const EvalContext& ctx);

class Debugger
{
public:
    explicit Debugger(Workbench& wb) : WB(wb) {}

    // breakpoints (emu thread)
    int AddBreakpoint(Breakpoint bp, std::string& error);
    bool RemoveBreakpoint(int id);   // -1 = all
    bool SetBreakpointEnabled(int id, bool en);
    // empty removes the condition; the hit count is kept
    bool SetBreakpointCondition(int id, const std::string& cond, std::string& error);
    std::vector<Breakpoint> List() const { return Breakpoints; }
    std::vector<BreakHit> Hits() const { return std::vector<BreakHit>(HitList.begin(), HitList.end()); }
    bool HasExecBreakpoints() const { return NumExec > 0; }
    bool HasWatchpoints() const { return NumWatch > 0; }

    // execution control (any thread)
    void RequestBreak(int cpu, const char* reason);   // break before the next instruction of cpu (-1 = any)
    void RequestStep(int cpu, u32 count);             // run count instructions on cpu, then break
    void Continue();                                  // leave the break state
    void CancelStep();
    bool InBreak() const { return BreakActive.load(); }
    BreakState CurrentBreak() const { std::lock_guard<std::mutex> l(StateLock); return Break; }
    void SuppressUntilFrameEnd() { Suppressed = true; }
    void FrameEnd() { Suppressed = false; }

    // jobs (any thread -> emu thread)
    void PostJob(std::function<void()> job);
    bool ServiceJobs();                               // emu thread; returns true if any ran
    bool WaitJobs(int timeoutms);                     // emu thread, when idle

    // hooks
    bool CheckExec(u32 cpu, u32 pc);                  // true -> break before this instruction
    void CheckAccess(u32 cpu, u32 addr, u32 size, u32 value, bool write);
    void EnterBreak(u32 cpu, u32 pc);                 // blocks until Continue()

    // host integration
    std::function<bool()> HostWantsAbort;             // true when the frontend needs the emu thread back
    std::function<void()> HostIdle;                   // called periodically while in break

    bool ExecHooksNeeded() const { return NumExec > 0 || PendingBreak.load() || StepActive.load(); }

    u8* ExecPages[2] = {nullptr, nullptr};            // per 4KB page: has exec breakpoints
    u8* WatchPages = nullptr;                         // per 4KB page: has read/write breakpoints

private:
    void RebuildIndex();
    bool EvalCondition(Breakpoint& bp, u32 cpu, u32 addr, u32 value, u32 size);
    void RecordHit(Breakpoint& bp, u32 cpu, u32 pc, u32 addr, u32 value, u32 size, bool write);

    Workbench& WB;
    std::vector<Breakpoint> Breakpoints;
    int NextId = 1;
    int NumExec = 0, NumWatch = 0;
    std::deque<BreakHit> HitList;

    std::atomic<bool> PendingBreak {false};
    std::atomic<int> PendingCpu {-1};
    std::string PendingReason;
    std::atomic<bool> StepActive {false};
    std::atomic<int> StepCpu {0};
    std::atomic<u32> StepRemaining {0};
    bool Suppressed = false;

    BreakState NextBreak;
    WBEvent LastHitEvent {};
    bool HaveHitEvent = false;

    // a watchpoint hit during an instruction stops before the next one
    bool WatchHitPending = false;
    BreakState WatchBreak;

    mutable std::mutex StateLock;
    BreakState Break;
    std::atomic<bool> BreakActive {false};
    std::atomic<bool> ContinueReq {false};

    std::mutex JobLock;
    std::condition_variable JobCond;
    std::deque<std::function<void()>> Jobs;
};

// ---------------------------------------------------------------- recorder

struct PendingDecompress
{
    bool Active = false;
    u32 Swi = 0;
    u32 Src = 0, Dst = 0;
    u32 ReturnAddr = 0;
    u32 PC = 0;
    u32 Frame = 0;
    u32 ReadMin = 0xFFFFFFFF, ReadMax = 0, WriteMin = 0xFFFFFFFF, WriteMax = 0;
};

struct CardWord
{
    bool Valid = false;
    u32 RomOffset = 0;
    u32 Value = 0;
};

struct OpenTransfer
{
    bool Active = false;
    ProvenanceEvent Ev {};
    u32 NextSrc = 0, NextDst = 0;
    s32 SrcStep = 0, DstStep = 0;
};

// Write history as a ring of slots, the running session's or a saved one's: `Used` valid entries,
// the newest at (Pos - 1) % Slots. Pos counts every entry ever appended, so Pos > Used once the
// oldest were dropped
struct HistoryView
{
    const WriteHistoryEntry* Data = nullptr;
    u64 Slots = 0, Pos = 0, Used = 0;
    bool Wrapped() const { return Pos > Used; }
    u32 OldestFrame() const { return Slots && Used ? Data[(Pos - Used) % Slots].frame : 0; }
    template <typename F> void ForEach(F&& f) const       // oldest first
    {
        if (!Slots || !Used) return;
        u64 start = (Pos - Used) % Slots;
        for (u64 i = 0; i < Used; i++) f(Data[(start + i) % Slots]);
    }
};

class Recorder
{
public:
    explicit Recorder(Workbench& wb);

    void Reset();                           // clears all recorded data
    void SetEnabled(bool on);
    void SetWriteHistory(bool on);
    void SetHistoryCap(u64 entries);
    // which writes write history keeps. Bulk: the bytes of a sequential run (a copy, fill or unpacker's
    // output) beyond the first BulkThreshold are skipped; 0 keeps everything
    struct HistoryFilter
    {
        std::vector<std::pair<u32, u32>> Include;           // only writes to these [start, end) ranges; empty = all
        std::vector<std::pair<u32, u32>> ExcludeWriters;    // not writes by code in these [start, end) ranges
        u32 BulkThreshold = 1024;
        bool SkipStack = true;                              // CPU writes near the writer's stack pointer (spills, locals)
        bool SkipUnchanged = true;                          // writes that store the value already there
    };
    void SetHistoryFilter(const HistoryFilter& f);     // keeps the ranges sorted and merged
    const HistoryFilter& GetHistoryFilter() const { return HFilter; }
    bool HistoryIncludes(u32 addr, u32 size) const;     // the include ranges let a write to [addr, addr+size) in
    bool HistoryExcludesWriter(u32 pc) const;
    u64 HistorySkipped() const { return Skipped; }

    bool Enabled() const { return On; }
    bool WriteHistoryOn() const { return HistoryOn; }
    u64 HistoryCap() const { return Cap; }
    u64 HistoryCount() const { return HistoryUsed; }
    // history kept in a caller-owned buffer (a memory-mapped history file) instead of memory; nullptr
    // returns to the in-memory buffer. Either way the current entries are discarded
    void SetHistoryStorage(WriteHistoryEntry* buffer, u64 entries);
    bool HistoryInFile() const { return Ext != nullptr; }
    bool HistoryWrapped() const { return HistoryPos > HistoryUsed; }        // the oldest entries were dropped
    u32 HistoryOldestFrame() const;
    u64 SessionHistoryMax = 4000000;            // newest entries a session file keeps
    u32 SessionStartFrame() const { return StartFrame; }

    // hooks
    void OnExec(u32 cpu, u32 pc, bool thumb);
    void OnPostExec(u32 cpu, ARM* arm, u32 pc, bool thumb, u32 instr);
    void OnDiscontinuity(u32 cpu, ARM* arm, u32 pc);
    void OnRead(u32 cpu, u32 addr, u32 size, u32 value);
    void OnWrite(u32 cpu, u32 addr, u32 size, u32 value, u32 oldvalue, PhysAddr pa);
    void OnDMA(u32 cpu, u32 channel, u32 src, u32 dst, u32 size, u32 value);
    void OnCardCommand(const u8* cmd);
    void OnCardData(u32 value);
    void OnTcmChange(u32 itcmsize, u32 dtcmbase, u32 dtcmsize);
    void OnFrameEnd();
    void OnBootLoad();
    void LogEvent(RecEventKind kind, u16 value = 0);

    // overlay ownership of main RAM
    int OverlayAt(u32 mainoff) const;       // overlay owning a main RAM offset (encoded cpu<<15|id), -1 if static
    void MarkOverlayLoaded(u32 cpu, u32 id, u8 kind = 0);
    // "written, then executed": after code bytes in an overlay range are rewritten (or a savestate
    // is loaded), the next instruction run there is compared with the overlays' ROM images
    void ComputeOverlaySpans();
    void InvalidateOverlays() { OvlDirty[0] = OvlDirty[1] = true; }

    // queries
    const RecordStore& Store() const { return Records; }
    const ByteRecord* Get(PhysAddr pa) const;
    u64 HistoryHead() const { return HistoryPos; }
    template <typename F> void ForEachHistory(F&& f) const;   // oldest first
    HistoryView LiveHistory() const { return {HistData(), HistSlots(), HistoryPos, HistoryUsed}; }
    const std::unordered_map<CallEdgeKey, CallEdge, CallEdgeKeyHash>& Edges(u32 cpu) const { return CallEdges[cpu]; }
    const std::vector<ShadowFrame>& ShadowStack(u32 cpu) const { return Stack[cpu]; }
    // function entry on top of the shadow call stack when the instruction at pc first ran (0 = unknown)
    u32 FirstRunIn(u32 cpu, u32 pc) const
    {
        auto it = FirstFunc.find(((u64)cpu << 32) | pc);
        return it == FirstFunc.end() ? 0 : it->second;
    }
    const std::vector<ProvenanceEvent>& Provenance() const { return ProvList; }
    const std::vector<OverlayEvent>& OverlayEvents() const { return OvlEvents; }
    const std::vector<TcmEvent>& TcmEvents() const { return TcmList; }
    const std::vector<RecEvent>& Events() const { return EventLog; }

    u32 FramesRecorded() const;

    // executed call targets and recognised prologues. TakeEntries copies them once per change.
    struct ObservedEntry { u32 Cpu = 0; u32 Addr = 0; bool Thumb = false; };
    bool TakeEntries(std::vector<ObservedEntry>& out);

    // function entries the knowledge store knows, so branches into functions never seen called still count as tail calls
    void SetKnownFunctions(u32 cpu, const std::vector<u32>& entries) { KbEntries[cpu & 1] = std::unordered_set<u32>(entries.begin(), entries.end()); }
    void FlushTransfers();

    // session serialization (pages, edges, events ...)
    void SerializeSession(std::vector<u8>& out, u32 flags) const;

    PageCache ExecCache[2], ReadCache[2], WriteCache[2];

private:
    friend class Workbench;
    friend class Origin;
    RecordPage* PageFor(u32 cpu, u32 addr, PageCache& cache, bool alloc);
    void Touch(PhysAddr pa, u32 size, int kind);
    void AddProvenance(const ProvenanceEvent& ev);
    void PushFrame(u32 cpu, const ShadowFrame& f);
    void AddEdge(u32 cpu, u32 callsite, u32 callee, u32 callerfunc, CallKind kind, bool thumb);
    void HandleCardWordStored(u32 cpu, u32 dst, u32 dstsize, PhysAddr pa, bool dma, u32 channel);
    void FlushTransfer(OpenTransfer& t);

    Workbench& WB;
    bool On = false;
    bool HistoryOn = false;
    u64 Cap = 4000000;
    u32 StartFrame = 0, EndFrame = 0;

    RecordStore Records;

    std::vector<WriteHistoryEntry> History;
    WriteHistoryEntry* Ext = nullptr;           // SetHistoryStorage
    const WriteHistoryEntry* HistData() const { return Ext ? Ext : History.data(); }
    u64 HistSlots() const { return Ext ? Cap : History.size(); }
    void PushHistory(const WriteHistoryEntry& e)
    {
        if (Ext) Ext[HistoryPos % Cap] = e;
        else if (History.size() < Cap) History.push_back(e);
        else History[HistoryPos % Cap] = e;
        HistoryPos++;
        if (HistoryUsed < Cap) HistoryUsed++;
    }
    u64 HistoryPos = 0;      // total entries ever appended
    u64 HistoryUsed = 0;

    std::unordered_map<CallEdgeKey, CallEdge, CallEdgeKeyHash> CallEdges[2];
    std::vector<ShadowFrame> Stack[2];
    u32 ExpectedPC[2] = {0xFFFFFFFF, 0xFFFFFFFF};
    std::unordered_map<u64, u32> FirstFunc;     // (cpu << 32 | pc) -> function entry, see FirstRunIn
    std::unordered_set<u32> KnownEntries[2];    // observed callees: a plain branch to one is a tail call
    std::unordered_map<u32, u8> EntryMode[2];   // executed entries: addr -> 1 thumb, 0 arm
    u32 EntryGen = 0;
    u32 IdentifiedGen = 0;
    bool PrologueScanned = false;
    bool DidIdentify = false;
    void NoteEntry(u32 cpu, u32 addr, bool thumb);
    void ScanExecutedPrologues();
    std::unordered_set<u32> KbEntries[2];       // function entries from the knowledge store (SetKnownFunctions)
    HistoryFilter HFilter;
    u64 Skipped = 0;
    struct BulkRun { u32 End = 0, Len = 0; };
    BulkRun Bulk[4][4];                         // per writer (ARM9, ARM7, DMA9, DMA7): recent sequential runs
    u32 BulkNext[4] {};
    bool HistoryWanted(u32 writer, u32 addr, u32 size, u32 pc, u32 sp, bool unchanged);

    std::vector<ProvenanceEvent> ProvList;
    std::vector<OverlayEvent> OvlEvents;
    std::vector<TcmEvent> TcmList;
    std::vector<RecEvent> EventLog;

    // overlay ownership (per 4-byte granule of main RAM, 0 = static, else (cpu<<15 | id) + 1)
    std::vector<u16> Owner;
    u32 OvlSpanStart[2] = {0, 0}, OvlSpanSize[2] = {0, 0};   // RAM covered by each CPU's overlays
    bool OvlDirty[2] = {false, false};
    bool InOverlaySpan(u32 cpu, u32 addr) const { return addr - OvlSpanStart[cpu] < OvlSpanSize[cpu]; }
    // unpacked images of compressed overlays, made when first needed ((cpu << 16 | id) -> image, empty
    // if the overlay does not unpack to its RAM size)
    std::unordered_map<u32, std::vector<u8>> OvlImages;
    // the RAM image of an overlay: its ROM file, or the unpacked file if compressed (nullptr if unknown)
    const u8* OverlayImage(const OverlayInfo& o, u32& len);
    void NoteOverlayWrite(u32 addr, u16 flags);              // a write that may replace overlay code
    void VerifyOverlay(u32 cpu, u32 pc);
    std::vector<u8> PageMixed;   // per main RAM page: ownership differs inside the page

    // card stream
    bool CardActive = false;
    u32 CardAddr = 0, CardWordIdx = 0;
    CardWord LastCard[2];
    OpenTransfer CardCopy[2];        // CPU copy loops from the card data port
    OpenTransfer DmaOpen[8];
    PendingDecompress Decomp[2];

    bool LiteralLoad[2] {};
    u32 CurPC[2] {};

    // CPU copy detection: a store of a value that was just loaded continues a copy (memcpy and friends)
    struct LoadRec { u32 Addr; u32 Value; u8 Size; };
    LoadRec Loads[2][16] {};
    u32 LoadPos[2] {};
    OpenTransfer CpuCopy[2];
    void TrackCopy(u32 cpu, u32 src, u32 dst, u32 size);
    void FlushCopy(OpenTransfer& t);
};

// ---------------------------------------------------------------- top level

class Workbench
{
public:
    Workbench();
    ~Workbench();

    void Attach(melonDS::NDS* nds);       // emu thread; nds may be nullptr
    void OnRomLoaded(const u8* rom, u32 romlen, u32 filelen);
    void OnReset();
    void OnDirectBoot();                  // the static modules were copied from the ROM into RAM
    void OnStateLoaded();
    melonDS::NDS* GetNDS() const { return NDS; }

    // mode selection
    void UpdateHookFlags();
    bool JITActive() const;

    // hooks called from the core (see WBHooks.h)
    bool PreExec(melonDS::ARM* cpu, u32 pc, u32 instr, bool thumb);
    // an instruction starts in `mode` (CPSR & 0x1F), the first of an IRQ handler when irqEntry:
    // data origin labels follow the register banks
    void TrackMode(u32 cpu, u32 mode, bool irqEntry);
    void PostExec(melonDS::ARM* cpu);
    void CpuRead(u32 cpu, u32 addr, u32 size, u32 value);
    void CpuWrite(u32 cpu, u32 addr, u32 size, u32 value);
    void DMAUnit(u32 cpu, u32 channel, u32 src, u32 dst, u32 size, u32 value);
    void CardCommand(const u8* cmd);
    void CardData(u32 value);
    void TcmChanged();
    void MappingChanged() { MapGen++; }
    void FrameEnd();

    // address helpers
    PhysAddr Translate(u32 cpu, u32 addr) const;        // cpu 0/1; 2/3 = DMA9/DMA7 (no TCM)
    u8* PhysPtr(PhysAddr pa) const;                    // backing memory, nullptr for IO/unmapped
    bool Peek(u32 cpu, u32 addr, u32 size, u32& out) const;   // side-effect free read
    bool Poke(u32 cpu, u32 addr, u32 size, u32 val);
    u32 ReadReg(u32 cpu, u32 reg) const;                // 15 = executing instruction address
    u32 CPSR(u32 cpu) const;
    u32 CurrentPC(u32 cpu) const;
    u32 Frame() const;
    u64 Timestamp() const;
    bool PCInRecordedThumb(u32 cpu, u32 addr) const;

    NitroROM Rom;
    Recorder Rec;
    Origin Taint;
    Debugger Dbg;

    std::function<void(const WBEvent&)> OnEvent;       // emu thread -> frontend
    // game archive resolver (installed by the frontend when it has indexed one): which archived file a
    // ROM offset of a ROM file belongs to (index + 1, 0 = none or not file data), and that file's name
    std::function<u32(u32 fileId, u32 romOffset)> SubFile;
    std::function<std::string(u32 fileId, u32 member)> SubFileName;
    // runs of RAM [addr, addr+size) equal to archived files' contents (which & 1), or to plain ROM files
    // and their unpacked forms (which & 2, member 0)
    std::function<void(const u8* mem, u32 memBase, u32 memLen, u32 addr, u32 size, u32 which, std::vector<SubFileRun>& out)> SubFileResolve;
    // plain ROM files SubFileResolve knows: once read, their copies and unpacked forms are labelled by content
    std::function<bool(u32 fileId)> FileIndexed;
    // ROM files packed whole that hold archived files (a compressed NARC): once unpacked, the archived files
    // are found by content (SubFileResolve, which & 1) and labelled as themselves
    std::function<bool(u32 fileId)> FilePackedMembers;
    // archived file inner (index + 1) is inside archived file outer, itself an archive (a nested NARC): once
    // outer is unpacked, inner's bytes are labelled as inner
    std::function<bool(u32 fileId, u32 outer, u32 inner)> SubFileInside;

    u32 MapGen = 1;
    u32 Cur[2] = {0, 0};          // address of the instruction being executed
    bool CurThumb[2] = {false, false};
    u32 CurInstr[2] = {0, 0};
    u32 PrevMode[2] = {0x13, 0x13};

private:
    melonDS::NDS* NDS = nullptr;
};

template <typename F> void Recorder::ForEachHistory(F&& f) const
{
    u64 nslots = HistSlots();
    if (!nslots || !HistoryUsed) return;
    const WriteHistoryEntry* d = HistData();
    u64 n = HistoryUsed;
    u64 start = (HistoryPos - n) % nslots;
    for (u64 i = 0; i < n; i++)
        f(d[(start + i) % nslots]);
}

}

#endif
