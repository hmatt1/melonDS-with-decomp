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

#include <chrono>
#include <cstring>
#include "Workbench.h"
#include "../NDS.h"

namespace melonDS::WB
{

int Debugger::AddBreakpoint(Breakpoint bp, std::string& error)
{
    if (!bp.Condition.empty())
    {
        bp.Compiled = CompileExpr(bp.Condition, error);
        if (!bp.Compiled) return -1;
    }
    if (bp.Size == 0) bp.Size = 1;
    bp.Id = NextId++;
    bp.HitCount = 0;
    Breakpoints.push_back(bp);
    RebuildIndex();
    return bp.Id;
}

bool Debugger::RemoveBreakpoint(int id)
{
    bool found = false;
    for (size_t i = 0; i < Breakpoints.size(); )
    {
        if (id == -1 || Breakpoints[i].Id == id)
        {
            Breakpoints.erase(Breakpoints.begin() + i);
            found = true;
        }
        else i++;
    }
    RebuildIndex();
    return found || id == -1;
}

bool Debugger::SetBreakpointEnabled(int id, bool en)
{
    for (Breakpoint& bp : Breakpoints)
        if (bp.Id == id)
        {
            bp.Enabled = en;
            RebuildIndex();
            return true;
        }
    return false;
}

bool Debugger::SetBreakpointCondition(int id, const std::string& cond, std::string& error)
{
    for (Breakpoint& bp : Breakpoints)
        if (bp.Id == id)
        {
            std::shared_ptr<Expr> compiled;
            if (!cond.empty())
            {
                compiled = CompileExpr(cond, error);
                if (!compiled) return false;
            }
            bp.Condition = cond;
            bp.Compiled = compiled;
            return true;
        }
    error = "no breakpoint " + std::to_string(id);
    return false;
}

void Debugger::RebuildIndex()
{
    NumExec = NumWatch = 0;
    for (int c = 0; c < 2; c++)
    {
        delete[] ExecPages[c];
        ExecPages[c] = nullptr;
    }
    delete[] WatchPages;
    WatchPages = nullptr;

    for (const Breakpoint& bp : Breakpoints)
    {
        if (!bp.Enabled) continue;
        u32 first = bp.Addr >> PageShift;
        u32 last = (bp.Addr + bp.Size - 1) >> PageShift;
        if (bp.Kind == BreakKind::Exec)
        {
            NumExec++;
            for (int c = 0; c < 2; c++)
            {
                if (bp.Cpu != -1 && bp.Cpu != c) continue;
                if (!ExecPages[c]) { ExecPages[c] = new u8[1u << 20]; memset(ExecPages[c], 0, 1u << 20); }
                for (u32 p = first; p <= last; p++) ExecPages[c][p & 0xFFFFF] = 1;
            }
        }
        else
        {
            NumWatch++;
            if (!WatchPages) { WatchPages = new u8[1u << 20]; memset(WatchPages, 0, 1u << 20); }
            for (u32 p = first; p <= last; p++) WatchPages[p & 0xFFFFF] = 1;
        }
    }
    WB.UpdateHookFlags();
}

bool Debugger::EvalCondition(Breakpoint& bp, u32 cpu, u32 addr, u32 value, u32 size)
{
    if (!bp.Compiled) return true;
    EvalContext ctx {&WB, cpu & 1, addr, value, size, bp.HitCount};
    return EvalExpr(*bp.Compiled, ctx) != 0;
}

void Debugger::RecordHit(Breakpoint& bp, u32 cpu, u32 pc, u32 addr, u32 value, u32 size, bool write)
{
    bp.HitCount++;
    BreakHit h;
    h.Id = bp.Id;
    h.Cpu = cpu;
    h.PC = pc;
    h.Frame = WB.Frame();
    h.AccessAddr = addr;
    h.AccessValue = value;
    h.AccessSize = size;
    h.Write = write;
    HitList.push_back(h);
    while (HitList.size() > 1000) HitList.pop_front();
    if (WB.OnEvent)
    {
        WBEvent ev {};
        ev.Type = WBEvent::BreakHitEvent;
        ev.Hit = h;
        // Break is filled in by EnterBreak's own event; this one carries the hit details
        ev.Break.Cpu = cpu;
        ev.Break.PC = pc;
        ev.Break.Reason = bp.Kind == BreakKind::Exec ? "breakpoint" : "watchpoint";
        ev.Break.BreakpointId = bp.Id;
        ev.Break.AccessAddr = addr;
        ev.Break.AccessValue = value;
        ev.Break.AccessSize = size;
        ev.Break.Frame = h.Frame;
        LastHitEvent = ev;
        HaveHitEvent = true;
    }
}

bool Debugger::CheckExec(u32 cpu, u32 pc)
{
    if (Suppressed) return false;

    if (WatchHitPending && WatchBreak.Cpu == cpu)
    {
        WatchHitPending = false;
        NextBreak = WatchBreak;
        return true;
    }

    if (StepActive.load() && (u32)StepCpu.load() == cpu)
    {
        u32 rem = StepRemaining.load();
        if (rem == 0)
        {
            StepActive = false;
            NextBreak = BreakState();
            NextBreak.Reason = "step";
            return true;
        }
        StepRemaining = rem - 1;
    }

    if (PendingBreak.load())
    {
        int pc_cpu = PendingCpu.load();
        if (pc_cpu == -1 || (u32)pc_cpu == cpu)
        {
            PendingBreak = false;
            NextBreak = BreakState();
            {
                std::lock_guard<std::mutex> l(StateLock);
                NextBreak.Reason = PendingReason.empty() ? "pause" : PendingReason;
            }
            return true;
        }
    }

    if (ExecPages[cpu] && ExecPages[cpu][pc >> PageShift])
    {
        for (size_t i = 0; i < Breakpoints.size(); i++)
        {
            Breakpoint& bp = Breakpoints[i];
            if (!bp.Enabled || bp.Kind != BreakKind::Exec) continue;
            if (bp.Cpu != -1 && (u32)bp.Cpu != cpu) continue;
            if (pc < bp.Addr || pc - bp.Addr >= bp.Size) continue;
            if (!EvalCondition(bp, cpu, pc, 0, 0)) continue;
            RecordHit(bp, cpu, pc, 0, 0, 0, false);
            NextBreak = BreakState();
            NextBreak.Reason = "breakpoint";
            NextBreak.BreakpointId = bp.Id;
            if (bp.Temporary)
            {
                Breakpoints.erase(Breakpoints.begin() + i);
                RebuildIndex();
            }
            return true;
        }
    }
    return false;
}

void Debugger::CheckAccess(u32 cpu, u32 addr, u32 size, u32 value, bool write)
{
    if (Suppressed || WatchHitPending) return;
    for (size_t i = 0; i < Breakpoints.size(); i++)
    {
        Breakpoint& bp = Breakpoints[i];
        if (!bp.Enabled || bp.Kind == BreakKind::Exec) continue;
        if (bp.Kind == BreakKind::Read && write) continue;
        if (bp.Kind == BreakKind::Write && !write) continue;
        if (bp.Cpu != -1 && (u32)bp.Cpu != (cpu & 1)) continue;
        if (addr + size <= bp.Addr || addr >= bp.Addr + bp.Size) continue;
        if (!EvalCondition(bp, cpu, addr, value, size)) continue;
        u32 pc = cpu < 2 ? WB.Cur[cpu] : 0;
        RecordHit(bp, cpu, pc, addr, value, size, write);
        WatchHitPending = true;
        WatchBreak = BreakState();
        WatchBreak.Cpu = cpu & 1;
        WatchBreak.Reason = cpu >= 2 ? "watchpoint (dma)" : "watchpoint";
        WatchBreak.BreakpointId = bp.Id;
        WatchBreak.AccessAddr = addr;
        WatchBreak.AccessValue = value;
        WatchBreak.AccessSize = size;
        WatchBreak.HitPC = pc;
        if (bp.Temporary)
        {
            Breakpoints.erase(Breakpoints.begin() + i);
            RebuildIndex();
        }
        WB.UpdateHookFlags();
        return;
    }
}

void Debugger::RequestBreak(int cpu, const char* reason)
{
    {
        std::lock_guard<std::mutex> l(StateLock);
        PendingReason = reason ? reason : "pause";
    }
    PendingCpu = cpu;
    PendingBreak = true;
    if (melonDS::NDS* nds = WB.GetNDS())
        if (!nds->IsJITEnabled()) nds->WBExecHooks = true;
}

void Debugger::RequestStep(int cpu, u32 count)
{
    if (count == 0) count = 1;
    u32 rem = count;
    if (BreakActive.load() && CurrentBreak().Cpu == (u32)cpu) rem = count - 1;
    StepCpu = cpu;
    StepRemaining = rem;
    StepActive = true;
    if (melonDS::NDS* nds = WB.GetNDS())
        if (!nds->IsJITEnabled()) nds->WBExecHooks = true;
}

void Debugger::CancelStep()
{
    StepActive = false;
    PendingBreak = false;
}

void Debugger::Continue()
{
    ContinueReq = true;
    JobCond.notify_all();
}

void Debugger::EnterBreak(u32 cpu, u32 pc)
{
    {
        std::lock_guard<std::mutex> l(StateLock);
        Break = NextBreak;
        Break.Cpu = cpu;
        Break.PC = pc;
        Break.Frame = WB.Frame();
    }
    ContinueReq = false;
    BreakActive = true;

    if (WB.OnEvent)
    {
        WBEvent ev {};
        if (HaveHitEvent)
        {
            ev = LastHitEvent;
            HaveHitEvent = false;
        }
        else ev.Hit.Id = -1;
        ev.Type = WBEvent::BreakHitEvent;
        ev.Break = CurrentBreak();
        WB.OnEvent(ev);
    }

    while (!ContinueReq.load())
    {
        ServiceJobs();
        if (HostIdle) HostIdle();
        if (HostWantsAbort && HostWantsAbort())
        {
            // the frontend needs the emulation thread (stop, reset, load state, quit):
            // run to the end of the frame without stopping again
            Suppressed = true;
            StepActive = false;
            PendingBreak = false;
            break;
        }
        std::unique_lock<std::mutex> lk(JobLock);
        JobCond.wait_for(lk, std::chrono::milliseconds(10), [this] { return !Jobs.empty() || ContinueReq.load(); });
    }

    BreakActive = false;
    ContinueReq = false;
    if (WB.OnEvent)
    {
        WBEvent ev {};
        ev.Type = WBEvent::BreakLeftEvent;
        ev.Break = CurrentBreak();
        WB.OnEvent(ev);
    }
    WB.UpdateHookFlags();
}

void Debugger::PostJob(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> l(JobLock);
        Jobs.push_back(std::move(job));
    }
    JobCond.notify_all();
}

bool Debugger::ServiceJobs()
{
    std::deque<std::function<void()>> run;
    {
        std::lock_guard<std::mutex> l(JobLock);
        if (Jobs.empty()) return false;
        run.swap(Jobs);
    }
    for (auto& j : run) j();
    return true;
}

bool Debugger::WaitJobs(int timeoutms)
{
    {
        std::unique_lock<std::mutex> lk(JobLock);
        JobCond.wait_for(lk, std::chrono::milliseconds(timeoutms), [this] { return !Jobs.empty(); });
    }
    return ServiceJobs();
}

}
