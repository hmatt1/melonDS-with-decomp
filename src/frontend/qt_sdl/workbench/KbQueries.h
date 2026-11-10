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

// Knowledge-store reads for the API: the lists the Functions, Annotations, Bookmarks and
// Types panels show, and one function as the Source and Decompiler panels show it. These
// read the knowledge store's caches, so they run on the main thread; what comes from the
// recording (how often an entry ran, decode conflicts) is filled in by the caller.

#ifndef WB_KBQUERIES_H
#define WB_KBQUERIES_H

#include <functional>
#include <map>
#include "WBApi.h"
#include "KnowledgeStore.h"

namespace WBQ
{

// conflicts: decode override id -> what the recording disagrees with (DecodeConflict), for
// the decode_overrides kind; without it no override has a conflict
bool KbList(const WBKB::KnowledgeStore& kb, const WBApi::KbListParams& p, WBApi::KbListResult& out, QString& err,
            const std::map<qint64, QString>* conflicts = nullptr);

WBApi::KbFunctionInfo KbFunction(const WBKB::KnowledgeStore& kb, const WBKB::Function& f);
WBApi::KbAnnotationInfo KbAnnotation(const WBKB::KnowledgeStore& kb, const WBKB::Annotation& a);

// annotations on a function, on its registers, and on addresses and ranges inside it
std::vector<const WBKB::Annotation*> FunctionAnnotations(const WBKB::KnowledgeStore& kb, const WBKB::Function& f);

// sourcePath: the function's source file resolved against sourceRoot; its text is read only
// when it is inside sourceRoot. overlayAt names the overlay at an address the decompiler
// output refers to
WBApi::KbFunctionDetail KbFunctionDetail(WBKB::KnowledgeStore& kb, const WBKB::Function& f, const QString& sourcePath,
                                         const QString& sourceRoot, bool readSource, int attempts,
                                         const std::function<int(melonDS::u32)>& overlayAt);

}

#endif
