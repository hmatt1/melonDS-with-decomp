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

#ifndef WB_TEXT_H
#define WB_TEXT_H

// Words shown in more than one place, kept here so every place says the same thing.
namespace WBText
{
// the write history file's size, wherever it is set (the Recording panel, Reset game with tracking)
inline constexpr const char* HistoryFileSuffix = " MB file";
inline constexpr const char* HistoryInMemory = "In memory";
inline constexpr const char* HistoryFileTip =
    "Where write history is kept. A file holds about 32,000 writes per MB; in memory, it keeps up to the "
    "write history cap. Changing it starts history afresh.";
}

#endif
