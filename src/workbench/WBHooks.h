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

// Minimal hook points used by the melonDS core. Each one costs a single flag
// test when the workbench is idle.

#ifndef WB_HOOKS_H
#define WB_HOOKS_H

#include "Workbench.h"
#include "../NDS.h"

namespace melonDS::WB
{

// Reports a CPU data read after the access completes, whichever return path the read takes.
struct ReadHook
{
    melonDS::NDS& N;
    u32* Val;
    u32 Addr;
    u8 Cpu, Size;
    ReadHook(melonDS::NDS& n, u32 cpu, u32 addr, u32 size, u32* val) : N(n), Val(val), Addr(addr), Cpu((u8)cpu), Size((u8)size) {}
    ~ReadHook()
    {
        if (N.WBMemHooks) [[unlikely]] N.WB->CpuRead(Cpu, Addr, Size, *Val);
    }
};

}

#define WB_READ_HOOK(cpu, addr, size, val) melonDS::WB::ReadHook wbReadHook_(NDS, cpu, addr, size, val)
#define WB_WRITE_HOOK(cpu, addr, size, val) do { if (NDS.WBMemHooks) [[unlikely]] NDS.WB->CpuWrite(cpu, addr, size, val); } while (0)

#endif
