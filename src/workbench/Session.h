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

#ifndef WB_SESSION_H
#define WB_SESSION_H

#include "Workbench.h"

namespace melonDS::WB
{

struct SessionSectionView
{
    SessionSectionHeader Header;
    const u8* Data;
};

// Coverage merged from saved sessions ("all time" scope)
struct MergedSessions
{
    RecordStore Records;
    std::unordered_map<CallEdgeKey, CallEdge, CallEdgeKeyHash> Edges[2];
    std::vector<ProvenanceEvent> Provenance;
    std::unordered_map<u64, u32> FirstFunc;     // (cpu << 32 | pc) -> function entry, see Recorder::FirstRunIn
    u32 SessionCount = 0;
    u64 FramesRecorded = 0;
    void Clear()
    {
        Records.Clear();
        Edges[0].clear();
        Edges[1].clear();
        Provenance.clear();
        FirstFunc.clear();
        SessionCount = 0;
        FramesRecorded = 0;
    }
};

bool ReadSession(const u8* data, size_t len, SessionHeader& hdr, std::vector<SessionSectionView>& sections);
bool MergeSession(const u8* data, size_t len, MergedSessions& into);
// the newest write history a session file keeps (Recorder::SessionHistoryMax entries at most)
bool SessionHistory(const u8* data, size_t len, HistoryView& out);

// <session>.dswbh, next to <session>.dswbs: all of a session's write history when the session file
// can't hold it (UI_REQUIREMENTS HC-3). The history file's slots, cut to the used ones when it never
// wrapped, then this trailer
struct HistoryFileTrailer
{
    char Magic[8];              // "DSWBH1"
    u64 Pos, Used, Slots;       // as in HistoryView
    u32 FrameStart, FrameEnd;   // the session's
    u32 EntrySize, Reserved;
};
static_assert(sizeof(HistoryFileTrailer) == 48, "history file trailer layout");
HistoryFileTrailer MakeHistoryTrailer(const HistoryView& v, u32 frameStart, u32 frameEnd);
// a mapped or loaded .dswbh; false when it isn't one
bool ReadHistoryFile(const u8* data, size_t len, HistoryView& out, HistoryFileTrailer* trailer = nullptr);

}

#endif
