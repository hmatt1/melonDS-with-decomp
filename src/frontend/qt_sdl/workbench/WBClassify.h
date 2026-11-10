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

#ifndef WB_CLASSIFY_H
#define WB_CLASSIFY_H

#include <vector>
#include <QString>

#include "types.h"
#include "WBRecords.h"

namespace WBKB { class KnowledgeStore; }

// Is an address ARM code, Thumb code or data, and how sure is that? One answer for the disassembly,
// the inspector, the functions list, the coverage map and the API (UI_REQUIREMENTS CD-1).
namespace WBQ
{

enum class CodeKind { Unknown, Arm, Thumb, Mixed, Data };

// where the answer comes from, strongest first
enum class Evidence
{
    None,         // nothing known: unknown
    Recorded,     // confirmed: executed (in that mode), or read / a literal pool and never executed
    Override,     // a person or an agent set it
    Defined,      // a data object in the knowledge store
    Function,     // inferred: inside a knowledge-store function, never executed
    Analysis,     // from analysis: marked, while paused, by following code that did run
};

struct Classification
{
    CodeKind Kind = CodeKind::Unknown;
    Evidence From = Evidence::None;
    bool Conflict = false;      // an override disagrees with what was recorded
    QString Why;                // one line for tooltips

    bool IsCode() const { return Kind == CodeKind::Arm || Kind == CodeKind::Thumb || Kind == CodeKind::Mixed; }
    bool Confirmed() const { return From == Evidence::Recorded; }
};

QString KindName(CodeKind k);          // arm, thumb, mixed, data, unknown
QString EvidenceName(Evidence e);      // confirmed, override, defined, inferred, from analysis, none
QChar BadgeLetter(CodeKind k);         // A, T, M, D, ?

// What the knowledge store says about a range, gathered on the main thread (where the store lives)
// so that queries on the emulation thread can classify without touching it.
struct KbHints
{
    struct Range
    {
        melonDS::u32 Start, End;    // End exclusive
        CodeKind Kind;              // functions: Arm / Thumb; data objects: Data; overrides: any
        QString Name;               // function or data object name, override author
    };
    std::vector<Range> Functions, Data, Overrides;

    // overlay: the overlay loaded at start (-1: unknown)
    static KbHints Collect(const WBKB::KnowledgeStore& kb, int cpu, melonDS::u32 start, melonDS::u32 end, int overlay);
    static const Range* Find(const std::vector<Range>& v, melonDS::u32 addr);
};

// one element of a defined data object: a field (or array element) of its type, shown as
// .byte/.hword/.word/.asciz with the field's name
struct DataUnit
{
    melonDS::u32 Addr, Size;
    bool Str;
    QString Name;
};

// the units of the data objects in [start, end), from their knowledge-store types (main thread), by address
std::vector<DataUnit> DataUnits(const WBKB::KnowledgeStore& kb, int cpu, melonDS::u32 start, melonDS::u32 end, int overlay);

// Marks from Mark unreached code. Ranges are sorted by Start and do not overlap.
// Fills only bytes that the recording, an override, a data object and the knowledge-store
// functions leave unknown.
struct AnalysisHints
{
    struct Range
    {
        melonDS::u32 Start = 0, End = 0;
        CodeKind Kind = CodeKind::Unknown;
        QString Why;
    };
    std::vector<Range> Ranges;

    static const Range* Find(const std::vector<Range>& v, melonDS::u32 addr);
};

// rec: the byte record of the first byte, or nullptr if nothing was recorded there
Classification Classify(const melonDS::WB::ByteRecord* rec, const KbHints& kb, melonDS::u32 addr,
                        const AnalysisHints* analysis = nullptr);

}

#endif
