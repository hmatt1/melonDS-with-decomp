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

#ifndef TRACE_H
#define TRACE_H

// Tracy profiling, by category, chosen at compile time with the CMake list MELONDS_TRACE
// (all, qt, emu, render). A category that is off compiles to nothing; with none on, Tracy
// isn't built in at all. The client is vendored in vendor/tracy (see VENDORED.md).
//
//   TRACE_QT("name")            a zone: this scope, timed, shown as one bar in the viewer
//   TRACE_QT_NAMED(text)        the same with a name made at run time (const char*)
//   TRACE_QT_STEP("name")       a step of straight-line code: ends the previous step, starts this one
//   TRACE_QT_STEP_END()         ends the last step
//   TRACE_QT_MESSAGE("text")    a marker on the timeline
// and the same for EMU and RENDER.

#if defined(MELONDS_TRACE_QT) || defined(MELONDS_TRACE_EMU) || defined(MELONDS_TRACE_RENDER)
#include "tracy/Tracy.hpp"
#include "tracy/TracyC.h"

namespace Trace
{
// one open step per thread (straight-line startup code, main() in practice)
inline thread_local TracyCZoneCtx StepCtx{};
inline thread_local bool StepOpen = false;
inline void StepEnd() { if (StepOpen) { TracyCZoneEnd(StepCtx); StepOpen = false; } }
inline void Step(const ___tracy_source_location_data* loc)
{
    StepEnd();
    StepCtx = ___tracy_emit_zone_begin(loc, 1);
    StepOpen = true;
}
}

#define TRACE_ZONE_(name) ZoneScopedN(name)
#define TRACE_NAMED_(text) ZoneTransientN(TracyConcat(traceZone, TracyLine), text, true)
#define TRACE_STEP_(name) do { static const ___tracy_source_location_data traceLoc{name, __func__, TracyFile, (uint32_t)TracyLine, 0}; Trace::Step(&traceLoc); } while (0)
#define TRACE_STEP_END_() Trace::StepEnd()
#define TRACE_MESSAGE_(text) TracyMessageL(text)
#endif

#define TRACE_OFF_(...) do {} while (0)

#ifdef MELONDS_TRACE_QT
#define TRACE_QT(name) TRACE_ZONE_(name)
#define TRACE_QT_NAMED(text) TRACE_NAMED_(text)
#define TRACE_QT_STEP(name) TRACE_STEP_(name)
#define TRACE_QT_STEP_END() TRACE_STEP_END_()
#define TRACE_QT_MESSAGE(text) TRACE_MESSAGE_(text)
#else
#define TRACE_QT(name) TRACE_OFF_()
#define TRACE_QT_NAMED(text) TRACE_OFF_()
#define TRACE_QT_STEP(name) TRACE_OFF_()
#define TRACE_QT_STEP_END() TRACE_OFF_()
#define TRACE_QT_MESSAGE(text) TRACE_OFF_()
#endif

#ifdef MELONDS_TRACE_EMU
#define TRACE_EMU(name) TRACE_ZONE_(name)
#define TRACE_EMU_NAMED(text) TRACE_NAMED_(text)
#define TRACE_EMU_STEP(name) TRACE_STEP_(name)
#define TRACE_EMU_STEP_END() TRACE_STEP_END_()
#define TRACE_EMU_MESSAGE(text) TRACE_MESSAGE_(text)
#else
#define TRACE_EMU(name) TRACE_OFF_()
#define TRACE_EMU_NAMED(text) TRACE_OFF_()
#define TRACE_EMU_STEP(name) TRACE_OFF_()
#define TRACE_EMU_STEP_END() TRACE_OFF_()
#define TRACE_EMU_MESSAGE(text) TRACE_OFF_()
#endif

#ifdef MELONDS_TRACE_RENDER
#define TRACE_RENDER(name) TRACE_ZONE_(name)
#define TRACE_RENDER_NAMED(text) TRACE_NAMED_(text)
#define TRACE_RENDER_STEP(name) TRACE_STEP_(name)
#define TRACE_RENDER_STEP_END() TRACE_STEP_END_()
#define TRACE_RENDER_MESSAGE(text) TRACE_MESSAGE_(text)
#else
#define TRACE_RENDER(name) TRACE_OFF_()
#define TRACE_RENDER_NAMED(text) TRACE_OFF_()
#define TRACE_RENDER_STEP(name) TRACE_OFF_()
#define TRACE_RENDER_STEP_END() TRACE_OFF_()
#define TRACE_RENDER_MESSAGE(text) TRACE_OFF_()
#endif

#endif // TRACE_H
