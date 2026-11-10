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

// Recursive disassembly of the bytes the recording has not already classified.
// A straight-line block is committed with the rest of its batch. Calls wait until
// that batch is in the program, so a callee is decoded only after the code that
// reached it has been kept. ARMv5TE has no delay slots; the successor of an
// instruction is the address after its encoding.

#ifndef WB_FLOWDISASM_H
#define WB_FLOWDISASM_H

#include <atomic>
#include <string>
#include <vector>

#include "types.h"
#include "Disasm.h"

namespace melonDS::WB
{

enum class FlowKind : u8
{
    Priority = 0,         // taken before anything discovered in the current batch
    Branch = 1,
    CallFallthrough = 2,  // the instruction after a call; after ordinary branches
    Call = 3              // starts a later batch
};

// How the first instruction of a block was reached. Later instructions in the
// block fall through from the one before them.
enum class FlowReach : u8
{
    Entry,
    Branch,
    Call,
    AfterCall,
    Continue
};

struct FlowInsn
{
    u32 Addr = 0;
    u32 Size = 0;
    bool Thumb = false;
    u32 Raw = 0;
    u32 From = 0;
    FlowReach Reach = FlowReach::Entry;
    bool Literal = false;
    u32 LiteralAddr = 0;
};

struct FlowNote
{
    enum Kind { Data, Offcut, Prototype, Repeat, Memory, Parse, Alignment, Context, Limit } Type = Data;
    u32 Addr = 0;
    std::string Text;
};

struct FlowSeed
{
    u32 Addr = 0;
    bool Thumb = false;
    u32 From = 0;
    FlowReach Reach = FlowReach::Entry;
};

struct FlowOptions
{
    u32 MaxPerSet = 2048;
    u32 RepeatLimit = 16;
    u32 MaxInstructions = 500000;
    bool FollowFlow = true;
};

// What the walk is allowed to read, and the instructions and data already known.
// Commit runs once per batch, before the next batch, so a call target sees the
// instructions that reached it.
class FlowImage
{
public:
    virtual ~FlowImage() = default;
    virtual bool Read(u32 addr, u32 size, u32& out) const = 0;
    virtual bool Restricted(u32 addr) const = 0;
    virtual bool Initialized(u32 addr) const = 0;
    virtual bool NoReturn(u32 addr) const = 0;
    virtual bool InstructionAt(u32 addr, FlowInsn& out) const = 0;
    virtual bool Offcut(u32 addr) const = 0;          // strictly inside an instruction
    virtual bool DefinedData(u32 addr) const = 0;
    virtual void Commit(const std::vector<FlowInsn>& added) = 0;
};

struct FlowDisasmResult
{
    std::vector<FlowInsn> Added;
    std::vector<FlowNote> Notes;
    u32 NoteCount = 0;
    bool Incomplete = false;
};

// Direct targets of one decoded instruction, and whether execution falls out of it.
// A call to a no-return function contributes neither a target nor a fall-through.
struct InsnEdges
{
    struct Target
    {
        u32 Addr;
        bool Thumb;
        bool Call;
    };
    std::vector<Target> Targets;
    bool Falls = false;
    u32 Next = 0;
};

InsnEdges EdgesOf(u32 addr, const DisasmInsn& in, bool noreturn);

// seed_queue starts as one priority flow. One call target is taken per batch,
// and only once priority flows from earlier batches are exhausted.
FlowDisasmResult Disassemble(FlowImage& image, FlowSeed start, const FlowOptions& opt = {}, std::atomic<bool>* cancel = nullptr);

// The same walk once per seed, skipping an address an earlier seed already covered.
FlowDisasmResult DisassembleSet(FlowImage& image, const std::vector<FlowSeed>& starts, const FlowOptions& opt = {}, std::atomic<bool>* cancel = nullptr);

}

#endif
