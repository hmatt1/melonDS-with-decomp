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

// Range queries over the recording. Everything here runs on the emulation
// thread (through WBHost::Query / RunSync); the API server and the UI panels
// share these implementations so both see the same answers.

#ifndef WB_QUERIES_H
#define WB_QUERIES_H

#include <QImage>
#include <QString>
#include "WBApi.h"
#include "Workbench.h"
#include "Session.h"
#include "Disasm.h"
#include "GameArchive.h"
#include "Containers.h"
#include "WBClassify.h"

namespace WBQ
{
using melonDS::u32;
using melonDS::u16;
using melonDS::u8;
using melonDS::WB::Workbench;

bool ParseCpu(const QString& s, u32& cpu, QString& err);
QString CpuName(u32 cpu);
QString ModeName(u32 cpsr);
u32 CanonicalAddr(Workbench& wb, u16 region, u32 offset);
QString FlagNames(u16 flags, QStringList* list = nullptr);

WBApi::RegisterSet Registers(Workbench& wb, u32 cpu);
WBApi::TcmLayout Tcm(Workbench& wb);
WBApi::RecordingState Recording(Workbench& wb);

bool MemoryRead(Workbench& wb, const WBApi::MemoryReadParams& p, WBApi::MemoryReadResult& out, QString& err);
// kb: what the knowledge store says about the range (gathered on the main thread), for classification
bool Metadata(Workbench& wb, const melonDS::WB::MergedSessions* all, const WBApi::MetadataParams& p, WBApi::MetadataResult& out, QString& err,
              const KbHints* kb = nullptr, const AnalysisHints* analysis = nullptr);
bool Writes(Workbench& wb, const WBApi::WritesListParams& p, WBApi::WritesListResult& out, QString& err);
// how a data origin label was made (origin.label); emulation thread
bool LabelTree(Workbench& wb, const WBApi::OriginLabelParams& p, WBApi::OriginLabelResult& out, QString& err);
// the same search in any history: a saved session's (no byte records, so no last writer)
bool WritesIn(const melonDS::WB::HistoryView& h, const WBApi::WritesListParams& p, WBApi::WritesListResult& out, QString& err);

struct FuncRange { u32 Start, End; int Cpu; };
bool Coverage(Workbench& wb, const melonDS::WB::MergedSessions* all, const WBApi::CoverageParams& p, const std::vector<FuncRange>& known,
              WBApi::CoverageResult& out, QString& err);
bool Calls(Workbench& wb, const melonDS::WB::MergedSessions* all, u32 cpu, u32 entry, u32 size, u32 limit, WBApi::CallsResult& out);
WBApi::CallStackResult CallStack(Workbench& wb, u32 cpu);
// entry of the function containing addr according to the recording: the nearest observed callee at or
// below addr whose code runs without a gap up to addr (addr itself when nothing qualifies)
u32 InferFunctionEntry(Workbench& wb, const melonDS::WB::MergedSessions* all, u32 cpu, u32 addr);
// detected game archive (WBHost::Archive / ArchiveStatus)
struct ArchiveCtx
{
    std::shared_ptr<const WBArc::ArchiveIndex> Index;
    QString Status = "none";
};
// NARC and SDAT containers (WBHost::Containers / ContainerStatus)
struct ContainerCtx
{
    std::shared_ptr<const WBArc::ContainerIndex> Index;
    QString Status = "none";
};
bool Origin(Workbench& wb, const WBApi::OriginParams& p, WBApi::OriginResult& out, QString& err, const ArchiveCtx& arc = {},
            const ContainerCtx& cont = {});
// RAM ranges whose data origin label includes a source (by name or the end of its name)
bool OriginRanges(Workbench& wb, const WBApi::OriginRangesParams& p, WBApi::OriginRangesResult& out, QString& err);
bool ArchiveList(const ArchiveCtx& arc, const WBApi::ArchiveListParams& p, WBApi::ArchiveListResult& out, QString& err);
bool ArchiveLocate(Workbench& wb, const ArchiveCtx& arc, const WBApi::ArchiveLocateParams& p, WBApi::ArchiveLocateResult& out, QString& err);
// archive runs in main RAM around [addr, addr+size), with loads and writers filled in
std::vector<WBApi::ArchiveSegment> ArchiveSegments(Workbench& wb, const ArchiveCtx& arc, u32 addr, u32 size, size_t max = 16);
bool ContainerList(const ContainerCtx& cont, const WBApi::ContainerListParams& p, WBApi::ContainerListResult& out, QString& err);
bool ContainerLocate(Workbench& wb, const ContainerCtx& cont, const WBApi::ContainerLocateParams& p, WBApi::ContainerLocateResult& out, QString& err);
// runs of files inside containers in main RAM around [addr, addr+size), with loads and writers filled in
std::vector<WBApi::ArchiveSegment> ContainerSegments(Workbench& wb, const ContainerCtx& cont, u32 addr, u32 size, size_t max = 16);
// units: the fields of defined data objects (DataUnits), shown as .byte/.hword/.word/.asciz with their names
bool Disasm(Workbench& wb, const WBApi::DisasmParams& p, WBApi::DisasmResult& out, QString& err, const KbHints* kb = nullptr,
            const std::vector<DataUnit>* units = nullptr, const AnalysisHints* analysis = nullptr);
bool CaptureScreens(Workbench& wb, QImage& top, QImage& bottom, QString& err);

// combined byte record (current recording + merged saved sessions when all != nullptr)
bool ByteAt(Workbench& wb, const melonDS::WB::MergedSessions* all, u32 cpu, u32 addr, melonDS::WB::ByteRecord& out);
// instruction mode at an address: 1 = thumb, 0 = arm, -1 = never executed
int RecordedMode(Workbench& wb, u32 cpu, u32 addr);
// what the recording disagrees with in a decode override's range ("marked data, but executed 12 times"), empty if nothing
QString DecodeConflict(Workbench& wb, u32 cpu, u32 start, u32 end, const QString& mode);
// times the instruction at addr ran this session
u32 ExecCount(Workbench& wb, u32 cpu, u32 addr);
// fetch the instruction word(s) for disassembly, using the ROM image for overlays that are not in RAM
bool FetchCode(Workbench& wb, u32 cpu, u32 addr, bool thumb, u32& word);

// an overlay's bytes as they are loaded into RAM, from the ROM (unpacked when compressed); no BSS
bool OverlayImage(const melonDS::WB::NitroROM& rom, const melonDS::WB::OverlayInfo& ov, std::vector<u8>& out);

// what the recording saw referring to addr (the Cross-references panel): calls into it as a function
// entry (fsize: the function's size, 0 if unknown), writes, reads, and executed literal-pool words holding it
struct XrefRow { QString Kind; u32 From; QString Source; melonDS::u64 Count; };
void RuntimeXrefs(Workbench& wb, const melonDS::WB::MergedSessions* all, u32 cpu, u32 addr, u32 fsize, std::vector<XrefRow>& rows);

// ---- search (the Search panel): ARM9 view of main RAM, or also shared WRAM, DTCM and ITCM (wide)
struct SearchHit { u32 Addr; QString Preview; };
// hex bytes with ?? wildcards: "12 34 ?? 56" or "1234??56"
bool ParseBytePattern(const QString& text, QByteArray& pat, QByteArray& mask, QString& err);
// encoding: ascii (Latin-1), sjis or utf16le
bool EncodeSearchText(const QString& text, const QString& encoding, QByteArray& out, QString& err);
std::vector<SearchHit> FindPattern(Workbench& wb, const QByteArray& pat, const QByteArray& mask, bool wide, size_t max);

// value scans; both enums are in the Search panel's order
enum class ScanType { U8, U16, U32, S32, FX32 };
enum class ScanCond { Equal, NotEqual, Changed, Unchanged, Increased, Decreased, Greater, Less, Unknown };
struct ScanState
{
    ScanType Type = ScanType::U32;
    std::vector<u32> Addrs, Values;      // candidates and their values at the last scan
    u32 Size() const { return Type == ScanType::U8 ? 1 : Type == ScanType::U16 ? 2 : 4; }
};
bool ScanNeedsValue(ScanCond c);
// decimal or 0x hex; fx32 takes a decimal such as 1.5
bool ParseScanValue(const QString& text, ScanType type, melonDS::s64& out);
// a value the type can hold: one outside it would never match
bool ScanValueInRange(ScanType type, melonDS::s64 value, QString& err);
double ScanDecode(ScanType type, u32 raw);
// a first scan keeps the values matching an equal / not equal / greater / less condition, or every value
void ScanFirst(Workbench& wb, ScanState& s, ScanCond cond, melonDS::s64 value, bool wide);
void ScanNext(Workbench& wb, ScanState& s, ScanCond cond, melonDS::s64 value);

}

#endif
