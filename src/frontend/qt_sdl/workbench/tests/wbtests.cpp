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

// Unit tests for the workbench: condition expressions, disassembler, data-flow
// decoding, ROM parsing, C type layouts, the knowledge store and the game archive resolver.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_set>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <sqlite3.h>

#include "Workbench.h"
#include "Disasm.h"
#include "InstrOps.h"
#include "NitroROM.h"
#include "KnowledgeStore.h"
#include "Session.h"
#include "GameArchive.h"
#include "Containers.h"
#include "WBClassify.h"
#include "FlowDisasm.h"
#include "WBSpans.h"
#include "KbQueries.h"
#include "ApiConnections.h"
#include "WBCompare.h"
#include "WBGfx.h"
#include <zlib.h>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int mode);
#endif

using namespace melonDS;
using namespace melonDS::WB;

static int Fails = 0, Passes = 0;

#define CHECK(cond) do { if (cond) Passes++; else { Fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fflush(stdout); } } while (0)
#define CHECK_EQ_STR(a, b) do { std::string _a = (a), _b = (b); if (_a == _b) Passes++; else { Fails++; printf("FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); fflush(stdout); } } while (0)

static void TestExpr()
{
    Workbench wb;
    EvalContext ctx {&wb, 0, 0x02001000, 0x1234, 4, 3};
    auto ev = [&](const char* text, s64 want)
    {
        std::string err;
        auto e = CompileExpr(text, err);
        if (!e) { Fails++; printf("FAIL expr '%s' did not compile: %s\n", text, err.c_str()); fflush(stdout); return; }
        s64 v = EvalExpr(*e, ctx);
        if (v == want) Passes++;
        else { Fails++; printf("FAIL expr '%s' = %lld, want %lld\n", text, (long long)v, (long long)want); fflush(stdout); }
    };
    ev("1 + 2 * 3", 7);
    ev("(1 + 2) * 3", 9);
    ev("0x10 << 2", 0x40);
    ev("$FF & 0x0F", 0x0F);
    ev("value == 0x1234", 1);
    ev("value != 0x1234 || addr == 0x02001000", 1);
    ev("size >= 4 && hits < 4", 1);
    ev("!0", 1);
    ev("~0 & 0xFF", 0xFF);
    ev("-5 + 10", 5);
    ev("10 / 3", 3);
    ev("10 % 3", 1);
    ev("1 <= 1 && 2 > 1 && 1 < 2 && 2 >= 2", 1);
    ev("r0 == 0", 1);              // no NDS attached: registers read as 0
    std::string err;
    CHECK(!CompileExpr("1 +", err) && !err.empty());
    CHECK(!CompileExpr("r16", err));
    CHECK(!CompileExpr("foo == 1", err));
    CHECK(!CompileExpr("[1", err));
    CHECK(CompileExpr("u16[r4 + 0x20] != 0", err) != nullptr);
    CHECK(CompileExpr("[sp+4].w > 3", err) != nullptr);
    // a register name is r and digits only; a hex prefix needs digits after it
    CHECK(!CompileExpr("r1x == 0", err));
    CHECK(!CompileExpr("r2d2", err));
    CHECK(!CompileExpr("$ == 0", err));
    CHECK(!CompileExpr("0x == 0", err));
    CHECK(!CompileExpr("$-1 == 0", err));
    // the one division that overflows wraps instead of faulting
    ev("0x8000000000000000 / -1 == 0x8000000000000000", 1);
    ev("0x8000000000000000 % -1", 0);
    ev("-0x8000000000000000 == 0x8000000000000000", 1);
}

static std::string Dis(u32 addr, u32 word, bool thumb)
{
    DisasmInsn in;
    Disassemble(addr, word, thumb, in);
    return in.Text();
}

static void TestDisasm()
{
    CHECK_EQ_STR(Dis(0x02000000, 0xE3A00000, false), "mov r0, #0");
    CHECK_EQ_STR(Dis(0x02000000, 0xE12FFF1E, false), "bx lr");
    CHECK_EQ_STR(Dis(0x02000000, 0xE92D41F0, false), "push {r4-r8, lr}");
    CHECK_EQ_STR(Dis(0x02000000, 0xE8BD81F0, false), "pop {r4-r8, pc}");
    CHECK_EQ_STR(Dis(0x02000000, 0xE59F1004, false), "ldr r1, [pc, #4]");
    CHECK_EQ_STR(Dis(0x02000000, 0xE5C10000, false), "strb r0, [r1]");
    CHECK_EQ_STR(Dis(0x02000000, 0xEE070F90, false), "mcr p15, 0, r0, c7, c0, 4");
    CHECK_EQ_STR(Dis(0x02000000, 0xEB000000, false), "bl 0x2000008");
    CHECK_EQ_STR(Dis(0x02000000, 0x1AFFFFFB, false), "bne 0x1FFFFF4");
    CHECK_EQ_STR(Dis(0x02000000, 0xE0810002, false), "add r0, r1, r2");
    CHECK_EQ_STR(Dis(0x02000000, 0xE1A00080, false), "mov r0, r0, lsl #1");
    CHECK_EQ_STR(Dis(0x02000000, 0xE0000291, false), "mul r0, r1, r2");
    CHECK_EQ_STR(Dis(0x02000000, 0xE1D100B2, false), "ldrh r0, [r1, #2]");
    CHECK_EQ_STR(Dis(0x02000000, 0xEF110000, false), "swi 0x11");
    CHECK_EQ_STR(Dis(0x02000000, 0xE16F0F11, false), "clz r0, r1");
    CHECK_EQ_STR(Dis(0x02000000, 0xE10F0000, false), "mrs r0, cpsr");
    CHECK_EQ_STR(Dis(0x02000000, 0xFA000000, false), "blx 0x2000008");
    // Thumb
    CHECK_EQ_STR(Dis(0x02000000, 0xB510, true), "push {r4, lr}");
    CHECK_EQ_STR(Dis(0x02000000, 0xBD10, true), "pop {r4, pc}");
    CHECK_EQ_STR(Dis(0x02000000, 0x4C02, true), "ldr r4, [pc, #8]");
    CHECK_EQ_STR(Dis(0x02000000, 0x3001, true), "adds r0, #1");
    CHECK_EQ_STR(Dis(0x02000000, 0x0040, true), "lsls r0, r0, #1");
    CHECK_EQ_STR(Dis(0x02000000, 0x4770, true), "bx lr");
    CHECK_EQ_STR(Dis(0x02000000, 0xDF0B, true), "swi 0xB");
    CHECK_EQ_STR(Dis(0x02000000, 0xD0FE, true), "beq 0x2000000");
    // BL pair: prefix + suffix in one word
    {
        DisasmInsn in;
        Disassemble(0x02000000, 0xF802F000, true, in);
        CHECK(in.Size == 4 && in.IsCall && in.Target == 0x02000008);
    }
    {
        DisasmInsn in;
        Disassemble(0x02000004, 0xE59F1004, false, in);
        CHECK(in.LiteralLoad && in.Ref == 0x02000010);
        Disassemble(0x02000002, 0x4C02, true, in);
        CHECK(in.LiteralLoad && in.Ref == 0x0200000C);
    }
    CHECK(IsFunctionPrologue(0xE92D41F0, false));
    CHECK(IsFunctionPrologue(0xE92D4000, false));
    CHECK(IsFunctionPrologue(0xB500, true));
    CHECK(IsFunctionPrologue(0xB510, true));
    CHECK(!IsFunctionPrologue(0xE3A00000, false));
    CHECK(!IsFunctionPrologue(0xB400, true));
    {
        DisasmInsn in;
        Disassemble(0x02000000, 0xE0810002, false, in);
        std::string e = ExplainInsn(in);
        CHECK(e.find("r0") != std::string::npos && e.find("r1") != std::string::npos && e.find("r2") != std::string::npos);
        CHECK(e.find("adds") != std::string::npos || e.find("Adds") != std::string::npos);
        Disassemble(0x02000000, 0xE59F1004, false, in);
        e = ExplainInsn(in);
        CHECK(e.find("r1") != std::string::npos && e.find("pc") != std::string::npos);
        Disassemble(0x02000000, 0x1AFFFFFB, false, in);
        e = ExplainInsn(in);
        CHECK(e.find("not equal") != std::string::npos || e.find("ne") != std::string::npos);
        Disassemble(0x02000000, 0xFFFFFFFF, false, in);
        CHECK(ExplainInsn(in).find("not a recognised") != std::string::npos);
        Disassemble(0x02000000, 0xE0810002, false, in);
        CHECK(InsnAsC(in).find("r0") != std::string::npos);
    }
}

static void TestInstrOps()
{
    InstrOps o;
    DecodeOps(0xE0810002, false, o);   // add r0, r1, r2
    CHECK(o.kind == InstrOps::Alu && o.dstMask == 1 && o.srcMask == 6);
    DecodeOps(0xE3A00005, false, o);   // mov r0, #5
    CHECK(o.kind == InstrOps::Alu && o.constResult);
    DecodeOps(0xE5912004, false, o);   // ldr r2, [r1, #4]
    CHECK(o.kind == InstrOps::Load && o.nLoad == 1 && o.loadRegs[0] == 2);
    DecodeOps(0xE59F1004, false, o);   // ldr r1, [pc, #4]
    CHECK(o.pcRelLoad);
    DecodeOps(0xE92D41F0, false, o);   // push {r4-r8, lr}
    CHECK(o.kind == InstrOps::Store && o.nStore == 6 && o.storeRegs[0] == 4 && o.storeRegs[5] == 14);
    DecodeOps(0xE1010092, false, o);   // swp r0, r2, [r1]
    CHECK(o.kind == InstrOps::Swap && o.loadRegs[0] == 0 && o.storeRegs[0] == 2);
    DecodeOps(0xB510, true, o);        // push {r4, lr}
    CHECK(o.nStore == 2 && o.storeRegs[1] == 14);
    DecodeOps(0xBD10, true, o);        // pop {r4, pc}
    CHECK(o.nLoad == 2 && o.loadRegs[1] == 15);
    DecodeOps(0x4C02, true, o);        // ldr r4, [pc, #8]
    CHECK(o.pcRelLoad && o.loadRegs[0] == 4);
    DecodeOps(0x1888, true, o);        // adds r0, r1, r2
    CHECK(o.kind == InstrOps::Alu && o.dstMask == 1 && o.srcMask == 6);
    CHECK(SwiNumber(0xEF110000, false) == 0x11 && SwiNumber(0xDF0B, true) == 0x0B && SwiNumber(0xE3A00000, false) == -1);

    DecodeOps(0xE0000291, false, o);   // mul r0, r1, r2
    CHECK(o.kind == InstrOps::Alu && o.dstMask == 1 && o.srcMask == 6 && !o.constResult);
    DecodeOps(0xE0213291, false, o);   // mla r1, r1, r2, r3
    CHECK(o.dstMask == (1 << 1) && o.srcMask == ((1 << 1) | (1 << 2) | (1 << 3)));
    DecodeOps(0xE0832594, false, o);   // umull r2, r3, r4, r5
    CHECK(o.dstMask == ((1 << 2) | (1 << 3)) && o.srcMask == ((1 << 4) | (1 << 5)));
    DecodeOps(0xE0A32594, false, o);   // umlal r2, r3, r4, r5: the previous 64-bit value is a source
    CHECK(o.dstMask == ((1 << 2) | (1 << 3)) && o.srcMask == ((1 << 2) | (1 << 3) | (1 << 4) | (1 << 5)));
    DecodeOps(0xE1500001, false, o);   // cmp r0, r1
    CHECK(o.kind == InstrOps::None && o.dstMask == 0);
    DecodeOps(0xE1C020D8, false, o);   // ldrd r2, r3, [r0, #8]
    CHECK(o.kind == InstrOps::Load && o.nLoad == 2 && o.loadRegs[0] == 2 && o.loadRegs[1] == 3 && !o.pcRelLoad && o.addrMask == 1);
    DecodeOps(0xE1CF20D8, false, o);   // ldrd r2, r3, [pc, #8]
    CHECK(o.pcRelLoad && o.nLoad == 2);
    DecodeOps(0xE1C020F8, false, o);   // strd r2, r3, [r0, #8]
    CHECK(o.kind == InstrOps::Store && o.nStore == 2 && o.storeRegs[0] == 2 && o.storeRegs[1] == 3);
    DecodeOps(0xE19100B2, false, o);   // ldrh r0, [r1, r2]
    CHECK(o.kind == InstrOps::Load && o.nLoad == 1 && o.addrMask == ((1 << 1) | (1 << 2)));
    DecodeOps(0xE7910002, false, o);   // ldr r0, [r1, r2]
    CHECK(o.addrMask == ((1 << 1) | (1 << 2)));
    DecodeOps(0xE8900006, false, o);   // ldmia r0, {r1, r2}
    CHECK(o.kind == InstrOps::Load && o.nLoad == 2 && o.loadRegs[0] == 1 && o.loadRegs[1] == 2 && o.addrMask == 1);
    DecodeOps(0xE92D41F0, false, o);   // push {r4-r8, lr}
    CHECK(o.addrMask == (1 << 13));
    DecodeOps(0xEB000000, false, o);   // bl
    CHECK(o.kind == InstrOps::Branch && o.writesLR && o.dstMask == (1 << 14) && o.constResult);
    DecodeOps(0xFA000000, false, o);   // blx imm
    CHECK(o.kind == InstrOps::Branch && o.writesLR);
    DecodeOps(0xE12FFF10, false, o);   // bx r0
    CHECK(o.kind == InstrOps::Branch && !o.writesLR && o.dstMask == 0);
    DecodeOps(0xE12FFF30, false, o);   // blx r0
    CHECK(o.kind == InstrOps::Branch && o.writesLR && o.dstMask == (1 << 14));
    DecodeOps(0xEF110000, false, o);   // swi 0x11
    CHECK(o.kind == InstrOps::Swi && o.swiNum == 0x11);
    DecodeOps(0xDF0B, true, o);
    CHECK(o.kind == InstrOps::Swi && o.swiNum == 0x0B);
    DecodeOps(0x5088, true, o);        // str r0, [r1, r2]
    CHECK(o.kind == InstrOps::Store && o.addrMask == ((1 << 1) | (1 << 2)));
    DecodeOps(0x9800, true, o);        // ldr r0, [sp]
    CHECK(o.kind == InstrOps::Load && o.addrMask == (1 << 13) && o.loadRegs[0] == 0);
    DecodeOps(0xC802, true, o);        // ldmia r0!, {r1}
    CHECK(o.kind == InstrOps::Load && o.nLoad == 1 && o.loadRegs[0] == 1 && o.addrMask == 1);
    DecodeOps(0xF800, true, o);        // bl suffix
    CHECK(o.kind == InstrOps::Branch && o.writesLR);
    DecodeOps(0xB510, true, o);        // push {r4, lr}
    CHECK(o.addrMask == (1 << 13));
}

// A minimal ROM: header, arm9, FNT with one directory and two files, FAT, overlay table.
static std::vector<u8> MakeRom()
{
    std::vector<u8> rom(0x8000, 0);
    auto w32 = [&](u32 off, u32 v) { memcpy(&rom[off], &v, 4); };
    auto w16 = [&](u32 off, u16 v) { memcpy(&rom[off], &v, 2); };
    memcpy(&rom[0], "TESTROM     ", 12);
    memcpy(&rom[0xC], "WBTS", 4);
    w32(0x20, 0x1000); w32(0x24, 0x02000800); w32(0x28, 0x02000000); w32(0x2C, 0x800);   // arm9
    w32(0x30, 0x1800); w32(0x34, 0x02380000); w32(0x38, 0x02380000); w32(0x3C, 0x100);   // arm7
    // FNT at 0x2000: root dir entry, subtable at 8: file "a.bin", dir "sub" (0xF001); sub dir: file "b.bin"
    u32 fnt = 0x2000;
    w32(fnt + 0, 16); w16(fnt + 4, 0); w16(fnt + 6, 2);      // root: subtable at +16, first file 0, 2 dirs
    w32(fnt + 8, 32); w16(fnt + 12, 2); w16(fnt + 14, 0xF000); // sub: subtable at +32, first file 2
    u8 root[] = {5, 'a', '.', 'b', 'i', 'n', 0x83, 's', 'u', 'b', 0x01, 0xF0, 0};
    memcpy(&rom[fnt + 16], root, sizeof(root));
    u8 sub[] = {5, 'b', '.', 'b', 'i', 'n', 0};
    memcpy(&rom[fnt + 32], sub, sizeof(sub));
    w32(0x40, fnt); w32(0x44, 0x40);
    // FAT at 0x2100: file 0 = a.bin, file 1 = overlay 0, file 2 = sub/b.bin
    u32 fat = 0x2100;
    w32(fat + 0, 0x3000); w32(fat + 4, 0x3010);
    w32(fat + 8, 0x4000); w32(fat + 12, 0x4100);
    w32(fat + 16, 0x5000); w32(fat + 20, 0x5020);
    w32(0x48, fat); w32(0x4C, 24);
    // overlay table at 0x2200: overlay 0 at 0x02100000, file 1
    u32 ovt = 0x2200;
    w32(ovt + 0, 0); w32(ovt + 4, 0x02100000); w32(ovt + 8, 0x100); w32(ovt + 12, 0x20); w32(ovt + 24, 1);
    w32(0x50, ovt); w32(0x54, 32);
    // footer and module params inside arm9 (params at arm9 offset 0x100)
    u32 a9 = 0x1000;
    w32(a9 + 0x800, 0xDEC00621); w32(a9 + 0x804, 0x100);
    w32(a9 + 0x100, 0x020007E8); w32(a9 + 0x104, 0x02000800); w32(a9 + 0x108, 0x02000700);
    w32(a9 + 0x10C, 0x02000700); w32(a9 + 0x110, 0x02000900); w32(a9 + 0x114, 0); w32(a9 + 0x118, 0x04017533);
    w32(a9 + 0x11C, 0xDEC00621); w32(a9 + 0x120, 0x2106C0DE);
    // autoload list (2 entries): ITCM 0x01FF8000 size 0x80, DTCM 0x027E0000 size 0x20
    w32(a9 + 0x7E8, 0x01FF8000); w32(a9 + 0x7EC, 0x80); w32(a9 + 0x7F0, 0x10);
    w32(a9 + 0x7F4, 0x027E0000); w32(a9 + 0x7F8, 0x20); w32(a9 + 0x7FC, 0x40);
    return rom;
}

static void TestNitroROM()
{
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    CHECK(r.Load(rom.data(), (u32)rom.size(), (u32)rom.size()));
    CHECK_EQ_STR(r.GameCode, "WBTS");
    const RomFile* a = r.FileByPath("/a.bin");
    const RomFile* b = r.FileByPath("/sub/b.bin");
    CHECK(a && a->Id == 0 && a->Start == 0x3000);
    CHECK(b && b->Id == 2 && b->Start == 0x5000);
    const RomFile* ov = r.FileById(1);
    CHECK(ov && ov->OverlayId == 0 && ov->Path == "overlay9_0000.bin");
    CHECK(r.FileForOffset(0x5010) == b);
    CHECK(r.FileForOffset(0x1100) && r.FileForOffset(0x1100)->Path == "arm9.bin");
    CHECK(r.Overlays9.size() == 1 && r.Overlays9[0].RamStart == 0x02100000 && r.Overlays9[0].RamEnd() == 0x02100120);
    CHECK(r.ModuleParamsAddr == 0x02000100 && r.BuildVersion == 0x04017533);
    bool itcm = false, dtcm = false, arm9 = false;
    for (const ModuleInfo& m : r.Modules)
    {
        if (m.Name == "itcm" && m.LoadAddress == 0x01FF8000 && m.Size == 0x80) itcm = true;
        if (m.Name == "dtcm" && m.LoadAddress == 0x027E0000) dtcm = true;
        if (m.Name == "arm9" && m.Size == 0x700 && m.BssSize == 0x200) arm9 = true;
    }
    CHECK(itcm && dtcm && arm9);
    CHECK(r.Sha1Hex().size() == 40);
}

// header fields whose offset + size wraps past 4 GB: each part is skipped, the rest still loads
static void TestNitroROMMalformed()
{
    auto load = [](const char* what, void (*patch)(std::vector<u8>&), auto&& check)
    {
        printf("  %s\n", what);
        fflush(stdout);
        std::vector<u8> rom = MakeRom();
        patch(rom);
        NitroROM r;
        CHECK(r.Load(rom.data(), (u32)rom.size(), (u32)rom.size()));
        check(r);
    };
    load("FAT", [](std::vector<u8>& rom) { u32 o = 0xFFFFFFF8, n = 0x10; memcpy(&rom[0x48], &o, 4); memcpy(&rom[0x4C], &n, 4); },
         [](NitroROM& r) { CHECK(!r.FileById(0) && !r.FileById(2)); });
    load("FNT", [](std::vector<u8>& rom) { u32 o = 0xFFFFFFF8, n = 0x40; memcpy(&rom[0x40], &o, 4); memcpy(&rom[0x44], &n, 4); },
         [](NitroROM& r) { CHECK(r.FileById(0) && r.FileById(0)->Path == "file_0000.bin"); });
    load("ARM9 overlay table", [](std::vector<u8>& rom) { u32 o = 0xFFFFFFF0, n = 0x20; memcpy(&rom[0x50], &o, 4); memcpy(&rom[0x54], &n, 4); },
         [](NitroROM& r) { CHECK(r.Overlays9.empty()); });
    load("ARM9 binary", [](std::vector<u8>& rom) { u32 o = 0xFFFFF000, n = 0x1800; memcpy(&rom[0x20], &o, 4); memcpy(&rom[0x2C], &n, 4); },
         [](NitroROM& r) { CHECK(r.ModuleParamsAddr == 0); });
    load("module params offset", [](std::vector<u8>& rom) { u32 o = 0xFFFFFFF0; memcpy(&rom[0x1000 + 0x804], &o, 4); },
         [](NitroROM& r) { CHECK(r.ModuleParamsAddr == 0x02000100); });     // found by searching instead
    load("autoload list before arm9", [](std::vector<u8>& rom) { u32 s = 0x02000000 - 12; memcpy(&rom[0x1000 + 0x100], &s, 4); },
         [](NitroROM& r)
         {
             int autoloads = 0;
             for (const ModuleInfo& m : r.Modules) if (m.Kind == "autoload") autoloads++;
             CHECK(autoloads == 0);
         });
}

// an IRQ interrupts code that holds data in lr (ldmia r0!, {r1-r3, lr}): that lr keeps its data origin
// label, and the handler's own lr starts as a return address, with none
static void TestOriginIrqEntry()
{
    Workbench wb;
    wb.Taint.Enable(true);
    DataSource src;
    src.Kind = SourceKind::Keypad;
    src.Name = "keypad";
    u32 label = wb.Taint.LabelForSource(wb.Taint.AddSource(src));
    CHECK(label != 0);
    wb.TrackMode(0, 0x1F, false);                       // system mode
    wb.Taint.SetRegLabel(0, 14, label);
    wb.TrackMode(0, 0x12, true);                        // IRQ entry
    CHECK(wb.Taint.RegLabel(0, 14) == 0);
    wb.Taint.SetRegLabel(0, 14, label);                 // the handler loads data into its lr too
    wb.TrackMode(0, 0x1F, false);                       // back in the interrupted code
    CHECK(wb.Taint.RegLabel(0, 14) == label);
    wb.TrackMode(0, 0x12, true);                        // the next IRQ
    CHECK(wb.Taint.RegLabel(0, 14) == 0);
}

// following addresses, a register's pointer flag and address label are banked with it: the IRQ handler's
// lr being a pointer doesn't make the interrupted code's lr one
static void TestOriginBankedPointers()
{
    Workbench wb;
    wb.Taint.Enable(true);
    wb.Taint.SetFollowAddresses(true);
    DataSource src;
    src.Kind = SourceKind::Keypad;
    src.Name = "keypad";
    u32 label = wb.Taint.LabelForSource(wb.Taint.AddSource(src));
    u32 regs[16] = {};
    regs[14] = 0x1234;                                  // data, not an address
    auto exec = [&](u32 instr) { wb.Taint.PreExec(0, instr, false, true, regs); wb.Taint.PostExec(0); };
    wb.TrackMode(0, 0x1F, false);                       // system mode
    wb.Taint.SetRegLabel(0, 14, label);                 // lr holds keypad data
    wb.TrackMode(0, 0x12, true);                        // IRQ entry
    exec(0xE59E0000);                                   // ldr r0, [lr]: the handler's lr is a pointer
    wb.TrackMode(0, 0x1F, false);                       // back in the interrupted code
    exec(0xE1A0200E);                                   // mov r2, lr
    CHECK(wb.Taint.RegLabel(0, 2) == label);
}

// write history's include and writer-exclude ranges are checked on every recorded write: they are kept sorted
// and merged, so a long list costs a binary search, and an unsorted, overlapping one still answers right
static void TestHistoryFilterRanges()
{
    Workbench wb;
    Recorder::HistoryFilter f;
    for (u32 i = 0; i < 50000; i++)
    {
        u32 s = 0x02000000 + (49999 - i) * 0x20;             // newest first
        f.Include.push_back({s, s + 0x10});
    }
    f.Include.push_back({0x02000008, 0x02000018});          // overlaps the first
    f.Include.push_back({0x02000040 + 0x10, 0x02000040 + 0x20}); // joins the third and the fourth into one
    f.Include.push_back({5, 5});                             // empty
    f.Include.push_back({10, 4});                            // backwards
    f.ExcludeWriters = {{0x150, 0x300}, {0x100, 0x200}};
    wb.Rec.SetHistoryFilter(f);
    const auto& in = wb.Rec.GetHistoryFilter().Include;
    bool sorted = true;
    for (size_t i = 1; i < in.size(); i++) if (in[i].first <= in[i - 1].second) sorted = false;
    CHECK(sorted && in.size() == 49999 && in.front() == std::make_pair(0x02000000u, 0x02000018u));
    CHECK(wb.Rec.GetHistoryFilter().ExcludeWriters == (std::vector<std::pair<u32, u32>>{{0x100, 0x300}}));
    auto inc = [&](u32 a, u32 n) { return wb.Rec.HistoryIncludes(a, n); };
    CHECK(inc(0x02000000, 1) && inc(0x02000017, 1) && !inc(0x02000018, 8) && inc(0x0200001C, 8));
    CHECK(inc(0x0200004F, 1) && inc(0x02000050, 1) && inc(0x0200006F, 1) && !inc(0x02000070, 0x10));
    CHECK(inc(0x02000000 + 49999 * 0x20 + 0xF, 1) && !inc(0x02000000 + 49999 * 0x20 + 0x10, 4));
    CHECK(!inc(0, 4) && !inc(0x01FFFFF0, 0x10) && inc(0x01FFFFF0, 0x11) && !inc(0xFFFFFFFE, 4));
    CHECK(wb.Rec.HistoryExcludesWriter(0x100) && wb.Rec.HistoryExcludesWriter(0x2FF));
    CHECK(!wb.Rec.HistoryExcludesWriter(0xFF) && !wb.Rec.HistoryExcludesWriter(0x300));
}

// API parameters from JSON: numbers out of a field's range are refused, not cast; an address written as a
// string is hex, as the window reads it (0x, $ and h are optional), while a count written as a string is decimal
static void TestApiParams()
{
    auto name = [](QJsonObject o, WBApi::KbNameParams& p) { QString e; o.insert("name", "x"); return WBApi::KbNameParams::fromJson(o, p, e); };
    WBApi::KbNameParams kp;
    CHECK(name({{"addr", 0x02000000}, {"overlay", -1}}, kp) && kp.overlay == -1);
    CHECK(!name({{"addr", 0x02000000}, {"overlay", 1e300}}, kp));
    CHECK(!name({{"addr", 0x02000000}, {"overlay", -1e300}}, kp));
    CHECK(!name({{"addr", 0x02000000}, {"overlay", 2147483648.0}}, kp));
    CHECK(!name({{"addr", 0x02000000}, {"overlay", 1.5}}, kp));
    auto rec = [](QJsonObject o, WBApi::RecordingParams& p) { QString e; return WBApi::RecordingParams::fromJson(o, p, e); };
    WBApi::RecordingParams rp;
    CHECK(rec({{"history_cap", 4000000}}, rp) && rp.history_cap && *rp.history_cap == 4000000);
    CHECK(!rec({{"history_cap", 1e300}}, rp));
    CHECK(!rec({{"history_cap", 1.5}}, rp));

    auto addr = [&](const char* s) -> qint64 { WBApi::KbNameParams p; return name({{"addr", s}}, p) ? (qint64)p.addr : -1; };
    CHECK(addr("0x02000000") == 0x02000000);
    CHECK(addr("02000000") == 0x02000000);
    CHECK(addr("2000000") == 0x02000000);
    CHECK(addr("0200001A") == 0x0200001A);
    CHECK(addr("$02000000") == 0x02000000);
    CHECK(addr("02000000h") == 0x02000000);
    CHECK(addr("0x") == -1 && addr("") == -1 && addr("x2") == -1 && addr("100000000") == -1);
    auto size = [](const char* s) -> qint64
    {
        WBApi::MemoryReadParams p;
        QString e;
        return WBApi::MemoryReadParams::fromJson({{"addr", 0x02000000}, {"size", s}}, p, e) ? (qint64)p.size : -1;
    };
    CHECK(size("32") == 32 && size("0x20") == 32 && size("1A") == -1);
}

// the API server's connections: one goes when its client disconnects; stopping the server (a settings
// change, quitting) closes the rest while clients are still connected. Each is deleted exactly once
static void TestApiConnections()
{
    auto waitFor = [](auto&& cond)
    {
        for (int i = 0; i < 400 && !cond(); i++) { QCoreApplication::processEvents(); QThread::msleep(5); }
        return cond();
    };
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    ApiConnections conns;
    QObject::connect(&server, &QTcpServer::newConnection, &conns, [&]()
    {
        while (QTcpSocket* s = server.nextPendingConnection()) conns.Add(s, false);
    });
    int live = ApiConnections::Conn::Live;

    QTcpSocket a;
    a.connectToHost(QHostAddress::LocalHost, server.serverPort());
    CHECK(waitFor([&]() { return conns.List().size() == 1; }));
    a.disconnectFromHost();
    CHECK(waitFor([&]() { return conns.List().empty(); }));
    CHECK(ApiConnections::Conn::Live == live);

    QTcpSocket b, c;
    b.connectToHost(QHostAddress::LocalHost, server.serverPort());
    c.connectToHost(QHostAddress::LocalHost, server.serverPort());
    CHECK(waitFor([&]() { return conns.List().size() == 2; }));
    CHECK(ApiConnections::Conn::Live == live + 2);
    printf("  closing with two clients connected\n");
    fflush(stdout);
    conns.CloseAll();
    CHECK(conns.List().empty());
    CHECK(ApiConnections::Conn::Live == live);
    CHECK(waitFor([&]() { return b.state() == QAbstractSocket::UnconnectedState && c.state() == QAbstractSocket::UnconnectedState; }));
}

static void TestTypes()
{
    std::map<QString, WBKB::Type> known;
    WBKB::Type t;
    QString err;
    CHECK(WBKB::ParseTypeDefinition("struct Vec { fx32 x, y, z; };", known, t, err));
    CHECK(t.Name == "Vec" && t.Size == 12 && t.Fields.size() == 3 && t.Fields[2].Offset == 8);
    known[t.Name] = t;
    CHECK(WBKB::ParseTypeDefinition("typedef struct { u8 flag; u32 speed; struct Vec pos; u16 id[3]; s8* name; } Car;", known, t, err));
    CHECK(t.Name == "Car" && t.Fields.size() == 5);
    CHECK(t.Fields[1].Offset == 4 && t.Fields[2].Offset == 8 && t.Fields[3].Offset == 20 && t.Fields[3].Count == 3 && t.Fields[4].Offset == 28);
    CHECK(t.Size == 32);
    CHECK(WBKB::ParseTypeDefinition("union U { u8 b; u32 w; };", known, t, err) && t.Size == 4 && t.Fields[1].Offset == 0);
    CHECK(WBKB::ParseTypeDefinition("struct Bits { u32 a : 3; u32 b : 5; u16 c; };", known, t, err));
    CHECK(t.Fields[1].Offset == 0 && t.Fields[1].BitOffset == 3 && t.Fields[2].Offset == 4 && t.Size == 8);
    CHECK(WBKB::ParseTypeDefinition("enum State { A, B = 4, C };", known, t, err) && t.Size == 4);
    CHECK(!WBKB::ParseTypeDefinition("struct Bad { Unknown x; };", known, t, err));
    CHECK(WBKB::ParseTypeDefinition("typedef u32 Handle;", known, t, err) && t.Size == 4);
}

static void TestAutoFunctions()
{
    QTemporaryDir dir;
    QString path = dir.filePath("kb.db");
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());
    WBKB::KnowledgeStore kb;
    QString err;
    CHECK(kb.Open(path, r, "tester", err));
    CHECK(kb.SchemaVersion() >= 6);
    CHECK(kb.SetLabel(0, 0x02000500, "keep_me", -1, err));
    CHECK(kb.CanUndo());
    std::vector<WBKB::KnowledgeStore::AutoFunction> batch = {
        {0, 0x02001000, 0x20, false, -1},
        {0, 0x02001000, 0x20, false, -1},
        {0, 0x02001010, 0x10, false, -1},
    };
    int n = kb.DefineAutoFunctions(batch, err);
    CHECK(n == 1);
    const WBKB::Function* f = kb.FunctionStarting(0, 0x02001000);
    CHECK(f && f->Name == "sub_02001000" && f->AuthorKind == "system" && f->Size == 0x20);
    CHECK(kb.FunctionAt(0, 0x02001010) == f);
    qint64 autoId = f->Id;
    CHECK(kb.CanUndo());
    CHECK(kb.Undo(err));
    CHECK(!kb.LabelAt(0, 0x02000500));
    CHECK(kb.FunctionStarting(0, 0x02001000) && kb.FunctionStarting(0, 0x02001000)->Name == "sub_02001000");
    CHECK(kb.DefineAutoFunctions(batch, err) == 0);
    CHECK(kb.ReviseAutoSizes({{autoId, 0x10}}, err) == 1);
    CHECK(kb.FunctionStarting(0, 0x02001000)->Size == 0x10);
    CHECK(kb.UpsertFunction(0, 0x02001200, 0x10, "thumb", "Already", -1, err));
    batch = {{0, 0x02001200, 0x30, true, -1}};
    CHECK(kb.DefineAutoFunctions(batch, err) == 0);
    CHECK(kb.FunctionStarting(0, 0x02001200)->Name == "Already");
    const WBKB::Function* already = kb.FunctionStarting(0, 0x02001200);
    u32 sz = already->Size;
    qint64 aid = already->Id;
    CHECK(kb.ReviseAutoSizes({{aid, 0x4}}, err) == 0);
    CHECK(kb.FunctionStarting(0, 0x02001200)->Size == sz);
}

static const WBSpan::Resize* resized(const WBSpan::Decision& d, u32 addr)
{
    for (const WBSpan::Resize& r : d.Resizes) if (r.Addr == addr) return &r;
    return nullptr;
}

static const WBSpan::Entry* created(const WBSpan::Decision& d, u32 addr)
{
    for (const WBSpan::Entry& e : d.Create) if (e.Addr == addr) return &e;
    return nullptr;
}

static void TestSpanDecide()
{
    {
        WBSpan::Decision d = WBSpan::Decide({{0x1000, 0x100, true}}, {{0x1010, 0x20, false}});
        const WBSpan::Resize* r = resized(d, 0x1000);
        const WBSpan::Entry* c = created(d, 0x1010);
        CHECK(r && r->NewSize == 0x10);
        CHECK(c && c->Size == 0x20);
        CHECK(d.Notches.empty());
    }
    {
        WBSpan::Decision d = WBSpan::Decide({{0x1000, 0x100, false}}, {{0x1010, 0x20, false}});
        CHECK(d.Resizes.empty());
        CHECK(d.Create.empty());
        CHECK(d.Notches.size() == 1 && d.Notches[0].FuncAddr == 0x1000 && d.Notches[0].EntryAddr == 0x1010);
    }
    {
        WBSpan::Decision d = WBSpan::Decide({}, {{0x2000, 0x800, true}});
        const WBSpan::Entry* c = created(d, 0x2000);
        CHECK(c && c->Size == 0x800);
        CHECK(d.Open.size() == 1 && d.Open[0] == 0x2000);
    }
    {
        WBSpan::Decision d = WBSpan::Decide({{0x1000, 0x300, true}}, {{0x1100, 0x200, false}, {0x1200, 0x10, false}});
        const WBSpan::Resize* r = resized(d, 0x1000);
        const WBSpan::Entry* a = created(d, 0x1100);
        const WBSpan::Entry* b = created(d, 0x1200);
        CHECK(r && r->NewSize == 0x100);
        CHECK(a && a->Size == 0x100);
        CHECK(b && b->Size == 0x10);
        CHECK(d.Open.empty());
    }
    {
        WBSpan::Decision d = WBSpan::Decide({{0x2000, 0x800, true}}, {{0x2000, 0x800, true}});
        CHECK(d.Create.empty());
        CHECK(d.Resizes.empty());
        CHECK(d.Open.size() == 1 && d.Open[0] == 0x2000);
    }
}

static void TestKnowledgeStore()
{
    QTemporaryDir dir;
    QString path = dir.filePath("kb.db");
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());

    WBKB::KnowledgeStore kb;
    QString err;
    CHECK(kb.Open(path, r, "tester", err));
    CHECK(kb.SchemaVersion() >= 1);
    CHECK(kb.ModuleFor(0, 0x02000400) && kb.ModuleFor(0, 0x02000400)->Name == "arm9");
    CHECK(kb.ModuleFor(0, 0x02100010) && kb.ModuleFor(0, 0x02100010)->Name == "ov9_0000");
    CHECK(kb.ModuleFor(0, 0x01FF8010) && kb.ModuleFor(0, 0x01FF8010)->Name == "itcm");

    // functions
    qint64 fid = 0;
    CHECK(kb.UpsertFunction(0, 0x02000400, 0x40, "arm", "Player_Update", -1, err, &fid));
    CHECK(fid > 0);
    const WBKB::Function* f = kb.FunctionAt(0, 0x02000420);
    CHECK(f && f->Name == "Player_Update" && f->Offset == 0x400 && f->Status == "not_started");
    CHECK(kb.NameAt(0, 0x02000400) == "Player_Update");
    CHECK(kb.NameAt(0, 0x02000410, -1, true) == "Player_Update+0x10");
    CHECK(!kb.UpsertFunction(0, 0x02000402, 0x10, "arm", "", -1, err));   // misaligned ARM
    CHECK(!kb.UpsertFunction(0, 0x05000000, 0x10, "arm", "", -1, err));   // palette RAM: no module
    // code outside the ROM's modules (relocated or generated) lands in a RAM region module
    CHECK(kb.UpsertFunction(0, 0x02300000, 0x10, "thumb", "Relocated", -1, err));
    CHECK(kb.FunctionAt(0, 0x02300004) && kb.ModuleById(kb.FunctionAt(0, 0x02300004)->ModuleId)->Name == "ram9_main");
    CHECK(kb.ModuleFor(0, 0x02000400)->Kind == "static");   // ROM modules take precedence
    CHECK(kb.SchemaVersion() >= 3);
    CHECK(kb.UpsertFunction(0, 0x02100000, 0x20, "thumb", "Ov_Init", 0, err));
    CHECK(kb.FunctionAt(0, 0x02100004, 0) && kb.FunctionAt(0, 0x02100004, 0)->Thumb());

    // rename, labels, undo/redo
    CHECK(kb.Rename(0, 0x02000400, "Player_Tick", -1, err));
    CHECK(kb.NameAt(0, 0x02000400) == "Player_Tick");
    CHECK(kb.Undo(err));
    CHECK(kb.NameAt(0, 0x02000400) == "Player_Update");
    CHECK(kb.Redo(err));
    CHECK(kb.NameAt(0, 0x02000400) == "Player_Tick");
    CHECK(kb.SetLabel(0, 0x02000500, "loop_top", -1, err));
    CHECK(kb.LabelAt(0, 0x02000500) && kb.LabelAt(0, 0x02000500)->Name == "loop_top");
    u32 a; int cpu;
    CHECK(kb.LookupName("loop_top", a, cpu) && a == 0x02000500);
    CHECK(kb.LookupName("func_02000600", a, cpu) && a == 0x02000600);

    // types and data objects
    CHECK(kb.DefineType("struct Car { u32 flags; fx32 speed; u16 id; };", err));
    CHECK(kb.TypeByName("Car") && kb.TypeByName("Car")->Size == 12);
    CHECK(kb.DefineData(0, 0x02000600, 0, "Car", "gPlayerCar", -1, err));
    const WBKB::DataObject* d = kb.DataObjectAt(0, 0x02000604);
    CHECK(d && d->Name == "gPlayerCar" && d->Size == 12);
    CHECK(kb.NameAt(0, 0x02000604, -1, true) == "gPlayerCar.speed");
    CHECK(kb.DefineData(0, 0x02200000, 16, "u32", "heap_obj", -1, err));   // outside modules: stored by region
    CHECK(kb.DataObjectAt(0, 0x02200008) != nullptr);
    CHECK(!kb.DefineData(0, 0x02000700, 4, "NoSuchType", "", -1, err));
    // without a size, an array of a basic type takes its size from the type
    CHECK(kb.DefineData(0, 0x02000740, 0, "u32[4]", "gTable", -1, err));
    CHECK(kb.DataObjectAt(0, 0x0200074C) && kb.DataObjectAt(0, 0x0200074C)->Size == 16 && !kb.DataObjectAt(0, 0x02000750));

    // annotations of several target kinds
    WBKB::Annotation an;
    an.TargetKind = "address";
    int mid; u32 off;
    kb.ModuleOffset(0, 0x02000404, -1, mid, off);
    an.ModuleId = mid; an.Offset = off;
    an.Comment = "reads the input state";
    an.Tags = "input, todo";
    CHECK(kb.SaveAnnotation(an, err) && an.Id > 0);
    CHECK(kb.AnnotationsAt(0, 0x02000404).size() == 1);
    CHECK(kb.Annotations().back().TagList().contains("todo"));
    WBKB::Annotation reg;
    reg.TargetKind = "register";
    reg.FunctionId = fid;
    reg.Register = "r4";
    reg.Comment = "player state";
    CHECK(kb.SaveAnnotation(reg, err));
    CHECK(kb.RegisterAnnotations(kb.FunctionAt(0, 0x02000400), 0x02000410).size() == 1);
    WBKB::Annotation fld;
    fld.TargetKind = "struct_field";
    fld.TypeName = "Car";
    fld.FieldName = "speed";
    fld.Comment = "fx32, units per frame";
    CHECK(kb.SaveAnnotation(fld, err));
    WBKB::Annotation bad;
    bad.TargetKind = "address";
    bad.Comment = "";
    CHECK(!kb.SaveAnnotation(bad, err));
    an.Comment = "reads the touch and key state";
    CHECK(kb.SaveAnnotation(an, err));
    CHECK(kb.AnnotationsAt(0, 0x02000404)[0]->Comment.startsWith("reads the touch"));
    CHECK(kb.Undo(err));
    CHECK(kb.AnnotationsAt(0, 0x02000404)[0]->Comment == "reads the input state");
    CHECK(kb.DeleteAnnotation(an.Id, err));
    CHECK(kb.AnnotationsAt(0, 0x02000404).empty());
    CHECK(kb.Undo(err));
    CHECK(kb.AnnotationsAt(0, 0x02000404).size() == 1);

    // bookmarks
    CHECK(kb.AddBookmark(0, 0x02000400, "start here", err));
    CHECK(kb.Bookmarks().size() == 1);

    // status derives from evidence, never set by hand
    QJsonObject o;
    o["status"] = "matched";
    CHECK(!kb.UpdateFunction(fid, o, err));
    WBKB::BuildResult br;
    br.FunctionId = fid;
    br.Compiler = "mwcc 2.0/sp1";
    br.Score = 87.5;
    br.Matched = false;
    CHECK(kb.RecordBuildResult(br, err));
    CHECK(kb.FunctionById(fid)->Status == "partially_matched" && kb.FunctionById(fid)->LastScore == 87.5);
    br.Score = 100;
    br.Matched = true;
    CHECK(kb.RecordBuildResult(br, err));
    CHECK(kb.FunctionById(fid)->Status == "matched");
    CHECK(kb.BuildResults(fid).size() == 2);

    // xrefs and decompiler cache
    CHECK(kb.AddXref(0, 0x02000500, 0x02000400, "call", "static", err));
    CHECK(kb.XrefsTo(0, 0x02000400).size() == 1);
    CHECK(kb.SetDecompiled(fid, "ghidra", "void FUN_02000400(void) {}", err));
    CHECK(kb.DecompilerText(fid).contains("FUN_02000400"));

    // session import
    WBKB::EvidenceRow ev {fid, 10, 120, 0x40, "[]", "[]", "[]"};
    WBKB::AssetLinkRow al {0, "/a.bin", 0x3000, 0x10, "card_read", 0x02200000, 0x02200010, 0x02000404, 5};
    CHECK(kb.ImportSession(dir.filePath("s.dswbs"), QString::fromStdString(r.Sha1Hex()), 0, 100, 0, {ev}, {al}, err));

    // every write is in the change log with its author
    sqlite3* db;
    sqlite3_open(path.toUtf8().constData(), &db);
    sqlite3_stmt* s;
    sqlite3_prepare_v2(db, "SELECT count(*), count(DISTINCT author_id) FROM change_log", -1, &s, nullptr);
    sqlite3_step(s);
    CHECK(sqlite3_column_int(s, 0) >= 12 && sqlite3_column_int(s, 1) == 1);
    sqlite3_finalize(s);
    sqlite3_close(db);

    // an agent's edits carry its author and session, and each author undoes only its own changes
    qint64 bot = kb.AgentAuthor("bot");
    CHECK(bot && bot != kb.PersonAuthor() && kb.AgentAuthor("tester") != kb.PersonAuthor());   // a person's name stays theirs
    {
        WBKB::KnowledgeStore::AuthorScope scope(kb, bot, "bot#1");
        CHECK(kb.SetLabel(0, 0x02000580, "agent_label", -1, err));
    }
    CHECK(kb.SetLabel(0, 0x02000584, "person_label", -1, err));
    CHECK(!kb.CanUndo(bot) || kb.CanUndo());
    CHECK(kb.Undo(err));                                        // the person's label goes, the agent's stays
    CHECK(kb.NameAt(0, 0x02000584).isEmpty() && kb.NameAt(0, 0x02000580) == "agent_label");
    CHECK(kb.Undo(err, bot));                                   // the agent undoes its own
    CHECK(kb.NameAt(0, 0x02000580).isEmpty());
    CHECK(!kb.CanUndo(bot) && kb.CanRedo(bot));
    CHECK(kb.Redo(err, bot) && kb.NameAt(0, 0x02000580) == "agent_label");
    auto changes = kb.RecentChanges(0, 10);
    // newest first: the person's undone label, then the agent's (undone, then redone)
    CHECK(changes.size() >= 2 && changes[0].AuthorKind == "person" && changes[0].Undone);
    CHECK(changes.size() >= 2 && changes[1].AuthorKind == "agent" && changes[1].Author == "bot" && changes[1].AgentSession == "bot#1" && !changes[1].Undone);

    // deleting a function takes its build results and annotations along, as one change that undo reverses
    qint64 gfid = 0;
    CHECK(kb.UpsertFunction(0, 0x02000440, 0x20, "arm", "grouped", -1, err, &gfid));
    WBKB::BuildResult gbr;
    gbr.FunctionId = gfid;
    gbr.Compiler = "test";
    gbr.Score = 50;
    CHECK(kb.RecordBuildResult(gbr, err));
    WBKB::Annotation fa;
    fa.TargetKind = "function";
    fa.FunctionId = gfid;
    fa.Comment = "goes with the function";
    CHECK(kb.SaveAnnotation(fa, err));
    size_t nann = kb.Annotations().size();
    CHECK(kb.DeleteFunction(gfid, err));
    CHECK(!kb.FunctionById(gfid) && kb.Annotations().size() == nann - 1 && kb.BuildResults(gfid).empty());
    CHECK(kb.Undo(err));                                        // one undo restores all three
    CHECK(kb.FunctionById(gfid) && kb.Annotations().size() == nann && kb.BuildResults(gfid).size() == 1);
    CHECK(kb.Redo(err) && !kb.FunctionById(gfid) && kb.BuildResults(gfid).empty());
    CHECK(kb.Undo(err) && kb.FunctionById(gfid));
    CHECK(kb.Undo(err) && kb.Annotations().size() == nann - 1); // then the annotation's own insert

    kb.Close();
    sqlite3_open(path.toUtf8().constData(), &db);
    sqlite3_prepare_v2(db, "SELECT count(*) FROM runtime_evidence", -1, &s, nullptr);
    sqlite3_step(s);
    CHECK(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);
    // vocabularies are enforced by the schema
    CHECK(sqlite3_exec(db, "UPDATE function SET mode = 'mips'", nullptr, nullptr, nullptr) != SQLITE_OK);
    CHECK(sqlite3_exec(db, "INSERT INTO annotation(target_kind, comment, author_id) VALUES ('nonsense', 'x', 1)", nullptr, nullptr, nullptr) != SQLITE_OK);
    sqlite3_close(db);

    // reopening keeps everything and does not re-run migrations; a different ROM is refused
    WBKB::KnowledgeStore kb2;
    CHECK(kb2.Open(path, r, "tester", err));
    CHECK(kb2.FunctionById(fid) && kb2.Annotations().size() == 3);
    kb2.Close();
    std::vector<u8> other = rom;
    other[0x200] ^= 1;
    NitroROM r2;
    r2.Load(other.data(), (u32)other.size(), (u32)other.size());
    WBKB::KnowledgeStore kb3;
    CHECK(!kb3.Open(path, r2, "tester", err));
}

// migration 0007: library code has one subsystem tag, in the functions and in the change log
static void TestKbLibrarySubsystem()
{
    QTemporaryDir dir;
    QString path = dir.filePath("kb.db");
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());
    QString err;
    qint64 fid = 0;
    {
        WBKB::KnowledgeStore kb;
        CHECK(kb.Open(path, r, "tester", err));
        CHECK(kb.SchemaVersion() >= 7);
        CHECK(kb.UpsertFunction(0, 0x02000400, 0x40, "arm", "Lib_Func", -1, err, &fid));
        CHECK(!kb.UpdateFunction(fid, {{"subsystem", "middleware"}}, err));
        CHECK(kb.UpdateFunction(fid, {{"subsystem", "library"}}, err));
        kb.Close();
    }

    // a store from before 0007: a tag that has since left the vocabulary, on the function and in a
    // logged change
    sqlite3* db = nullptr;
    sqlite3_open(path.toUtf8().constData(), &db);
    QByteArray old = QString(
        "INSERT INTO vocab_subsystem(name) VALUES ('middleware');"
        "UPDATE function SET subsystem = 'middleware' WHERE id = %1;"
        "UPDATE change_log SET after = json_set(after, '$.subsystem', 'middleware') WHERE table_name = 'function' AND row_id = %1 AND action = 'update';"
        "DELETE FROM schema_migrations WHERE version = 7;").arg(fid).toUtf8();
    CHECK(sqlite3_exec(db, old.constData(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    {
        WBKB::KnowledgeStore kb;
        CHECK(kb.Open(path, r, "tester", err));
        const WBKB::Function* f = kb.FunctionById(fid);
        CHECK(f && f->Subsystem == "library" && f->Status == "not_started");
        kb.Close();
    }

    sqlite3_open(path.toUtf8().constData(), &db);
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM change_log WHERE table_name = 'function' AND "
                           "json_extract(after, '$.subsystem') = 'library'", -1, &s, nullptr);
    sqlite3_step(s);
    CHECK(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);
    sqlite3_prepare_v2(db, "SELECT group_concat(name, ',') FROM (SELECT name FROM vocab_subsystem ORDER BY name)", -1, &s, nullptr);
    sqlite3_step(s);
    CHECK_EQ_STR(std::string((const char*)sqlite3_column_text(s, 0)), "game,library,msl,runtime,unknown");
    sqlite3_finalize(s);
    sqlite3_close(db);
}

// kb.list and kb.function.get: the knowledge store's lists with the panels' filters, paged
// undo and redo of a function's creation, after rows it doesn't own came to refer to it
static void TestKbUndoReferenced()
{
    QTemporaryDir dir;
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());
    WBKB::KnowledgeStore kb;
    QString err;
    CHECK(kb.Open(dir.filePath("kb.db"), r, "tester", err));

    // another author's annotation refers to the person's function: undoing the function can't remove it,
    // and must not block the person's earlier changes
    CHECK(kb.SetLabel(0, 0x02000500, "earlier_label", -1, err));
    qint64 fid = 0;
    CHECK(kb.UpsertFunction(0, 0x02000400, 0x40, "arm", "PersonFunc", -1, err, &fid));
    qint64 bot = kb.AgentAuthor("bot");
    {
        WBKB::KnowledgeStore::AuthorScope scope(kb, bot, "bot#1");
        WBKB::Annotation fa;
        fa.TargetKind = "function";
        fa.FunctionId = fid;
        fa.Comment = "agent note";
        CHECK(kb.SaveAnnotation(fa, err));
    }
    size_t nann = kb.Annotations().size();
    err.clear();
    CHECK(!kb.Undo(err) && !err.isEmpty());                     // refused, and says why
    CHECK(kb.FunctionById(fid) && kb.Annotations().size() == nann);
    CHECK(kb.Undo(err) && !kb.LabelAt(0, 0x02000500));          // the earlier change still undoes
    CHECK(!kb.CanUndo());

    // build results, decompiler text and runtime evidence come and go with the function
    qint64 g = 0;
    CHECK(kb.UpsertFunction(0, 0x02000600, 0x20, "arm", "G", -1, err, &g));
    WBKB::BuildResult br;
    br.FunctionId = g;
    br.Compiler = "test";
    br.Score = 75;
    CHECK(kb.RecordBuildResult(br, err));
    CHECK(kb.SetDecompiled(g, "ghidra", "void G(void) {}", err));
    CHECK(kb.Undo(err) && !kb.FunctionById(g) && kb.BuildResults(g).empty());
    CHECK(kb.Redo(err) && kb.FunctionById(g));
    CHECK(kb.BuildResults(g).size() == 1 && kb.BuildResults(g)[0].Score == 75);
    CHECK(kb.DecompilerText(g) == "void G(void) {}");
}

static void TestKbList()
{
    QTemporaryDir dir;
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());
    WBKB::KnowledgeStore kb;
    QString err;
    CHECK(kb.Open(dir.filePath("kb.db"), r, "tester", err));
    qint64 fid = 0;
    CHECK(kb.UpsertFunction(0, 0x02000400, 0x40, "arm", "Player_Update", -1, err, &fid));
    CHECK(kb.UpsertFunction(0, 0x02000480, 0x20, "thumb", "", -1, err));
    CHECK(kb.UpsertFunction(0, 0x02100000, 0x20, "thumb", "Ov_Init", 0, err));
    qint64 bot = kb.AgentAuthor("bot");
    {
        WBKB::KnowledgeStore::AuthorScope scope(kb, bot, "bot#1");
        CHECK(kb.UpsertFunction(0, 0x02000500, 0x10, "arm", "Bot_Func", -1, err));
        CHECK(kb.SetLabel(0, 0x02000510, "bot_label", -1, err));
        CHECK(kb.AddBookmark(0, 0x02000500, "bot mark", err));
        CHECK(kb.SetDecodeOverride(0, 0x02000520, 0x02000528, "data", -1, err));
    }
    CHECK(kb.SetLabel(0, 0x02000420, "loop_top", -1, err));
    CHECK(kb.DefineType("struct Car { u32 flags; fx32 speed; u16 id; };", err));
    CHECK(kb.DefineData(0, 0x02000600, 0, "Car", "gPlayerCar", -1, err));
    CHECK(kb.DefineData(0, 0x06000000, 16, "u32", "vram_obj", -1, err));    // outside every module
    CHECK(kb.AddBookmark(0, 0x02000400, "start here", err));
    WBKB::Annotation an;
    CHECK(kb.ResolveAnnotationTarget(an, 0, 0x02000404, true, 0, -1, false, err));
    an.Comment = "reads the input state";
    an.Tags = "input, todo";
    CHECK(kb.SaveAnnotation(an, err));
    WBKB::Annotation reg;
    reg.TargetKind = "register";
    reg.Register = "r4";
    CHECK(kb.ResolveAnnotationTarget(reg, 0, 0x02000410, true, 0, -1, true, err));
    reg.Comment = "player state";
    CHECK(kb.SaveAnnotation(reg, err));
    WBKB::Annotation fld;
    fld.TargetKind = "struct_field";
    fld.TypeName = "Car";
    fld.FieldName = "speed";
    fld.Comment = "units per frame";
    CHECK(kb.SaveAnnotation(fld, err));

    auto list = [&](const QString& kind, std::function<void(WBApi::KbListParams&)> set = {}) -> WBApi::KbListResult
    {
        WBApi::KbListParams p;
        p.kind = kind;
        if (set) set(p);
        WBApi::KbListResult res;
        err.clear();
        if (!WBQ::KbList(kb, p, res, err)) res.kind = "error";
        return res;
    };
    using P = WBApi::KbListParams;

    // functions: by address, with the Functions panel's name-or-address filter
    WBApi::KbListResult fl = list("functions");
    CHECK(fl.functions && fl.functions->size() == 4 && fl.total == 4 && !fl.truncated && !fl.labels);
    CHECK(fl.functions && (*fl.functions)[0].addr == 0x02000400 && (*fl.functions)[0].name == "Player_Update"
          && (*fl.functions)[0].module == "arm9" && (*fl.functions)[0].overlay == -1 && (*fl.functions)[0].author_kind == "person");
    CHECK(fl.functions && fl.functions->back().module == "ov9_0000" && fl.functions->back().overlay == 0);
    CHECK(list("functions", [](P& p) { p.filter = "player"; }).total == 1);
    CHECK(list("functions", [](P& p) { p.filter = "func_02000480"; }).total == 1);   // the shown name of an unnamed one
    CHECK(list("functions", [](P& p) { p.filter = "2100000"; }).total == 1);
    CHECK(list("functions", [](P& p) { p.author = "agents"; }).total == 1);
    CHECK(list("functions", [](P& p) { p.author = "bot"; }).total == 1);
    CHECK(list("functions", [](P& p) { p.author = "people"; }).total == 3);
    CHECK(list("functions", [](P& p) { p.module = "OV9_0000"; }).total == 1);
    CHECK(list("functions", [](P& p) { p.cpu = "arm7"; }).total == 0);
    CHECK(list("functions", [](P& p) { p.addr = 0x02000430; }).total == 1);           // the function containing it
    CHECK(list("functions", [](P& p) { p.addr = 0x02000400; p.end = 0x02000500; }).total == 2);
    CHECK(list("functions", [](P& p) { p.status = "not_started"; }).total == 4);
    CHECK(list("functions", [&](P& p) { p.id = (u32)fid; }).total == 1);
    // paging
    WBApi::KbListResult pg = list("functions", [](P& p) { p.offset = 1; p.limit = 2; });
    CHECK(pg.functions && pg.functions->size() == 2 && pg.total == 4 && pg.truncated && pg.next_offset == 3
          && (*pg.functions)[0].addr == 0x02000480);
    pg = list("functions", [](P& p) { p.offset = 3; p.limit = 2; });
    CHECK(pg.functions && pg.functions->size() == 1 && !pg.truncated);
    pg = list("functions", [](P& p) { p.limit = 0; });
    CHECK(pg.functions && pg.functions->empty() && pg.total == 4 && pg.truncated);
    QJsonObject js = list("labels").toJson();
    CHECK(js.contains("labels") && !js.contains("functions"));                       // only the requested list

    // labels, data objects, types
    WBApi::KbListResult ll = list("labels", [](P& p) { p.author = "people"; });
    CHECK(ll.labels && ll.labels->size() == 1 && (*ll.labels)[0].name == "loop_top" && (*ll.labels)[0].addr == 0x02000420);
    WBApi::KbListResult dl = list("data_objects");
    CHECK(dl.data_objects && dl.data_objects->size() == 2);
    CHECK(dl.data_objects && (*dl.data_objects)[0].name == "gPlayerCar" && (*dl.data_objects)[0].size == 12
          && (*dl.data_objects)[0].type == "Car" && (*dl.data_objects)[0].cpu == "arm9");
    CHECK(dl.data_objects && (*dl.data_objects)[1].cpu == "any" && (*dl.data_objects)[1].module.isEmpty()
          && (*dl.data_objects)[1].region == "vram");
    CHECK(list("data_objects", [](P& p) { p.filter = "car"; }).total == 1);           // by type too
    CHECK(list("data_objects", [](P& p) { p.cpu = "arm7"; }).total == 1);             // region data: either CPU
    CHECK(list("data_objects", [](P& p) { p.module = "arm9"; }).total == 1);
    WBApi::KbListResult tl = list("types");
    CHECK(tl.types && tl.types->size() == 1 && (*tl.types)[0].size == 12 && (*tl.types)[0].fields.size() == 3
          && (*tl.types)[0].fields[1].name == "speed" && (*tl.types)[0].fields[1].offset == 4 && (*tl.types)[0].author_kind == "person");

    // annotations: author, target kind, tag, and where they are
    WBApi::KbListResult al = list("annotations");
    CHECK(al.annotations && al.annotations->size() == 3);
    CHECK(al.annotations && (*al.annotations)[0].has_addr && (*al.annotations)[0].addr == 0x02000404
          && (*al.annotations)[0].tags.size() == 2 && (*al.annotations)[0].tags[1] == "todo");
    CHECK(al.annotations && (*al.annotations)[1].reg == "r4" && (*al.annotations)[1].function == 0x02000400
          && !(*al.annotations)[1].has_addr && (*al.annotations)[1].cpu == "arm9");
    CHECK(al.annotations && (*al.annotations)[2].cpu.isEmpty() && (*al.annotations)[2].type_name == "Car");
    CHECK(list("annotations", [](P& p) { p.tag = "TODO"; }).total == 1);
    CHECK(list("annotations", [](P& p) { p.target_kind = "struct_field"; }).total == 1);
    CHECK(list("annotations", [](P& p) { p.filter = "player"; }).total == 1);
    CHECK(list("annotations", [](P& p) { p.addr = 0x02000400; p.end = 0x02000440; }).total == 2);   // by address and by function
    CHECK(list("annotations", [](P& p) { p.cpu = "arm9"; }).total == 2);              // a type field has no CPU

    // bookmarks, decode overrides, modules
    CHECK(list("bookmarks").total == 2 && list("bookmarks", [](P& p) { p.author = "agents"; }).total == 1);
    CHECK(list("bookmarks", [](P& p) { p.module = "arm9"; }).total == 2);
    WBApi::KbListResult ol = list("decode_overrides");
    CHECK(ol.decode_overrides && ol.decode_overrides->size() == 1 && (*ol.decode_overrides)[0].mode == "data"
          && (*ol.decode_overrides)[0].start == 0x02000520 && (*ol.decode_overrides)[0].conflict.isEmpty());
    CHECK(list("decode_overrides", [](P& p) { p.conflicts_only = true; }).total == 0);
    if (ol.decode_overrides && !ol.decode_overrides->empty())
    {
        std::map<qint64, QString> conflicts {{(*ol.decode_overrides)[0].id, "marked data, but executed 3 times"}};
        WBApi::KbListParams p;
        p.kind = "decode_overrides";
        p.conflicts_only = true;
        WBApi::KbListResult res;
        CHECK(WBQ::KbList(kb, p, res, err, &conflicts) && res.total == 1 && (*res.decode_overrides)[0].conflict.startsWith("marked data"));
    }
    WBApi::KbListResult ml = list("modules", [](P& p) { p.filter = "ov9"; });
    CHECK(ml.modules && ml.modules->size() == 1 && (*ml.modules)[0].kind == "overlay" && (*ml.modules)[0].overlay_id == 0
          && (*ml.modules)[0].load == 0x02100000);
    CHECK(list("modules", [](P& p) { p.addr = 0x02000400; }).total == 3);             // arm9 and both CPUs' main RAM regions
    CHECK(list("modules", [](P& p) { p.addr = 0x02000400; p.cpu = "arm9"; }).total == 2);

    // refused: unknown kinds and filters a kind does not have
    CHECK(list("widgets").kind == "error" && err.contains("functions"));
    CHECK(list("labels", [](P& p) { p.status = "matched"; }).kind == "error" && err.contains("status"));
    CHECK(list("functions", [](P& p) { p.status = "done"; }).kind == "error");    CHECK(list("types", [](P& p) { p.cpu = "arm9"; }).kind == "error");
    CHECK(list("modules", [](P& p) { p.author = "people"; }).kind == "error");
    CHECK(list("functions", [](P& p) { p.addr = 0x02000500; p.end = 0x02000400; }).kind == "error");
    CHECK(list("functions", [](P& p) { p.cpu = "z80"; }).kind == "error");

    // one function: attempts, decompiler output with names, annotations, and the source file
    WBKB::BuildResult br;
    br.FunctionId = fid;
    br.Compiler = "mwcc";
    br.Score = 87.5;
    CHECK(kb.RecordBuildResult(br, err));
    br.Score = 100;
    br.Matched = true;
    CHECK(kb.RecordBuildResult(br, err));
    CHECK(kb.SetDecompiled(fid, "ghidra", "void FUN_02000400(void) { DAT_02000600 = 0; FUN_02000500(); }", err));
    QDir(dir.path()).mkpath("src");
    QString root = dir.filePath("src");
    QFile sf(dir.filePath("src/player.c"));
    CHECK(sf.open(QIODevice::WriteOnly) && sf.write("void Player_Update(void) {}\n") > 0);
    sf.close();
    QFile of(dir.filePath("secret.txt"));
    CHECK(of.open(QIODevice::WriteOnly) && of.write("not for the API\n") > 0);
    of.close();
    auto noOverlay = [](u32) { return -1; };
    const WBKB::Function* f = kb.FunctionById(fid);
    WBApi::KbFunctionDetail d = WBQ::KbFunctionDetail(kb, *f, dir.filePath("src/player.c"), root, true, 50, noOverlay);
    CHECK(d.function.status == "matched" && d.function.last_score == 100 && d.display_name == "Player_Update");
    CHECK(d.attempts.size() == 2 && d.attempts[0].matched && d.attempts[1].score == 87.5);   // newest first
    CHECK(d.decompiler_tool == "ghidra" && d.decompiled.contains("FUN_02000400")
          && d.decompiled_named == "void Player_Update(void) { gPlayerCar = 0; Bot_Func(); }");
    CHECK(d.annotations.size() == 2);                                                 // its address note and its register
    CHECK(d.source_status == "ok" && d.source_text.startsWith("void Player_Update"));
    d = WBQ::KbFunctionDetail(kb, *f, dir.filePath("src/../secret.txt"), root, true, 1, noOverlay);
    CHECK(d.source_status == "outside_source_root" && d.source_text.isEmpty() && d.attempts.size() == 1);
    d = WBQ::KbFunctionDetail(kb, *f, dir.filePath("secret.txt"), QString(), true, 50, noOverlay);
    CHECK(d.source_status == "outside_source_root" && d.source_text.isEmpty());
    d = WBQ::KbFunctionDetail(kb, *f, dir.filePath("src/missing.c"), root, true, 50, noOverlay);
    CHECK(d.source_status == "missing");
    d = WBQ::KbFunctionDetail(kb, *f, dir.filePath("src/player.c"), root, false, 50, noOverlay);
    CHECK(d.source_status == "not_requested" && d.source_text.isEmpty());
    d = WBQ::KbFunctionDetail(kb, *kb.FunctionAt(0, 0x02000480), QString(), root, true, 50, noOverlay);
    CHECK(d.source_status == "none" && d.display_name == "func_02000480" && d.attempts.empty() && d.decompiled_named.isEmpty());
}

// A small Octane VFS: vfs.zip (stored, deflated and duplicate members), vfs.bin and vfs.dat.
//   dir/a.bin           stored            file 0
//   dir/sub/b.nsbmd     deflated          file 1
//   c.bin               deflated, = a.bin file 2
// vfs.bin table: 0 dir "dir" (next 4), 1 a.bin (next 2), 2 dir "sub" (last), 3 b.nsbmd (last), 4 c.bin (last)
static void TestArchive()
{
    auto put16 = [](std::vector<u8>& v, u32 x) { v.push_back((u8)x); v.push_back((u8)(x >> 8)); };
    auto put32 = [&](std::vector<u8>& v, u32 x) { put16(v, x & 0xFFFF); put16(v, x >> 16); };
    u32 seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245 + 12345; return (u8)(seed >> 16); };
    std::vector<u8> A(300), B(5000);
    for (u8& x : A) x = rnd();
    for (size_t i = 0; i < B.size(); i++) B[i] = (i % 7 == 0) ? 0 : rnd();   // compressible but varied

    struct Z { std::string name; std::vector<u8> data; bool deflate; u32 lho = 0; std::vector<u8> packed; };
    std::vector<Z> files = {{"Dir/a.bin", A, false}, {"Dir/Sub/b.nsbmd", B, true}, {"c.bin", A, true}};
    std::vector<u8> zip;
    for (Z& f : files)
    {
        if (f.deflate)
        {
            z_stream zs {};
            deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            f.packed.resize(deflateBound(&zs, (uLong)f.data.size()));
            zs.next_in = f.data.data();
            zs.avail_in = (uInt)f.data.size();
            zs.next_out = f.packed.data();
            zs.avail_out = (uInt)f.packed.size();
            deflate(&zs, Z_FINISH);
            f.packed.resize(zs.total_out);
            deflateEnd(&zs);
        }
        else f.packed = f.data;
        f.lho = (u32)zip.size();
        put32(zip, 0x04034B50); put16(zip, 20); put16(zip, 0); put16(zip, f.deflate ? 8 : 0); put32(zip, 0);
        put32(zip, crc32(0, f.data.data(), (uInt)f.data.size())); put32(zip, (u32)f.packed.size()); put32(zip, (u32)f.data.size());
        put16(zip, (u32)f.name.size()); put16(zip, 0);
        zip.insert(zip.end(), f.name.begin(), f.name.end());
        zip.insert(zip.end(), f.packed.begin(), f.packed.end());
    }
    u32 cdStart = (u32)zip.size();
    std::vector<u32> cdOffsets;
    for (Z& f : files)
    {
        cdOffsets.push_back((u32)zip.size() - cdStart);
        put32(zip, 0x02014B50); put16(zip, 20); put16(zip, 20); put16(zip, 0); put16(zip, f.deflate ? 8 : 0); put32(zip, 0);
        put32(zip, crc32(0, f.data.data(), (uInt)f.data.size())); put32(zip, (u32)f.packed.size()); put32(zip, (u32)f.data.size());
        put16(zip, (u32)f.name.size()); put16(zip, 0); put16(zip, 0); put16(zip, 0); put16(zip, 0); put32(zip, 0); put32(zip, f.lho);
        zip.insert(zip.end(), f.name.begin(), f.name.end());
    }
    u32 cdSize = (u32)zip.size() - cdStart;
    put32(zip, 0x06054B50); put16(zip, 0); put16(zip, 0); put16(zip, 3); put16(zip, 3); put32(zip, cdSize); put32(zip, cdStart); put16(zip, 0);

    std::string names;
    auto name = [&](const char* n) { u32 o = (u32)names.size(); names += n; names += '\0'; return o; };
    u32 nDir = name("dir"), nA = name("a.bin"), nSub = name("sub"), nB = name("b.nsbmd"), nC = name("c.bin");
    std::vector<u8> bin = {0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF};
    auto ent = [&](u32 n, u32 next, u32 c, u32 d) { put16(bin, n); put16(bin, next); put16(bin, c); put16(bin, d); };
    ent(nDir, 4, 1, 0x7FFF);
    ent(nA, 2, 0xFFFF, 0);
    ent(nSub, 0xFFFF, 3, 0x7FFF);
    ent(nB, 0xFFFF, 0xFFFF, 1);
    ent(nC, 0xFFFF, 0xFFFF, 2);
    for (u32 o : cdOffsets) put32(bin, o);
    bin.insert(bin.end(), names.begin(), names.end());
    std::string dat = "5, 3, " + std::to_string(names.size()) + "\r\n";

    WBArc::ArchiveIndex arc;
    std::string err;
    CHECK(arc.Build(zip.data(), (u32)zip.size(), bin.data(), (u32)bin.size(), (const u8*)dat.data(), (u32)dat.size(), err));
    const auto& ms = arc.Members();
    CHECK(ms.size() == 3);
    if (ms.size() != 3) return;
    CHECK(ms[0].Ok && ms[1].Ok && ms[2].Ok);
    CHECK(ms[0].Method == 0 && ms[1].Method == 8);
    CHECK(arc.VfsAgree == 3);
    CHECK(ms[0].VfsEntry == 1 && ms[1].VfsEntry == 3 && ms[2].VfsEntry == 4);
    CHECK(ms[2].DuplicateOf == 0);
    CHECK(!memcmp(arc.Content(ms[1]), B.data(), B.size()));
    CHECK(arc.FindPath("/DIR/SUB/B.NSBMD") == 1);
    CHECK(arc.FindPath("dir\\a.bin") == 0);
    CHECK(arc.FindPath("missing.bin") == -1);
    std::string part;
    u32 po = 0;
    CHECK(arc.MemberAt(ms[1].DataOffset + 10, &part, &po) == 1 && part == "compressed data" && po == 10);
    CHECK(arc.MemberAt(ms[1].HeaderOffset + 4, &part) == 1 && part == "local header");
    CHECK(arc.MemberAt(cdStart + cdOffsets[2] + 5, &part) == 2 && part == "central directory record");
    CHECK(arc.MemberAt((u32)zip.size() - 4) == -1);
    CHECK_EQ_STR(arc.Describe(ms[0].DataOffset + 0x20), "Dir/a.bin, stored data +0x20");

    // RAM: noise, then b.nsbmd bytes 100..4100 at +0x1000 and a.bin at +0x3000
    std::vector<u8> ram(0x10000);
    for (u8& x : ram) x = rnd();
    memcpy(&ram[0x1000], &B[100], 4000);
    memcpy(&ram[0x3000], A.data(), A.size());
    ram[0x1000 - 1] = (u8)~B[99];                               // the copy's neighbours differ from the file
    ram[0x1000 + 4000] = (u8)~B[4100];
    const u32 base = 0x02000000;
    auto res = arc.Resolve(ram.data(), base, (u32)ram.size(), base + 0x1000 + 50, 16);
    CHECK(res.size() == 1 && res[0].Member == 1 && res[0].Addr == base + 0x1000 && res[0].Size == 4000 && res[0].MemberOffset == 100);
    res = arc.Resolve(ram.data(), base, (u32)ram.size(), base + 0x3000, 300);
    CHECK(res.size() == 1 && res[0].Member == 0 && res[0].Size == 300 && res[0].Alternatives == std::vector<int>{2});
    res = arc.Resolve(ram.data(), base, (u32)ram.size(), base + 0x8000, 0x100);
    CHECK(res.empty());                                         // noise matches nothing
    // a byte patched in place splits the run in two
    ram[0x1000 + 2000] ^= 0xFF;
    res = arc.Resolve(ram.data(), base, (u32)ram.size(), base + 0x1000, 4000);
    CHECK(res.size() == 2 && res[0].Size == 2000 && res[1].Addr == base + 0x1000 + 2001 && res[1].MemberOffset == 2101);
    auto loc = arc.Locate(1, ram.data(), base, (u32)ram.size());
    CHECK(loc.size() == 2 && loc[0].Member == 1);
    loc = arc.Locate(0, ram.data(), base, (u32)ram.size());
    CHECK(loc.size() == 1 && loc[0].Addr == base + 0x3000 && loc[0].Size == 300);

    // a truncated archive is refused
    CHECK(!arc.Build(zip.data(), 10, nullptr, 0, nullptr, 0, err));

    // vfs.dat counts the index can't hold: an entry count whose table size wraps past 4 GB is no index at all,
    // and an entry naming a file past the file table is skipped
    auto vfsEntries = [&](const std::string& d)
    {
        WBArc::ArchiveIndex a;
        CHECK(a.Build(zip.data(), (u32)zip.size(), bin.data(), (u32)bin.size(), (const u8*)d.data(), (u32)d.size(), err));
        std::vector<int> out;
        for (const auto& m : a.Members()) out.push_back(m.VfsEntry);
        return out;
    };
    CHECK((vfsEntries("536870917, 3, 0\r\n") == std::vector<int>{-1, -1, -1}));    // 0x20000005 entries * 8 wraps to 40
    CHECK((vfsEntries("5, 1, 0\r\n") == std::vector<int>{1, -1, -1}));
}

// ARM, Thumb or data, and how sure: the classification every view shows (UI_REQUIREMENTS CD-1)
// containers built the way the DS tools lay them out, for the container reader's tests
static void Put16(std::vector<u8>& v, u32 x) { v.push_back((u8)x); v.push_back((u8)(x >> 8)); }
static void Put32(std::vector<u8>& v, u32 x) { Put16(v, x & 0xFFFF); Put16(v, x >> 16); }
static void Set32(std::vector<u8>& v, size_t at, u32 x) { for (int i = 0; i < 4; i++) v[at + i] = (u8)(x >> (i * 8)); }
static void Pad4(std::vector<u8>& v) { while (v.size() & 3) v.push_back(0); }

static std::vector<u8> Varied(u32 seed, u32 n, const char* magic = nullptr)
{
    std::vector<u8> v(n);
    for (u8& x : v) { seed = seed * 1103515245 + 12345; x = (u8)(seed >> 16); }
    if (magic) memcpy(v.data(), magic, 4);
    return v;
}

// LZ10 with literals only: a valid stream that UnpackBios accepts
static std::vector<u8> Lz10(const std::vector<u8>& d)
{
    std::vector<u8> o;
    Put32(o, 0x10 | ((u32)d.size() << 8));
    for (size_t i = 0; i < d.size(); i += 8)
    {
        o.push_back(0);
        for (size_t j = i; j < i + 8 && j < d.size(); j++) o.push_back(d[j]);
    }
    Pad4(o);
    return o;
}

// files in id order; a name "dir/x" puts x in that directory (one level, a directory's files together,
// after the root's). No names at all: a name table with the root only
static std::vector<u8> MakeNarc(const std::vector<std::pair<std::string, std::vector<u8>>>& files, bool names)
{
    std::vector<u8> fnt;
    if (!names)
    {
        Put32(fnt, 8); Put16(fnt, 0); Put16(fnt, 1);
        fnt.push_back(0);
    }
    else
    {
        std::vector<std::string> dirs;
        for (auto& f : files)
        {
            size_t s = f.first.find('/');
            if (s != std::string::npos && std::find(dirs.begin(), dirs.end(), f.first.substr(0, s)) == dirs.end()) dirs.push_back(f.first.substr(0, s));
        }
        std::vector<std::vector<u8>> sub(dirs.size() + 1);
        std::vector<u32> first(dirs.size() + 1, 0);
        u32 id = 0;
        for (size_t d = 0; d <= dirs.size(); d++)
        {
            first[d] = id;
            for (auto& f : files)
            {
                size_t s = f.first.find('/');
                bool here = d == 0 ? s == std::string::npos : (s != std::string::npos && f.first.substr(0, s) == dirs[d - 1]);
                if (!here) continue;
                std::string n = d == 0 ? f.first : f.first.substr(s + 1);
                sub[d].push_back((u8)n.size());
                sub[d].insert(sub[d].end(), n.begin(), n.end());
                id++;
            }
            if (d == 0)
                for (size_t k = 0; k < dirs.size(); k++)
                {
                    sub[0].push_back((u8)(0x80 | dirs[k].size()));
                    sub[0].insert(sub[0].end(), dirs[k].begin(), dirs[k].end());
                    Put16(sub[0], 0xF001 + (u32)k);
                }
            sub[d].push_back(0);
        }
        u32 at = (u32)(dirs.size() + 1) * 8;
        for (size_t d = 0; d <= dirs.size(); d++)
        {
            Put32(fnt, at); Put16(fnt, first[d]); Put16(fnt, d == 0 ? (u32)dirs.size() + 1 : 0xF000);
            at += (u32)sub[d].size();
        }
        for (auto& s : sub) fnt.insert(fnt.end(), s.begin(), s.end());
    }
    Pad4(fnt);

    std::vector<u8> data, btaf;
    for (auto& f : files)
    {
        Put32(btaf, (u32)data.size());
        data.insert(data.end(), f.second.begin(), f.second.end());
        Put32(btaf, (u32)data.size());
        Pad4(data);
    }
    std::vector<u8> n = {'N', 'A', 'R', 'C', 0xFE, 0xFF, 0x00, 0x01};
    Put32(n, 0); Put16(n, 16); Put16(n, 3);
    n.insert(n.end(), {'B', 'T', 'A', 'F'}); Put32(n, 12 + (u32)btaf.size()); Put16(n, (u32)files.size()); Put16(n, 0);
    n.insert(n.end(), btaf.begin(), btaf.end());
    n.insert(n.end(), {'B', 'T', 'N', 'F'}); Put32(n, 8 + (u32)fnt.size());
    n.insert(n.end(), fnt.begin(), fnt.end());
    n.insert(n.end(), {'G', 'M', 'I', 'F'}); Put32(n, 8 + (u32)data.size());
    n.insert(n.end(), data.begin(), data.end());
    Set32(n, 8, (u32)n.size());
    return n;
}

// sequences and wave archives; names as the SYMB block lists them ("" for an entry without a name), or no
// SYMB block at all
static std::vector<u8> MakeSdat(const std::vector<std::vector<u8>>& seqs, const std::vector<std::vector<u8>>& waves,
                                const std::vector<std::string>* seqNames, const std::vector<std::string>* waveNames)
{
    u32 nfiles = (u32)(seqs.size() + waves.size());
    std::vector<u8> s(0x40, 0);
    memcpy(s.data(), "SDAT", 4);
    s[4] = 0xFF; s[5] = 0xFE; s[6] = 0x00; s[7] = 0x01;
    s[12] = 0x40; s[14] = seqNames ? 4 : 3;
    if (seqNames)
    {
        // SYMB: 8 record offsets, the records, then the names
        u32 symb = (u32)s.size();
        std::vector<u8> b = {'S', 'Y', 'M', 'B', 0, 0, 0, 0};
        for (int i = 0; i < 8; i++) Put32(b, 0);
        std::vector<u8> strs;
        auto names = [&](int type, const std::vector<std::string>& v)
        {
            Set32(b, 8 + type * 4, (u32)b.size());
            Put32(b, (u32)v.size());
            size_t at = b.size();
            for (size_t i = 0; i < v.size(); i++) Put32(b, 0);
            for (size_t i = 0; i < v.size(); i++)
            {
                if (v[i].empty()) continue;
                Set32(b, at + i * 4, 0x10000 + (u32)strs.size());    // fixed up below
                strs.insert(strs.end(), v[i].begin(), v[i].end());
                strs.push_back(0);
            }
        };
        names(0, *seqNames);
        names(3, *waveNames);
        u32 strBase = (u32)b.size();
        for (size_t i = 40; i + 4 <= b.size(); i += 4)
        {
            u32 v = (u32)b[i] | ((u32)b[i + 1] << 8) | ((u32)b[i + 2] << 16) | ((u32)b[i + 3] << 24);
            if (v >= 0x10000) Set32(b, i, strBase + v - 0x10000);
        }
        b.insert(b.end(), strs.begin(), strs.end());
        Pad4(b);
        Set32(b, 4, (u32)b.size());
        Set32(s, 0x10, symb); Set32(s, 0x14, (u32)b.size());
        s.insert(s.end(), b.begin(), b.end());
    }
    // INFO: sequence entries name files 0.., wave archives the files after them
    {
        u32 info = (u32)s.size();
        std::vector<u8> b = {'I', 'N', 'F', 'O', 0, 0, 0, 0};
        for (int i = 0; i < 8; i++) Put32(b, 0);
        for (int type = 0; type < 8; type++)
        {
            Set32(b, 8 + type * 4, (u32)b.size());
            const std::vector<std::vector<u8>>* list = type == 0 ? &seqs : type == 3 ? &waves : nullptr;
            u32 n = list ? (u32)list->size() : 0;
            size_t at = b.size();
            Put32(b, n);
            for (u32 i = 0; i < n; i++) Put32(b, 0);
            for (u32 i = 0; i < n; i++)
            {
                Set32(b, at + 4 + i * 4, (u32)b.size());
                Put16(b, type == 0 ? i : (u32)seqs.size() + i);
                Put16(b, 0); Put32(b, 0);
            }
        }
        Set32(b, 4, (u32)b.size());
        Set32(s, 0x18, info); Set32(s, 0x1C, (u32)b.size());
        s.insert(s.end(), b.begin(), b.end());
    }
    u32 fat = (u32)s.size();
    u32 fatSize = 12 + nfiles * 16;
    u32 file = fat + fatSize;
    std::vector<u8> f = {'F', 'I', 'L', 'E', 0, 0, 0, 0};
    Put32(f, nfiles); Put32(f, 0);
    std::vector<u8> b = {'F', 'A', 'T', ' '};
    Put32(b, fatSize); Put32(b, nfiles);
    auto add = [&](const std::vector<u8>& d)
    {
        Put32(b, file + (u32)f.size()); Put32(b, (u32)d.size()); Put32(b, 0); Put32(b, 0);
        f.insert(f.end(), d.begin(), d.end());
        Pad4(f);
    };
    for (auto& d : seqs) add(d);
    for (auto& d : waves) add(d);
    Set32(f, 4, (u32)f.size());
    Set32(s, 0x20, fat); Set32(s, 0x24, fatSize); Set32(s, 0x28, file); Set32(s, 0x2C, (u32)f.size());
    s.insert(s.end(), b.begin(), b.end());
    s.insert(s.end(), f.begin(), f.end());
    Set32(s, 8, (u32)s.size());
    return s;
}

static void TestContainers()
{
    using WBArc::ContainerIndex;
    using WBArc::ContainerMember;
    std::vector<u8> x = Varied(1, 300), y = Varied(2, 200, "RGCN"), model = Varied(3, 256, "BMD0");

    // a NARC with names: a stored file, one in a directory, and one packed on its own
    std::vector<u8> named = MakeNarc({{"x.bin", x}, {"sub/y.ncgr", y}, {"sub/m.nsbmd", Lz10(model)}}, true);
    std::vector<WBArc::ContainerEntry> e;
    std::string err;
    CHECK(WBArc::ReadNarc(named.data(), (u32)named.size(), e, err) && e.size() == 3);
    CHECK(e.size() == 3 && e[0].Name == "x.bin" && e[1].Name == "sub/y.ncgr" && e[2].Name == "sub/m.nsbmd" && e[0].Size == 300);

    // without names, holding a stored NARC that holds an LZ10-packed NARC: three levels
    std::vector<u8> inner = MakeNarc({{"", Varied(4, 400)}}, false);
    std::vector<u8> middle = MakeNarc({{"", y}, {"", Lz10(inner)}}, false);
    std::vector<u8> outer = MakeNarc({{"", x}, {"", middle}}, false);

    std::vector<u8> seq1 = Varied(5, 180, "SSEQ"), seq2 = Varied(6, 120, "SSEQ"), wave = Varied(7, 500, "SWAR");
    std::vector<std::string> sn = {"SEQ_TITLE", ""}, wn = {"WAVE_SE"};
    std::vector<u8> sdat = MakeSdat({seq1, seq2}, {wave}, &sn, &wn);
    std::vector<u8> bare = MakeSdat({seq1}, {}, nullptr, nullptr);
    CHECK(WBArc::ReadSdat(sdat.data(), (u32)sdat.size(), e, err) && e.size() == 3);
    CHECK(e.size() == 3 && e[0].Name == "SEQ_TITLE.sseq" && e[1].Name == "0001.sseq" && e[2].Name == "WAVE_SE.swar" && e[2].Size == 500);
    CHECK(WBArc::ReadSdat(bare.data(), (u32)bare.size(), e, err) && e.size() == 1 && e[0].Name == "0000.sseq");

    ContainerIndex idx;
    idx.Build({{5, "/data/a.narc", 0x10000, named}, {6, "/data/nest.narc", 0x20000, outer},
               {7, "/data/pack.carc", 0x30000, Lz10(named)}, {8, "/sound/sound.sdat", 0x40000, sdat},
               {9, "/sound/bare.sdat", 0x50000, bare}, {10, "/plain.bin", 0x60000, x}});
    const auto& cs = idx.Containers();
    CHECK(cs.size() == 5);
    CHECK(idx.Summary.find("3 NARC, 2 SDAT (1 packed whole): 15 files, 3 of them nested") == 0);

    const WBArc::Container* a = idx.ByFile(5);
    CHECK(a && a->Format == "narc" && a->Packed.empty() && a->Members.size() == 3);
    if (a && a->Members.size() == 3)
    {
        const ContainerMember& m = a->Members[2];
        CHECK(m.Form == "lz10" && m.Size == 256 && m.Kind == "BMD0" && m.Offset != ContainerMember::NoOffset);
        CHECK(!memcmp(idx.Content(m), model.data(), 256));
        CHECK(a->Members[1].Kind == "NCGR" && a->Members[0].Form == "stored" && a->Members[0].Size == 300);
        std::string part;
        u32 po = 0;
        CHECK(idx.MemberAt(*a, a->Members[0].Offset + 3, &part, &po) == 0 && part == "stored data" && po == 3);
        CHECK(idx.MemberAt(*a, 0, &part) == -1);       // the header
        CHECK(idx.Describe(5, a->Members[2].Offset) == "sub/m.nsbmd, packed data +0x0");
    }

    const WBArc::Container* n = idx.ByFile(6);
    CHECK(n && n->Members.size() == 5 && n->Nested == 3);
    if (n && n->Members.size() == 5)
    {
        std::vector<std::string> names;
        for (const ContainerMember& m : n->Members) names.push_back(m.Name);
        CHECK(names == std::vector<std::string>({"0000.bin", "0001.narc", "0001.narc/0000.ncgr", "0001.narc/0001.narc",
                                                 "0001.narc/0001.narc/0000.bin"}));
        const ContainerMember& ncgr = n->Members[2];
        const ContainerMember& packedNarc = n->Members[3];
        // the nested NARC's files start after its header (16), BTAF (12 + 2 * 8), BTNF (8 + 12) and GMIF's header (8)
        CHECK(ncgr.Depth == 1 && ncgr.Parent == 1 && ncgr.Offset == n->Members[1].Offset + 72);
        CHECK(!memcmp(idx.Content(ncgr), y.data(), y.size()));
        CHECK(packedNarc.Form == "lz10" && packedNarc.Kind == "NARC");
        // inside a packed container: no ROM offset, found by content only
        const ContainerMember& deepest = n->Members[4];
        CHECK(deepest.Depth == 2 && deepest.Parent == 3 && deepest.Offset == ContainerMember::NoOffset && deepest.Size == 400);
        // a card read inside the nested NARC's file names the nested file, not the NARC holding it
        CHECK(idx.MemberAt(*n, ncgr.Offset + 10) == 2);
        CHECK(idx.MemberAt(*n, n->Members[1].Offset + 2) == 1);   // the nested NARC's own header
    }

    const WBArc::Container* p = idx.ByFile(7);
    CHECK(p && p->Packed == "lz10" && p->Members.size() == 3 && p->Members[0].Offset == ContainerMember::NoOffset);
    if (p) CHECK(idx.MemberAt(*p, 40) == -1);

    const WBArc::Container* s = idx.ByFile(8);
    CHECK(s && s->Format == "sdat" && s->Members.size() == 3 && s->Members[0].Name == "SEQ_TITLE.sseq" && s->Members[0].Kind == "SSEQ");
    CHECK(!idx.ByFile(10));     // not a container

    // names, and content found in RAM
    int ci = -1, mi = -1;
    CHECK(idx.Find("DATA\\A.NARC:SUB/Y.NCGR", ci, mi) && idx.FullName(ci, mi) == "/data/a.narc:sub/y.ncgr");
    CHECK(!idx.Find("/data/a.narc:nothing", ci, mi));
    std::vector<u8> ram(0x4000, 0);
    memcpy(ram.data() + 0x1000, model.data(), model.size());
    memcpy(ram.data() + 0x2000, wave.data(), wave.size());
    std::vector<WBArc::Match> r = idx.Resolve(ram.data(), 0x02000000, (u32)ram.size(), 0x02000000, (u32)ram.size());
    CHECK(r.size() == 2);
    if (r.size() == 2)
    {
        auto [c0, m0] = idx.FromGlobal(r[0].Member);
        auto [c1, m1] = idx.FromGlobal(r[1].Member);
        // the packed member of a.narc and of the packed copy of it have the same bytes
        CHECK(r[0].Addr == 0x02001000 && r[0].Size == 256 && idx.FullName(c0, m0).find("m.nsbmd") != std::string::npos && !r[0].Alternatives.empty());
        CHECK(r[1].Addr == 0x02002000 && idx.FullName(c1, m1) == "/sound/sound.sdat:WAVE_SE.swar");
    }
    if (a)
    {
        std::vector<WBArc::Match> loc = idx.Locate(idx.IndexOf(5), 2, ram.data(), 0x02000000, (u32)ram.size());
        CHECK(loc.size() == 1 && loc[0].Addr == 0x02001000 && loc[0].Size == 256 && loc[0].Member == (int)a->Members[2].Global);
    }

    // a nested container is found in RAM through its files, so they are named
    if (n && n->Members.size() == 5)
    {
        std::vector<u8> ram2(0x1000, 0);
        memcpy(ram2.data() + 0x100, inner.data(), inner.size());
        std::vector<WBArc::Match> r2 = idx.Resolve(ram2.data(), 0x02000000, (u32)ram2.size(), 0x02000000, (u32)ram2.size());
        CHECK(r2.size() == 1 && r2[0].Size == 400 && idx.FullName(idx.FromGlobal(r2[0].Member).first, idx.FromGlobal(r2[0].Member).second)
              == "/data/nest.narc:0001.narc/0001.narc/0000.bin");
        CHECK(n->Members[3].Expanded && !n->Members[2].Expanded);
        CHECK(idx.Inside(*n, 1, 4) && idx.Inside(*n, 3, 4) && !idx.Inside(*n, 0, 4) && !idx.Inside(*n, 4, 3));
    }

    // a depth cap stops the nesting
    ContainerIndex shallow;
    shallow.MaxDepth = 1;
    shallow.Build({{6, "/data/nest.narc", 0x20000, outer}});
    CHECK(shallow.Containers().size() == 1 && shallow.Containers()[0].Members.size() == 4);

    // malformed input is refused, never read past
    std::vector<u8> bad = named;
    Set32(bad, 16 + 12 + 4, 0x7FFFFFFF);        // the first file's end, past GMIF
    CHECK(!WBArc::ReadNarc(bad.data(), (u32)bad.size(), e, err));
    bad = sdat;
    Set32(bad, (bad[0x20] | (bad[0x21] << 8)) + 8, 0x40000000);    // FAT count
    CHECK(!WBArc::ReadSdat(bad.data(), (u32)bad.size(), e, err));
    CHECK(!WBArc::ReadNarc(named.data(), 40, e, err));
    ContainerIndex broken;
    broken.Build({{5, "/bad.narc", 0, std::vector<u8>(named.begin(), named.begin() + 40)}});
    CHECK(broken.Containers().empty() && broken.Summary.find("1 unreadable") != std::string::npos);
    // random damage anywhere: whatever is read must stay inside the input
    u32 seed = 99;
    for (int i = 0; i < 3000; i++)
    {
        const std::vector<u8>& src = (i % 3 == 0) ? named : (i % 3 == 1) ? outer : sdat;
        std::vector<u8> d = src;
        for (int k = 0; k < 4; k++) { seed = seed * 1103515245 + 12345; d[(seed >> 8) % d.size()] = (u8)(seed >> 3); }
        if (i % 7 == 0) d.resize(d.size() / 2);
        ContainerIndex fuzz;
        fuzz.Build({{1, "/f.narc", 0, d}});
        for (const WBArc::Container& c : fuzz.Containers())
            for (const ContainerMember& m : c.Members)
                if (m.Offset != ContainerMember::NoOffset && (u64)m.Offset + m.StoredSize > d.size()) { CHECK(!"member outside the file"); break; }
    }
}

static void TestClassify()
{
    using namespace WBQ;
    WBQ::KbHints kb;
    kb.Functions.push_back({0x02000100, 0x02000200, CodeKind::Thumb, "Thumb_Func"});
    kb.Functions.push_back({0x02000300, 0x02000400, CodeKind::Arm, "Arm_Func"});
    kb.Data.push_back({0x02000500, 0x02000540, CodeKind::Data, "gTable"});
    auto rec = [](u32 exec, u32 read, u16 flags)
    {
        ByteRecord r {};
        r.exec_count = exec;
        r.read_count = read;
        r.flags = flags;
        return r;
    };
    ByteRecord thumb = rec(12, 0, ByteFlag::Exec9 | ByteFlag::Thumb | ByteFlag::InstrStart);
    ByteRecord arm = rec(3, 0, ByteFlag::Exec9 | ByteFlag::Arm | ByteFlag::InstrStart);
    ByteRecord both = rec(5, 0, ByteFlag::Exec9 | ByteFlag::Arm | ByteFlag::Thumb);
    ByteRecord pool = rec(0, 7, ByteFlag::Read9 | ByteFlag::LiteralPool);
    ByteRecord read = rec(0, 2, ByteFlag::Read9);

    Classification c = Classify(&thumb, kb, 0x02000110);
    CHECK(c.Kind == CodeKind::Thumb && c.Confirmed() && c.Why.contains("12 times as Thumb"));
    c = Classify(&arm, kb, 0x02000110);      // recorded beats the function's mode
    CHECK(c.Kind == CodeKind::Arm && c.Confirmed());
    c = Classify(&both, kb, 0x02000010);
    CHECK(c.Kind == CodeKind::Mixed && c.Confirmed());
    c = Classify(&pool, kb, 0x02000120);     // a literal pool inside a function is data
    CHECK(c.Kind == CodeKind::Data && c.Confirmed() && c.Why.contains("literal pool"));
    c = Classify(&read, kb, 0x02000010);     // read, never executed, outside functions
    CHECK(c.Kind == CodeKind::Data && c.Confirmed());
    c = Classify(&read, kb, 0x02000310);     // ...inside a function it stays code (a loader may copy code)
    CHECK(c.Kind == CodeKind::Arm && c.From == Evidence::Function && c.Why.contains("read 2 times"));
    c = Classify(nullptr, kb, 0x02000150);
    CHECK(c.Kind == CodeKind::Thumb && c.From == Evidence::Function && EvidenceName(c.From) == "inferred" && c.Why.contains("Thumb_Func"));
    c = Classify(nullptr, kb, 0x02000510);
    CHECK(c.Kind == CodeKind::Data && c.From == Evidence::Defined && c.Why.contains("gTable"));
    c = Classify(nullptr, kb, 0x02000800);
    CHECK(c.Kind == CodeKind::Unknown && c.From == Evidence::None && BadgeLetter(c.Kind) == '?');

    // a guess fills only what nothing else has classified
    AnalysisHints az;
    az.Ranges.push_back({0x02000150, 0x02000160, CodeKind::Arm, "ARM, from analysis: a branch from 02000100"});
    az.Ranges.push_back({0x02000510, 0x02000514, CodeKind::Thumb, "Thumb, from analysis: a call from 02000400"});
    az.Ranges.push_back({0x02000800, 0x02000810, CodeKind::Thumb, "Thumb, from analysis: a branch from 02000700"});
    c = Classify(nullptr, kb, 0x02000150, &az);
    CHECK(c.From == Evidence::Function);
    c = Classify(nullptr, kb, 0x02000510, &az);
    CHECK(c.From == Evidence::Defined);
    c = Classify(nullptr, kb, 0x02000800, &az);
    CHECK(c.Kind == CodeKind::Thumb && c.From == Evidence::Analysis && EvidenceName(c.From) == "from analysis" && c.Why.contains("02000700"));
    c = Classify(nullptr, kb, 0x02000804, &az);
    CHECK(c.From == Evidence::Analysis && c.Kind == CodeKind::Thumb);
    c = Classify(&read, kb, 0x02000800, &az);
    CHECK(c.From == Evidence::Recorded);

    // an override decides how it decodes, and says so when the recording disagrees
    kb.Overrides.push_back({0x02000100, 0x02000108, CodeKind::Data, "tester"});
    c = Classify(&thumb, kb, 0x02000104);
    CHECK(c.Kind == CodeKind::Data && c.From == Evidence::Override && c.Conflict && c.Why.contains("tester") && c.Why.contains("as Thumb"));
    c = Classify(nullptr, kb, 0x02000104);
    CHECK(c.Kind == CodeKind::Data && !c.Conflict);
    c = Classify(&thumb, kb, 0x02000108);    // just past it
    CHECK(c.Kind == CodeKind::Thumb && c.Confirmed());

    // overrides in the knowledge store: set, trim, undo as one, and the hints the views use
    QTemporaryDir dir;
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());
    WBKB::KnowledgeStore store;
    QString err;
    CHECK(store.Open(dir.filePath("kb.db"), r, "tester", err));
    CHECK(store.SchemaVersion() >= 5);
    CHECK(store.SetDecodeOverride(0, 0x02000400, 0x02000500, "thumb", -1, err));
    CHECK(store.SetDecodeOverride(0, 0x02000440, 0x02000460, "data", -1, err));
    CHECK(store.DecodeOverrides().size() == 3);
    const WBKB::DecodeOverride* o = store.DecodeOverrideAt(0, 0x02000450);
    CHECK(o && o->Mode == "data" && o->Author == "tester");
    o = store.DecodeOverrideAt(0, 0x02000470);
    CHECK(o && o->Mode == "thumb" && o->Start == 0x02000460 && o->End == 0x02000500);
    CHECK(!store.DecodeOverrideAt(1, 0x02000450));               // the other CPU
    CHECK(!store.SetDecodeOverride(0, 0x02000400, 0x02000400, "arm", -1, err));
    CHECK(!store.SetDecodeOverride(0, 0x02000400, 0x02000410, "code", -1, err));
    CHECK(store.Undo(err) && store.DecodeOverrides().size() == 1 && store.DecodeOverrideAt(0, 0x02000450)->Mode == "thumb");
    CHECK(store.Redo(err) && store.DecodeOverrides().size() == 3);
    CHECK(store.SetDecodeOverride(0, 0x02000000, 0x02001000, "clear", -1, err) && store.DecodeOverrides().empty());
    CHECK(store.Undo(err) && store.DecodeOverrides().size() == 3);
    CHECK(store.UpsertFunction(0, 0x02000600, 0x20, "thumb", "Never_Run", -1, err));
    WBQ::KbHints h = WBQ::KbHints::Collect(store, 0, 0x02000400, 0x02000700, -1);
    CHECK(h.Overrides.size() == 3 && h.Functions.size() == 1 && h.Functions[0].Kind == CodeKind::Thumb);
    c = Classify(nullptr, h, 0x02000610);
    CHECK(c.Kind == CodeKind::Thumb && c.From == Evidence::Function);
    c = Classify(nullptr, h, 0x02000450);
    CHECK(c.Kind == CodeKind::Data && c.From == Evidence::Override);
    CHECK(WBQ::KbHints::Collect(store, 1, 0x02000400, 0x02000700, -1).Overrides.empty());
}

// backward LZ (compressed overlays): an 8-byte head, then 64 bytes "ABCD"... packed as four
// literals and four references, stored backwards, then the footer
static void TestBackwardLz()
{
    std::vector<u8> fwd = {0x0F, 'D', 'C', 'B', 'A', 0xF0, 0x01, 0xF0, 0x01, 0xF0, 0x01, 0x30, 0x01};
    std::vector<u8> file = {'H', 'E', 'A', 'D', '0', '1', '2', '3'};
    file.insert(file.end(), fwd.rbegin(), fwd.rend());
    u32 enc = (u32)fwd.size() + 8, grow = (8 + 64) - (8 + (u32)fwd.size() + 8);
    u32 foot[2] = {enc | (8u << 24), grow};
    file.insert(file.end(), (u8*)foot, (u8*)foot + 8);
    std::vector<u8> out;
    std::string want = "HEAD0123";
    for (int i = 0; i < 16; i++) want += "ABCD";
    CHECK(UnpackBackwardLz(file.data(), (u32)file.size(), out));
    CHECK_EQ_STR(std::string(out.begin(), out.end()), want);
    // a footer that claims more data than there is, and a reference before the end of the output
    std::vector<u8> bad = file;
    bad[bad.size() - 8] = 0xFF;
    CHECK(!UnpackBackwardLz(bad.data(), (u32)bad.size(), out));
    bad = file;
    bad[8 + fwd.size() - 7] = 0xFF;     // the first reference's displacement byte
    CHECK(!UnpackBackwardLz(bad.data(), (u32)bad.size(), out));

    // a real compressor's output (ndspy.codeCompression): a text table, no head
    static const u8 real[] = {
        0x18, 0x40, 0x30, 0x4e, 0xb0, 0x30, 0x2e, 0x28, 0x30, 0x30, 0x18, 0x50, 0x31, 0xf0, 0xb0, 0x33,
        0x2e, 0x31, 0x14, 0x37, 0x18, 0x50, 0x32, 0xf0, 0xb0, 0x36, 0x2e, 0x33, 0x34, 0x0a, 0x18, 0x50,
        0x33, 0x4e, 0x50, 0xba, 0x30, 0x39, 0x2e, 0x35, 0x31, 0x0d, 0x18, 0x50, 0x34, 0x4e, 0x50, 0x18,
        0x30, 0x32, 0x2e, 0x36, 0x38, 0x0d, 0x18, 0x50, 0x35, 0x4e, 0x50, 0x18, 0x30, 0x35, 0x2e, 0x38,
        0x35, 0x0d, 0x18, 0x50, 0x36, 0x4e, 0xa0, 0x35, 0x38, 0x2e, 0x30, 0x32, 0x05, 0x18, 0x50, 0x37,
        0x4e, 0x50, 0x18, 0x30, 0x31, 0x2e, 0x31, 0x39, 0x0d, 0x18, 0x50, 0x38, 0x4e, 0x50, 0x18, 0x30,
        0x34, 0x2e, 0x33, 0x36, 0x0d, 0x18, 0x40, 0x30, 0x39, 0x18, 0x40, 0x33, 0x18, 0x20, 0x36, 0x37,
        0x29, 0x2e, 0x35, 0x33, 0x18, 0x50, 0x30, 0x18, 0x40, 0x34, 0x18, 0x30, 0xa8, 0x31, 0x2e, 0x37,
        0x30, 0x0a, 0x74, 0x72, 0x61, 0x00, 0x63, 0x6b, 0x20, 0x31, 0x31, 0x3a, 0x20, 0x6c, 0x00, 0x61,
        0x70, 0x73, 0x20, 0x35, 0x20, 0x70, 0x61, 0x00, 0x72, 0x20, 0x34, 0x34, 0x2e, 0x38, 0x37, 0x0a,
        0x00, 0xff, 0xff, 0xff, 0xac, 0x00, 0x00, 0x0b, 0x98, 0x00, 0x00, 0x00,
    };
    std::string text;
    for (int i = 0; i < 12; i++)
    {
        char line[64];
        snprintf(line, sizeof(line), "track %02d: laps %d par %d.%02d\n", i, 3 + i % 3, 40 + i * 3 % 29, i * 17 % 100);
        text += line;
    }
    CHECK(UnpackBackwardLz(real, sizeof(real), out));
    CHECK_EQ_STR(std::string(out.begin(), out.end()), text);
}

// BIOS compression formats, and the index of plain ROM files and their unpacked forms
static void TestBiosUnpack()
{
    auto unpack = [](std::vector<u8> in, std::string& form) -> std::string
    {
        std::vector<u8> out;
        if (!WBArc::UnpackBios(in.data(), (u32)in.size(), out, form)) return "<fail>";
        return std::string(out.begin(), out.end());
    };
    std::string form;
    // LZ77: three literals, then 15 bytes from 3 back
    CHECK_EQ_STR(unpack({0x10, 18, 0, 0, 0x10, 'A', 'B', 'C', 0xC0, 0x02}, form), "ABCABCABCABCABCABC");
    CHECK_EQ_STR(form, "lz10");
    // the same behind an "LZ77" tag
    CHECK_EQ_STR(unpack({'L', 'Z', '7', '7', 0x10, 18, 0, 0, 0x10, 'A', 'B', 'C', 0xC0, 0x02}, form), "ABCABCABCABCABCABC");
    // type 0x11: a short reference (length 15) and a medium one (length 0x11 + 3)
    CHECK_EQ_STR(unpack({0x11, 18, 0, 0, 0x10, 'A', 'B', 'C', 0xE0, 0x02}, form), "ABCABCABCABCABCABC");
    CHECK_EQ_STR(form, "lz11");
    CHECK_EQ_STR(unpack({0x11, 23, 0, 0, 0x10, 'A', 'B', 'C', 0x00, 0x30, 0x02}, form), "ABCABCABCABCABCABCABCAB");
    // run length: 5 x 'x', then 15 literals
    CHECK_EQ_STR(unpack({0x30, 20, 0, 0, 0x82, 'x', 0x0E, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o'}, form),
                 "xxxxxabcdefghijklmno");
    CHECK_EQ_STR(form, "rle");
    // Huffman, 8-bit: root with two data children 'a' (0) and 'b' (1); bits 0101...
    CHECK_EQ_STR(unpack({0x28, 32, 0, 0, 1, 0xC0, 'a', 'b', 0x55, 0x55, 0x55, 0x55}, form), "abababababababababababababababab");
    CHECK_EQ_STR(form, "huffman");
    // Huffman, 4-bit: nibbles 1, 2, 1, 2...; the first nibble is the low one
    CHECK_EQ_STR(unpack({0x24, 16, 0, 0, 1, 0xC0, 1, 2, 0x55, 0x55, 0x55, 0x55}, form), std::string(16, '\x21'));
    // not compressed: a reference before the start, input left over, input too short
    CHECK_EQ_STR(unpack({0x10, 18, 0, 0, 0x10, 'A', 'B', 'C', 0xC0, 0x09}, form), "<fail>");
    std::vector<u8> junk = {0x10, 18, 0, 0, 0x10, 'A', 'B', 'C', 0xC0, 0x02};
    junk.resize(junk.size() + 64, 0x5A);
    CHECK_EQ_STR(unpack(junk, form), "<fail>");
    CHECK_EQ_STR(unpack({0x10, 0, 1, 0, 0x00, 'A', 'B'}, form), "<fail>");

    // the index holds packed files' unpacked forms only
    u32 seed = 777;
    auto rnd = [&]() { seed = seed * 1103515245 + 12345; return (u8)(seed >> 16); };
    std::vector<u8> plain(300);
    for (u8& x : plain) x = rnd();
    std::vector<u8> packed = {0x10, 34, 0, 0, 0x00, '0', '1', '2', '3', '4', '5', '6', '7', 0x00, '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', 0x80, 0xF0, 0x0F};
    std::vector<WBArc::RomFileIndex::Input> in;
    in.push_back({5, "/data/plain.bin", plain});
    in.push_back({6, "/data/packed.lz", packed});
    in.push_back({7, "/data/tiny.bin", {1, 2, 3}});
    WBArc::RomFileIndex idx;
    idx.Build(std::move(in));
    CHECK(!idx.Indexed(5) && idx.Indexed(6) && !idx.Indexed(7));
    CHECK(idx.Forms().size() == 1);
    std::vector<u8> ram(0x1000, 0);
    const u32 base = 0x02000000;
    memcpy(ram.data() + 0x100, plain.data(), plain.size());
    const char* unpacked = "0123456789abcdef0123456789abcdef01";
    memcpy(ram.data() + 0x800, unpacked, 34);
    CHECK(idx.Resolve(ram.data(), base, (u32)ram.size(), base + 0x100, 300).empty());
    auto m = idx.Resolve(ram.data(), base, (u32)ram.size(), base + 0x800, 34);
    CHECK(m.size() == 1 && m[0].Addr == base + 0x800 && m[0].Size == 34 && idx.Forms()[m[0].Member].FileId == 6 &&
          idx.Forms()[m[0].Member].Form == "lz10");
}

// a saved session's first-run function records reach the "all time" scope
static void TestSessionFirstRun()
{
    std::vector<u8> buf(sizeof(SessionHeader));
    SessionHeader h {};
    memcpy(h.magic, "DSWBSESS", 8);
    h.version = SchemaVersion;
    h.header_size = sizeof(SessionHeader);
    h.section_count = 1;
    memcpy(buf.data(), &h, sizeof(h));
    FirstRunEntry e[2] {};
    e[0].pc = 0x02033eba; e[0].func = 0x02033c48; e[0].cpu = 0;
    e[1].pc = 0x037f8000; e[1].func = 0x037f7f00; e[1].cpu = 1;
    SessionSectionHeader sh {};
    sh.id = (u32)SessionSection::FirstRun;
    sh.count = 2;
    sh.size = sizeof(e);
    buf.insert(buf.end(), (u8*)&sh, (u8*)&sh + sizeof(sh));
    buf.insert(buf.end(), (u8*)e, (u8*)e + sizeof(e));
    MergedSessions m;
    CHECK(MergeSession(buf.data(), buf.size(), m));
    CHECK(m.FirstFunc.size() == 2 && m.FirstFunc[0x02033eba] == 0x02033c48 && m.FirstFunc[(1ull << 32) | 0x037f8000] == 0x037f7f00);
    // a later session does not override an earlier one
    e[0].func = 0x02000000;
    memcpy(buf.data() + sizeof(SessionHeader) + sizeof(sh), e, sizeof(e));
    CHECK(MergeSession(buf.data(), buf.size(), m) && m.FirstFunc[0x02033eba] == 0x02033c48 && m.SessionCount == 2);
}

// HC-3: a history ring read back oldest first, as a .dswbh (slots + trailer) and from a session file
static void TestHistoryFile()
{
    // 5 slots, 8 entries appended: frames 3..7 kept, the oldest (3) at slot 3
    WriteHistoryEntry ring[5] {};
    for (u32 f = 0; f < 8; f++) ring[f % 5].frame = f;
    HistoryView v {ring, 5, 8, 5};
    std::vector<u32> frames;
    v.ForEach([&](const WriteHistoryEntry& e) { frames.push_back(e.frame); });
    CHECK(v.Wrapped() && v.OldestFrame() == 3 && frames == std::vector<u32>({3, 4, 5, 6, 7}));

    std::vector<u8> file((u8*)ring, (u8*)ring + sizeof(ring));
    HistoryFileTrailer t = MakeHistoryTrailer(v, 100, 900);
    file.insert(file.end(), (u8*)&t, (u8*)&t + sizeof(t));
    HistoryView back;
    HistoryFileTrailer bt;
    CHECK(ReadHistoryFile(file.data(), file.size(), back, &bt) && bt.FrameStart == 100 && bt.FrameEnd == 900);
    frames.clear();
    back.ForEach([&](const WriteHistoryEntry& e) { frames.push_back(e.frame); });
    CHECK(back.Wrapped() && frames == std::vector<u32>({3, 4, 5, 6, 7}));
    // a history that never wrapped is cut to its used slots
    HistoryView part {ring, 3, 3, 3};
    std::vector<u8> small((u8*)ring, (u8*)ring + 3 * sizeof(WriteHistoryEntry));
    t = MakeHistoryTrailer(part, 0, 10);
    small.insert(small.end(), (u8*)&t, (u8*)&t + sizeof(t));
    CHECK(ReadHistoryFile(small.data(), small.size(), back) && !back.Wrapped() && back.Used == 3 && back.OldestFrame() == 5);
    // not one: a session file, a cut-off file
    CHECK(!ReadHistoryFile(small.data(), small.size() - 1, back));
    small[small.size() - 48] = 'X';
    CHECK(!ReadHistoryFile(small.data(), small.size(), back));

    // a session file's own history section
    std::vector<u8> buf(sizeof(SessionHeader));
    SessionHeader h {};
    memcpy(h.magic, "DSWBSESS", 8);
    h.version = SchemaVersion;
    h.header_size = sizeof(SessionHeader);
    h.section_count = 1;
    memcpy(buf.data(), &h, sizeof(h));
    SessionSectionHeader sh {};
    sh.id = (u32)SessionSection::WriteHistory;
    sh.count = 2;
    sh.size = 2 * sizeof(WriteHistoryEntry);
    buf.insert(buf.end(), (u8*)&sh, (u8*)&sh + sizeof(sh));
    buf.insert(buf.end(), (u8*)ring, (u8*)ring + 2 * sizeof(WriteHistoryEntry));
    CHECK(SessionHistory(buf.data(), buf.size(), back) && back.Used == 2 && !back.Wrapped() && back.OldestFrame() == 5);
}

// Recursive disassembly: batches, call deferral, and the bytes the program already knows.
static void TestFlowDisasm()
{
    struct Mem : FlowImage
    {
        std::map<u32, u8> Bytes;
        std::map<u32, FlowInsn> Insns;
        std::vector<std::pair<u32, u32>> Data;
        std::unordered_set<u32> Noreturn;
        std::atomic<bool>* Cancel = nullptr;

        void Put(u32 addr, u32 word, u32 n = 4)
        {
            for (u32 i = 0; i < n; i++) Bytes[addr + i] = (u8)(word >> (8 * i));
        }
        bool Read(u32 addr, u32 size, u32& out) const override
        {
            out = 0;
            for (u32 i = 0; i < size; i++)
            {
                auto it = Bytes.find(addr + i);
                if (it == Bytes.end()) return false;
                out |= (u32)it->second << (8 * i);
            }
            return true;
        }
        bool Restricted(u32 addr) const override { return addr < 0x9000; }
        bool Initialized(u32) const override { return true; }
        bool NoReturn(u32 addr) const override { return Noreturn.count(addr) != 0; }
        bool InstructionAt(u32 addr, FlowInsn& out) const override
        {
            auto it = Insns.find(addr);
            if (it == Insns.end()) return false;
            out = it->second;
            return true;
        }
        bool Offcut(u32 addr) const override
        {
            auto it = Insns.upper_bound(addr);
            if (it == Insns.begin()) return false;
            --it;
            return addr > it->second.Addr && addr < it->second.Addr + it->second.Size;
        }
        bool DefinedData(u32 addr) const override
        {
            for (const auto& r : Data) if (addr >= r.first && addr < r.second) return true;
            return false;
        }
        void Commit(const std::vector<FlowInsn>& added) override
        {
            for (const FlowInsn& in : added) Insns[in.Addr] = in;
            if (Cancel) Cancel->store(true);
        }
    };

    auto armB = [](u32 at, u32 target, bool link, u32 cond) {
        u32 imm = ((u32)((s32)target - (s32)at - 8) >> 2) & 0xFFFFFF;
        return (cond << 28) | (link ? 0x0B000000u : 0x0A000000u) | imm;
    };
    auto armBlx = [](u32 at, u32 target) {
        s32 off = (s32)target - (s32)at - 8;
        return 0xFA000000u | ((off & 2) ? 0x01000000u : 0u) | ((u32)(off >> 2) & 0xFFFFFF);
    };
    const u32 nop = 0xE1A00000;       // mov r0, r0
    const u32 bxlr = 0xE12FFF1E;

    auto at = [](const FlowDisasmResult& r, u32 addr) -> const FlowInsn* {
        for (const FlowInsn& in : r.Added) if (in.Addr == addr) return &in;
        return nullptr;
    };
    auto index = [](const FlowDisasmResult& r, u32 addr) {
        for (int i = 0; i < (int)r.Added.size(); i++) if (r.Added[i].Addr == addr) return i;
        return -1;
    };

    // Encodings the walk depends on.
    {
        DisasmInsn in;
        Disassemble(0x1000, armB(0x1000, 0x1100, false, 0), false, in);
        CHECK(in.IsBranch && in.Conditional && !in.IsCall && in.HasTarget && in.Target == 0x1100 && !in.TargetThumb);
        Disassemble(0x1004, armB(0x1004, 0x1200, true, 0xE), false, in);
        CHECK(in.IsCall && !in.Conditional && in.Target == 0x1200);
        Disassemble(0x2000, armBlx(0x2000, 0x2100), false, in);
        CHECK(in.IsCall && in.TargetThumb && in.Target == 0x2100);
        Disassemble(0x2100, 0x2000, true, in);
        CHECK(in.Valid && in.Size == 2 && in.Thumb);
        Disassemble(0x2102, 0x4770, true, in);
        CHECK(in.IsReturn && !in.HasTarget);
    }

    // A conditional branch is taken before the instruction after a call. The call itself waits.
    {
        Mem m;
        m.Put(0x1000, armB(0x1000, 0x1100, false, 0));
        m.Put(0x1004, armB(0x1004, 0x1200, true, 0xE));
        m.Put(0x1008, nop);
        m.Put(0x100C, bxlr);
        m.Put(0x1100, nop);
        m.Put(0x1104, bxlr);
        m.Put(0x1200, nop);
        m.Put(0x1204, bxlr);
        FlowDisasmResult r = Disassemble(m, {0x1000, false, 0, FlowReach::Entry});
        CHECK(index(r, 0x1100) >= 0 && index(r, 0x1008) > index(r, 0x1100));
        CHECK(index(r, 0x1200) > index(r, 0x1008));
        CHECK(at(r, 0x1200) && at(r, 0x1200)->Reach == FlowReach::Call && at(r, 0x1200)->From == 0x1004);
        CHECK(at(r, 0x1008) && at(r, 0x1008)->Reach == FlowReach::AfterCall);
        CHECK(!r.Incomplete && r.NoteCount == 0);
    }

    // A block split by the batch limit resumes before a branch to a lower address.
    {
        Mem m;
        m.Put(0x5000, armB(0x5000, 0x4000, false, 0));
        for (u32 a = 0x5004; a <= 0x5010; a += 4) m.Put(a, nop);
        m.Put(0x5014, bxlr);
        m.Put(0x4000, nop);
        m.Put(0x4004, bxlr);
        FlowOptions opt;
        opt.MaxPerSet = 4;
        FlowDisasmResult r = Disassemble(m, {0x5000, false, 0, FlowReach::Entry}, opt);
        CHECK(index(r, 0x5010) >= 0 && index(r, 0x4000) > index(r, 0x5010));
        CHECK(at(r, 0x5010)->Reach == FlowReach::Continue && at(r, 0x5010)->From == 0x500C);
    }

    // An instruction the program already has is not guessed again. Falling onto a different one conflicts.
    {
        Mem m;
        m.Put(0x1000, armB(0x1000, 0x1100, false, 0xE));
        m.Put(0x1100, nop);
        m.Put(0x1104, bxlr);
        FlowInsn known;
        known.Addr = 0x1100;
        known.Size = 4;
        known.Raw = nop;
        m.Insns[0x1100] = known;
        FlowDisasmResult r = Disassemble(m, {0x1000, false, 0, FlowReach::Entry});
        CHECK(!at(r, 0x1100) && at(r, 0x1000) && r.NoteCount == 0);

        Mem m2;
        m2.Put(0x2000, nop);
        m2.Put(0x2004, 0xE3A00001);     // mov r0, #1, a different instruction than the one on record
        known.Addr = 0x2004;
        known.Raw = nop;
        m2.Insns[0x2004] = known;
        r = Disassemble(m2, {0x2000, false, 0, FlowReach::Entry});
        CHECK(at(r, 0x2000) && !at(r, 0x2004));
        CHECK(r.NoteCount >= 1 && r.Notes[0].Type == FlowNote::Prototype);
    }

    // Defined data and an offcut stop the walk. An unaligned entry is not decoded.
    {
        Mem m;
        m.Put(0x1000, armB(0x1000, 0x1100, false, 0xE));
        m.Data.push_back({0x1100, 0x1104});
        FlowDisasmResult r = Disassemble(m, {0x1000, false, 0, FlowReach::Entry});
        CHECK(!at(r, 0x1100) && r.Notes[0].Type == FlowNote::Data);

        Mem m2;
        FlowInsn wide;
        wide.Addr = 0x4000;
        wide.Size = 4;
        wide.Raw = nop;
        m2.Insns[0x4000] = wide;
        r = Disassemble(m2, {0x4002, false, 0, FlowReach::Entry});
        CHECK(r.Added.empty() && r.Notes[0].Type == FlowNote::Offcut);

        r = Disassemble(m2, {0x1002, false, 0, FlowReach::Entry});
        CHECK(r.Added.empty() && r.Notes[0].Type == FlowNote::Alignment);
    }

    // A no-return call has no callee and no fall-through. An indirect branch adds no target.
    {
        Mem m;
        m.Put(0x1000, armB(0x1000, 0x3000, true, 0xE));
        m.Put(0x1004, nop);
        m.Put(0x3000, nop);
        m.Noreturn.insert(0x3000);
        FlowDisasmResult r = Disassemble(m, {0x1000, false, 0, FlowReach::Entry});
        CHECK(r.Added.size() == 1 && r.Added[0].Addr == 0x1000);

        Mem m2;
        m2.Put(0x1000, bxlr);
        r = Disassemble(m2, {0x1000, false, 0, FlowReach::Entry});
        CHECK(r.Added.size() == 1 && r.NoteCount == 0);
    }

    // BLX switches the following block to Thumb. A branch to itself is one instruction.
    {
        Mem m;
        m.Put(0x2000, armBlx(0x2000, 0x2100));
        m.Put(0x2004, bxlr);
        m.Put(0x2100, 0x2000, 2);
        m.Put(0x2102, 0x4770, 2);
        FlowDisasmResult r = Disassemble(m, {0x2000, false, 0, FlowReach::Entry});
        const FlowInsn* t = at(r, 0x2100);
        CHECK(t && t->Thumb && t->Size == 2 && at(r, 0x2102) && at(r, 0x2102)->Thumb);

        Mem m2;
        m2.Put(0x1000, armB(0x1000, 0x1000, false, 0xE));
        r = Disassemble(m2, {0x1000, false, 0, FlowReach::Entry});
        CHECK(r.Added.size() == 1 && !r.Incomplete);
    }

    // The same encoding stops the block once it has repeated past the limit.
    {
        Mem m;
        for (int i = 0; i < 24; i++) m.Put(0x8000 + i * 4, nop);
        FlowDisasmResult r = Disassemble(m, {0x8000, false, 0, FlowReach::Entry});
        CHECK(r.Added.size() == 17);
        CHECK(r.Notes[0].Type == FlowNote::Repeat && r.Notes[0].Addr == 0x8000 + 16 * 4);
    }

    // A literal load remembers the pool word. A second seed covered by the first adds nothing.
    {
        Mem m;
        m.Put(0x6000, 0xE59F0000);     // ldr r0, [pc, #0]
        m.Put(0x6004, bxlr);
        m.Put(0x6008, 0x11111111);
        FlowDisasmResult r = Disassemble(m, {0x6000, false, 0, FlowReach::Entry});
        CHECK(at(r, 0x6000) && at(r, 0x6000)->Literal && at(r, 0x6000)->LiteralAddr == 0x6008);

        Mem m2;
        m2.Put(0x1000, armB(0x1000, 0x1100, true, 0xE));
        m2.Put(0x1004, bxlr);
        m2.Put(0x1100, bxlr);
        std::vector<FlowSeed> seeds = {{0x1000, false, 0, FlowReach::Entry}, {0x1100, false, 0x1000, FlowReach::Call}};
        r = DisassembleSet(m2, seeds);
        CHECK(at(r, 0x1100) && index(r, 0x1100) == index(r, 0x1100));
        int n = 0;
        for (const FlowInsn& in : r.Added) if (in.Addr == 0x1100) n++;
        CHECK(n == 1);
    }

    // Outside the restricted set is not guessed. Cancelling stops the next batch.
    {
        Mem m;
        m.Put(0x1000, armB(0x1000, 0xA000, false, 0xE));
        m.Put(0xA000, nop);
        FlowDisasmResult r = Disassemble(m, {0x1000, false, 0, FlowReach::Entry});
        CHECK(r.Added.size() == 1 && !at(r, 0xA000));

        Mem m2;
        std::atomic<bool> stop{false};
        m2.Cancel = &stop;
        m2.Put(0x1000, armB(0x1000, 0x2000, true, 0xE));
        m2.Put(0x1004, nop);
        m2.Put(0x1008, bxlr);
        m2.Put(0x2000, nop);
        m2.Put(0x2004, bxlr);
        r = Disassemble(m2, {0x1000, false, 0, FlowReach::Entry}, {}, &stop);
        CHECK(at(r, 0x1004) && !at(r, 0x2000) && r.Incomplete);
    }
}

// A PC-relative load is a literal-pool slot. LDRD's load bit is clear (the doubleword
// space encodes the direction in the opcode), and the common LDR [pc, #+imm] has the up bit set.
static void TestLiteralLoads()
{
    DisasmInsn in;
    Disassemble(0x02000004, 0xE59F1004, false, in);   // ldr r1, [pc, #4]
    CHECK(in.LiteralLoad && in.Ref == 0x02000010);
    Disassemble(0x02000000, 0xE51F0004, false, in);   // ldr r0, [pc, #-4]
    CHECK(in.LiteralLoad && in.Ref == 0x02000004);
    Disassemble(0x02000000, 0xE1CF20D8, false, in);   // ldrd r2, [pc, #8]
    CHECK_EQ_STR(in.Text(), "ldrd r2, r3, [pc, #8]");
    CHECK(in.LiteralLoad && in.HasRef && in.Ref == 0x02000010);
    Disassemble(0x02000000, 0xE1CF20F8, false, in);   // strd r2, [pc, #8]
    CHECK_EQ_STR(in.Text(), "strd r2, r3, [pc, #8]");
    CHECK(!in.LiteralLoad);
    Disassemble(0x02000000, 0xE50F0004, false, in);   // str r0, [pc, #-4]
    CHECK(!in.LiteralLoad);
    Disassemble(0x02000002, 0x4801, true, in);        // ldr r0, [pc, #4]
    CHECK(in.LiteralLoad);
    Disassemble(0x02000000, 0xE1DF10B4, false, in);   // ldrh r1, [pc, #4]
    CHECK_EQ_STR(in.Text(), "ldrh r1, [pc, #4]");
    CHECK(in.LiteralLoad && in.Ref == 0x0200000C);
    // the recorder's fast test must agree with the disassembler, including LDRD (its load bit is clear)
    CHECK(LoadsLiteralPool(0xE59F1004, false));
    CHECK(LoadsLiteralPool(0xE51F0004, false));
    CHECK(LoadsLiteralPool(0xE1CF20D8, false));
    CHECK(LoadsLiteralPool(0xE1DF10B4, false));
    CHECK(!LoadsLiteralPool(0xE50F0004, false));
    CHECK(!LoadsLiteralPool(0xE1CF20F8, false));
    CHECK(!LoadsLiteralPool(0xE5DF0000, false));      // ldrb r0, [pc]
    CHECK(LoadsLiteralPool(0x4801, true));
    CHECK(!LoadsLiteralPool(0x6800, true));           // ldr r0, [r0]
}

// Plain-English and C forms for the widths the generic load path used to flatten.
static void TestInsnForms()
{
    DisasmInsn in;
    Disassemble(0x02000000, 0xE1C020D8, false, in);   // ldrd r2, [r0, #8]
    CHECK_EQ_STR(in.Text(), "ldrd r2, r3, [r0, #8]");
    std::string e = ExplainInsn(in);
    CHECK(e.find("doubleword") != std::string::npos && e.find("[r0, #8]") != std::string::npos);
    CHECK(e.find("r2") != std::string::npos && e.find("r3") != std::string::npos && e.find("from r3") == std::string::npos);
    std::string c = InsnAsC(in);
    CHECK(c.find("*(uint64_t*)([r0, #8])") != std::string::npos);
    CHECK(c.find("r2") != std::string::npos && c.find("r3") != std::string::npos);

    Disassemble(0x02000000, 0xE1C020F8, false, in);   // strd r2, [r0, #8]
    c = InsnAsC(in);
    CHECK(c.find("*(uint64_t*)([r0, #8])") != std::string::npos && c.find("r2") != std::string::npos && c.find("r3") != std::string::npos);

    Disassemble(0x02000000, 0xE1D100D0, false, in);   // ldrsb r0, [r1]
    CHECK_EQ_STR(in.Text(), "ldrsb r0, [r1]");
    CHECK_EQ_STR(InsnAsC(in), "r0 = *(int8_t*)([r1]);");
    Disassemble(0x02000000, 0xE1D100F0, false, in);   // ldrsh r0, [r1]
    CHECK_EQ_STR(InsnAsC(in), "r0 = *(int16_t*)([r1]);");
    Disassemble(0x02000000, 0xE5D10000, false, in);   // ldrb r0, [r1]
    CHECK_EQ_STR(InsnAsC(in), "r0 = *(uint8_t*)([r1]);");

    Disassemble(0x02000000, 0xE1500001, false, in);   // cmp r0, r1
    CHECK(ExplainInsn(in).find("No register is written") != std::string::npos);
    Disassemble(0x02000000, 0xE0910002, false, in);   // adds r0, r1, r2
    CHECK(ExplainInsn(in).find("The flags are updated") != std::string::npos);
    Disassemble(0x02000000, 0xE0213291, false, in);   // mla r1, r1, r2, r3
    CHECK_EQ_STR(in.Text(), "mla r1, r1, r2, r3");
    CHECK(ExplainInsn(in).find("adds r3") != std::string::npos && ExplainInsn(in).find("low 32 bits") != std::string::npos);
    Disassemble(0x02000000, 0xE0832594, false, in);   // umull r2, r3, r4, r5
    CHECK_EQ_STR(in.Text(), "umull r2, r3, r4, r5");
    CHECK(ExplainInsn(in).find("64-bit unsigned") != std::string::npos);
    Disassemble(0x02000000, 0xE0C32594, false, in);   // smull r2, r3, r4, r5
    CHECK(ExplainInsn(in).find("64-bit signed") != std::string::npos);
    Disassemble(0x02000000, 0xE10F0000, false, in);   // mrs r0, cpsr
    CHECK(ExplainInsn(in).find("status register") != std::string::npos);
    Disassemble(0x02000000, 0xE16F0F11, false, in);   // clz r0, r1
    CHECK(ExplainInsn(in).find("leading zero") != std::string::npos);
    Disassemble(0x02000000, 0xEF110000, false, in);
    CHECK(ExplainInsn(in).find("Software interrupt") != std::string::npos);
    Disassemble(0x02000000, 0xEB000000, false, in);
    CHECK(ExplainInsn(in).find("saving the return address") != std::string::npos);
    struct CondPhraseCase { u32 Instr; const char* Phrase; };
    const CondPhraseCase conds[] = {
        {0x2A000000, "carry set (unsigned higher or same)."},
        {0x3A000000, "carry clear (unsigned lower)."},
        {0x4A000000, "negative (N is set)."},
        {0x5A000000, "positive or zero (N is clear)."},
        {0x6A000000, "overflow set."},
        {0x7A000000, "overflow clear."},
        {0x8A000000, "unsigned higher."},
        {0x9A000000, "unsigned lower or same."},
        {0xAA000000, "signed greater than or equal."},
        {0xBA000000, "signed less than."},
        {0xCA000000, "signed greater than."},
        {0xDA000000, "signed less than or equal."},
    };
    for (const CondPhraseCase& c : conds)
    {
        Disassemble(0x02000000, c.Instr, false, in);
        CHECK(ExplainInsn(in).find(std::string("condition is ") + c.Phrase) != std::string::npos);
    }
}

// DataObjectAt used to stop 2MB before the address, so a larger object was invisible past that.
static void TestDataObjectReach()
{
    QTemporaryDir dir;
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());
    WBKB::KnowledgeStore kb;
    QString err;
    CHECK(kb.Open(dir.filePath("kb.db"), r, "tester", err));
    CHECK(kb.DefineData(0, 0x02000000, 0x300000, "u8", "big", -1, err));
    CHECK(kb.DefineData(0, 0x02100000, 4, "u32", "near", -1, err));
    const WBKB::DataObject* big = kb.DataObjectAt(0, 0x02210000);
    CHECK(big && big->Name == "big");
    CHECK(kb.DataObjectAt(0, 0x022FFFFF) && kb.DataObjectAt(0, 0x022FFFFF)->Name == "big");
    CHECK(!kb.DataObjectAt(0, 0x02300000));
    // the nearer object wins where they overlap, and does not hide the rest of the large one
    CHECK(kb.DataObjectAt(0, 0x02100000) && kb.DataObjectAt(0, 0x02100000)->Name == "near");
    CHECK(kb.NameAt(0, 0x02210000, -1, true).startsWith("big"));
}

// DataUnits sizes a basic type the store does not have a definition for. u64 and fx64 were words.
static void TestDataUnitWidths()
{
    QTemporaryDir dir;
    std::vector<u8> rom = MakeRom();
    NitroROM r;
    r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());
    WBKB::KnowledgeStore kb;
    QString err;
    CHECK(kb.Open(dir.filePath("kb.db"), r, "tester", err));
    CHECK(kb.DefineData(0, 0x02000680, 16, "u64", "wide", -1, err));
    CHECK(kb.DefineData(0, 0x020006A0, 8, "fx64", "fixed", -1, err));
    CHECK(kb.DefineData(0, 0x020006B0, 8, "s64", "signed", -1, err));
    std::vector<WBQ::DataUnit> u = WBQ::DataUnits(kb, 0, 0x02000680, 0x020006C0, -1);
    int n64 = 0, nfx = 0, ns = 0;
    for (const WBQ::DataUnit& d : u)
    {
        if (d.Addr >= 0x02000680 && d.Addr < 0x02000690) { n64++; CHECK(d.Size == 8); }
        if (d.Addr >= 0x020006A0 && d.Addr < 0x020006A8) { nfx++; CHECK(d.Size == 8); }
        if (d.Addr >= 0x020006B0 && d.Addr < 0x020006B8) { ns++; CHECK(d.Size == 8); }
    }
    CHECK(n64 == 2 && nfx == 1 && ns == 1);
}

static void TestDebugger()
{
    Workbench wb;
    std::string err;
    Breakpoint bp {};
    bp.Kind = BreakKind::Exec;
    bp.Addr = 0x02001000;
    bp.Size = 4;
    bp.Cpu = 0;
    // r0 is 0 with no machine attached, so this condition is false and the breakpoint does not hit
    bp.Condition = "r0 == 1";
    int id = wb.Dbg.AddBreakpoint(bp, err);
    CHECK(id > 0 && !wb.Dbg.CheckExec(0, 0x02001000) && wb.Dbg.Hits().empty());
    CHECK(wb.Dbg.SetBreakpointCondition(id, "", err));
    CHECK(wb.Dbg.CheckExec(0, 0x02001000) && wb.Dbg.Hits().size() == 1 && wb.Dbg.Hits()[0].Id == id);
    CHECK(!wb.Dbg.CheckExec(1, 0x02001000));           // the other CPU
    CHECK(!wb.Dbg.CheckExec(0, 0x02001004));           // the next instruction
    CHECK(wb.Dbg.SetBreakpointEnabled(id, false) && !wb.Dbg.CheckExec(0, 0x02001000));
    CHECK(wb.Dbg.SetBreakpointEnabled(id, true) && wb.Dbg.CheckExec(0, 0x02001000));

    // one breakpoint covering the last bytes of a page and the first of the next
    Breakpoint cross {};
    cross.Kind = BreakKind::Exec;
    cross.Addr = 0x00001FFC;
    cross.Size = 8;
    cross.Cpu = -1;
    CHECK(wb.Dbg.AddBreakpoint(cross, err) > 0);
    CHECK(wb.Dbg.CheckExec(1, 0x00001FFC) && wb.Dbg.CheckExec(0, 0x00002000));
    CHECK(!wb.Dbg.CheckExec(0, 0x00002004));

    // a condition on the hit count stops after the first success
    Breakpoint once {};
    once.Kind = BreakKind::Exec;
    once.Addr = 0x02002000;
    once.Size = 2;
    once.Cpu = 0;
    once.Condition = "hits == 0";
    int onceId = wb.Dbg.AddBreakpoint(once, err);
    CHECK(onceId > 0 && wb.Dbg.CheckExec(0, 0x02002000) && !wb.Dbg.CheckExec(0, 0x02002000));

    Breakpoint wr {};
    wr.Kind = BreakKind::Write;
    wr.Addr = 0x02003002;
    wr.Size = 2;
    wr.Cpu = 0;
    int wrId = wb.Dbg.AddBreakpoint(wr, err);
    wb.Dbg.CheckAccess(0, 0x02003000, 4, 0x11223344, false);   // a read that overlaps
    CHECK(wb.Dbg.Hits().back().Id != wrId);
    u32 hitsBefore = (u32)wb.Dbg.Hits().size();
    wb.Dbg.CheckAccess(0, 0x02003000, 4, 0x11223344, true);    // a write that overlaps the watched bytes
    CHECK(wb.Dbg.Hits().size() == hitsBefore + 1 && wb.Dbg.Hits().back().Id == wrId && wb.Dbg.Hits().back().Write);
    CHECK(wb.Dbg.CheckExec(0, 0x02004000));                    // the watch stops before the next instruction
    wb.Dbg.CheckAccess(1, 0x02003000, 4, 1, true);             // the other CPU
    CHECK(wb.Dbg.Hits().back().Id != wrId || wb.Dbg.Hits().back().Cpu == 0);

    Breakpoint bad {};
    bad.Kind = BreakKind::Exec;
    bad.Addr = 1;
    bad.Condition = "not_a_thing";
    CHECK(wb.Dbg.AddBreakpoint(bad, err) < 0 && err.find("not_a_thing") != std::string::npos);

    Breakpoint temp {};
    temp.Kind = BreakKind::Exec;
    temp.Addr = 0x02005000;
    temp.Size = 4;
    temp.Cpu = 0;
    temp.Temporary = true;
    int tempId = wb.Dbg.AddBreakpoint(temp, err);
    CHECK(tempId > 0 && wb.Dbg.CheckExec(0, 0x02005000) && !wb.Dbg.CheckExec(0, 0x02005000));
    CHECK(wb.Dbg.RemoveBreakpoint(tempId) == false);
    CHECK(wb.Dbg.RemoveBreakpoint(-1));
    CHECK(!wb.Dbg.CheckExec(0, 0x02001000) && !wb.Dbg.HasExecBreakpoints() && !wb.Dbg.HasWatchpoints());

    // step 3 instructions, then stop on the fourth check
    wb.Dbg.RequestStep(0, 3);
    CHECK(!wb.Dbg.CheckExec(0, 0x10) && !wb.Dbg.CheckExec(0, 0x14) && !wb.Dbg.CheckExec(0, 0x18));
    CHECK(wb.Dbg.CheckExec(0, 0x1C));
    wb.Dbg.CancelStep();
    CHECK(!wb.Dbg.CheckExec(0, 0x20));

    // a requested pause stops the CPU it names, and leaves the other one running
    wb.Dbg.RequestBreak(0, "user pause");
    CHECK(!wb.Dbg.CheckExec(1, 0x30));
    CHECK(wb.Dbg.CheckExec(0, 0x30) && !wb.Dbg.CheckExec(0, 0x34));
    wb.Dbg.RequestBreak(-1, nullptr);
    CHECK(wb.Dbg.CheckExec(1, 0x40));
    wb.Dbg.RequestBreak(0, "dropped");
    wb.Dbg.CancelStep();
    CHECK(!wb.Dbg.CheckExec(0, 0x44));

    Breakpoint rd {};
    rd.Kind = BreakKind::Read;
    rd.Addr = 0x02006000;
    rd.Size = 4;
    rd.Cpu = -1;
    int rdId = wb.Dbg.AddBreakpoint(rd, err);
    u32 readsBefore = (u32)wb.Dbg.Hits().size();
    wb.Dbg.CheckAccess(0, 0x02006000, 4, 1, true);         // a write is not a read watch
    CHECK(wb.Dbg.Hits().size() == readsBefore);
    wb.Dbg.CheckAccess(2, 0x02006000, 4, 0x99, false);     // a DMA read on CPU 0's bus
    CHECK(wb.Dbg.Hits().back().Id == rdId && !wb.Dbg.Hits().back().Write);
    CHECK(wb.Dbg.CheckExec(0, 0x50) && !wb.Dbg.CheckExec(1, 0x50));
    CHECK(!wb.Dbg.SetBreakpointEnabled(99999, false));
    CHECK(!wb.Dbg.SetBreakpointCondition(99999, "", err));
}

// Two saved sessions: counts add, a later write replaces the writer, a session that only reads does not,
// and code that is also read becomes code-and-data. A page for a region that does not exist is skipped.
static void TestSessionMerge()
{
    auto header = [](std::vector<u8>& buf, u32 start, u32 end, u32 sections)
    {
        buf.assign(sizeof(SessionHeader), 0);
        SessionHeader h {};
        memcpy(h.magic, "DSWBSESS", 8);
        h.version = SchemaVersion;
        h.header_size = sizeof(SessionHeader);
        h.section_count = sections;
        h.frame_start = start;
        h.frame_end = end;
        memcpy(buf.data(), &h, sizeof(h));
    };
    auto section = [](std::vector<u8>& buf, SessionSection id, u32 count, const void* data, u64 size)
    {
        SessionSectionHeader sh {};
        sh.id = (u32)id;
        sh.count = count;
        sh.size = size;
        buf.insert(buf.end(), (u8*)&sh, (u8*)&sh + sizeof(sh));
        if (size) buf.insert(buf.end(), (const u8*)data, (const u8*)data + size);
    };
    const u32 stride = sizeof(PageHeader) + sizeof(ByteRecord) * PageSize;
    std::vector<u8> page(stride, 0);
    PageHeader ph {};
    ph.region = (u16)Region::MainRAM;
    ph.page = 0;
    memcpy(page.data(), &ph, sizeof(ph));
    ByteRecord* rec = (ByteRecord*)(page.data() + sizeof(PageHeader));
    rec[0].exec_count = 3;
    rec[0].mem_class = (u8)MemClass::Code;
    rec[0].flags = 0x1;
    rec[4].write_count = 2;
    rec[4].last_writer_pc = 0x02001000;
    rec[4].last_write_frame = 10;
    rec[4].last_writer_cpu = 0;
    rec[4].mem_class = (u8)MemClass::Data;
    rec[8].read_count = 4;
    rec[8].mem_class = (u8)MemClass::Data;

    CallEdge edge {};
    edge.cpu = 0;
    edge.call_site = 0x02000100;
    edge.callee = 0x02000200;
    edge.count = 4;
    edge.first_frame = 1;
    edge.last_frame = 9;
    ProvenanceEvent prov {};
    prov.kind = (u8)ProvenanceKind::CardRead;
    prov.pc = 0x02000300;
    prov.frame = 12;

    std::vector<u8> a;
    header(a, 100, 140, 3);
    section(a, SessionSection::ByteRecords, 1, page.data(), page.size());
    section(a, SessionSection::CallEdges, 1, &edge, sizeof(edge));
    section(a, SessionSection::Provenance, 1, &prov, sizeof(prov));

    MergedSessions m;
    CHECK(MergeSession(a.data(), a.size(), m));
    CHECK(m.SessionCount == 1 && m.FramesRecorded == 40 && m.Records.PageCount() == 1);
    CHECK(m.Provenance.size() == 1 && m.Provenance[0].pc == 0x02000300);

    memset(rec, 0, sizeof(ByteRecord) * PageSize);
    rec[0].read_count = 5;
    rec[0].mem_class = (u8)MemClass::Data;
    rec[0].flags = 0x2;
    rec[4].read_count = 1;
    rec[4].mem_class = (u8)MemClass::Data;
    rec[4].last_writer_pc = 0xDEAD;            // no write in this session, so the earlier writer stays
    rec[8].exec_count = 1;
    rec[8].mem_class = (u8)MemClass::Code;
    edge.caller_func = 0x02000080;
    edge.count = 3;
    prov.pc = 0x02000400;
    std::vector<u8> b;
    header(b, 200, 100, 3);                    // end before start: no frames counted
    section(b, SessionSection::ByteRecords, 1, page.data(), page.size());
    section(b, SessionSection::CallEdges, 1, &edge, sizeof(edge));
    section(b, SessionSection::Provenance, 1, &prov, sizeof(prov));
    CHECK(MergeSession(b.data(), b.size(), m));

    const RecordPage* got = m.Records.Get((u16)Region::MainRAM, 0);
    CHECK(got && got->Rec[0].exec_count == 3 && got->Rec[0].read_count == 5);
    CHECK(got->Rec[0].mem_class == (u8)MemClass::CodeAndData && got->Rec[0].flags == 0x3);
    CHECK(got->Rec[4].write_count == 2 && got->Rec[4].read_count == 1 && got->Rec[4].last_writer_pc == 0x02001000);
    CHECK(got->Rec[8].exec_count == 1 && got->Rec[8].read_count == 4 && got->Rec[8].mem_class == (u8)MemClass::CodeAndData);
    CallEdgeKey key {edge.call_site, edge.callee};
    auto it = m.Edges[0].find(key);
    CHECK(it != m.Edges[0].end() && it->second.count == 7 && it->second.caller_func == 0x02000080);
    CHECK(m.Provenance.size() == 2 && m.SessionCount == 2 && m.FramesRecorded == 40);

    // a later write replaces the writer
    memset(rec, 0, sizeof(ByteRecord) * PageSize);
    rec[4].write_count = 1;
    rec[4].last_writer_pc = 0x0200ABCD;
    rec[4].last_write_frame = 50;
    rec[4].last_writer_cpu = 1;
    rec[4].mem_class = (u8)MemClass::Data;
    std::vector<u8> c;
    header(c, 0, 10, 1);
    section(c, SessionSection::ByteRecords, 1, page.data(), page.size());
    CHECK(MergeSession(c.data(), c.size(), m));
    got = m.Records.Get((u16)Region::MainRAM, 0);
    CHECK(got->Rec[4].write_count == 3 && got->Rec[4].last_writer_pc == 0x0200ABCD && got->Rec[4].last_writer_cpu == 1);
    CHECK(m.FramesRecorded == 50);

    ph.region = 7000;
    memcpy(page.data(), &ph, sizeof(ph));
    std::vector<u8> bad;
    header(bad, 0, 1, 1);
    section(bad, SessionSection::ByteRecords, 1, page.data(), page.size());
    CHECK(MergeSession(bad.data(), bad.size(), m) && m.Records.PageCount() == 1);

    std::vector<u8> cut;
    header(cut, 0, 1, 1);
    section(cut, SessionSection::ByteRecords, 1, page.data(), 4);   // claims a page, holds four bytes
    CHECK(MergeSession(cut.data(), cut.size(), m) && m.Records.PageCount() == 1);

    a[0] = 'X';
    CHECK(!MergeSession(a.data(), a.size(), m) && m.SessionCount == 5);
}

// Labels from two sources join into one, a memory byte keeps the label, and a constant drops out of a union.
static void TestOriginLabels()
{
    Workbench wb;
    wb.Taint.Enable(true);
    CHECK(!wb.Taint.RemoveSource(1));              // the built-in code-constant source
    DataSource keys;
    keys.Kind = SourceKind::Keypad;
    keys.Name = "keys";
    DataSource screen;
    screen.Kind = SourceKind::Touch;
    screen.Name = "screen";
    u32 idA = wb.Taint.AddSource(keys);
    u32 idB = wb.Taint.AddSource(screen);
    u32 la = wb.Taint.LabelForSource(idA);
    u32 lb = wb.Taint.LabelForSource(idB);
    CHECK(la && lb && la != lb && wb.Taint.LabelForSource(idA) == la);
    u32 joined = wb.Taint.Union(la, lb);
    CHECK(joined != la && joined != lb && wb.Taint.Union(joined, la) == joined && wb.Taint.Union(joined, joined) == joined);
    CHECK(wb.Taint.Union(la, 0) == la && wb.Taint.Union(0, lb) == lb);
    const std::vector<u32>& srcs = wb.Taint.LabelSources(joined);
    CHECK(srcs.size() == 2 && srcs[0] == idA && srcs[1] == idB);
    CHECK(wb.Taint.LabelSources(0xFFFFFFFF).empty());
    const Origin::LabelMerge& made = wb.Taint.MergeOf(joined);
    CHECK((made.A == la && made.B == lb) || (made.A == lb && made.B == la));
    CHECK(std::string(Origin::MergeOpName(Origin::MergeAlu)) == "computed");
    CHECK(std::string(Origin::MergeOpName(Origin::MergeAddress)) == "table_lookup");
    CHECK(std::string(Origin::MergeOpName(Origin::MergeBytes)) == "mixed_bytes");
    CHECK(std::string(Origin::MergeOpName(Origin::MergeMath)) == "math_unit");
    CHECK(std::string(Origin::MergeOpName(Origin::MergeCopy)) == "copy");
    CHECK(std::string(Origin::MergeOpName(Origin::MergeSource)) == "source_added");
    CHECK(Origin::MergeOpName(255)[0] == 0);
    std::vector<u8> merges;
    wb.Taint.SerializeMerges(merges);
    CHECK(merges.size() == sizeof(LabelMergeEntry));

    PhysAddr pa;
    pa.Region = (u16)Region::MainRAM;
    pa.Offset = 0x120;
    wb.Taint.SetMemLabel(pa, la);
    CHECK(wb.Taint.GetMemLabel(pa) == la);
    PhysAddr io;
    io.Region = (u16)Region::IO9;
    wb.Taint.SetMemLabel(io, la);
    CHECK(wb.Taint.GetMemLabel(io) == 0);
    wb.Taint.SetMemLabel(pa, 0);
    CHECK(wb.Taint.GetMemLabel(pa) == 0);

    DataSource reg;
    reg.Kind = SourceKind::Register;
    reg.Name = "r3in";
    reg.Reg = 3;
    CHECK(wb.Taint.AddSource(reg) && wb.Taint.RegLabel(0, 3) != 0);
    CHECK(wb.Taint.RemoveSource(idB) && wb.Taint.FindSource(idB) && !wb.Taint.FindSource(idB)->Enabled);
    wb.Taint.SetAllRomFiles(false);
    CHECK(wb.Taint.SourceFor(7, 0) == 0);
    wb.Taint.SetAllRomFiles(true);
    u32 file = wb.Taint.SourceFor(7, 0);
    CHECK(file && wb.Taint.SourceFor(7, 0) == file && wb.Taint.FindSource(file)->Name == "file 7");
}

// ---------------------------------------------------------------- compare (WBCompare.h)

// the defined and undefined symbols, and the relocations, of an object WriteTargetObject made
struct ElfRead
{
    bool Ok = false;
    std::map<std::string, std::pair<u32, u16>> Syms;     // name -> value, section (0 = undefined)
    std::vector<std::pair<u32, u32>> Rels;               // offset, info
    std::vector<u8> Text;
};

static ElfRead ReadElf(const QByteArray& b)
{
    ElfRead r;
    auto u16at = [&](int o) { return o + 2 <= b.size() ? (u32)(u8)b[o] | ((u32)(u8)b[o + 1] << 8) : 0u; };
    auto u32at = [&](int o) { return u16at(o) | (u16at(o + 2) << 16); };
    if (b.size() < 52 || !b.startsWith("\x7f" "ELF") || u16at(18) != 40) return r;
    u32 shoff = u32at(32), shnum = u16at(48);
    if (shoff + shnum * 40 > (u32)b.size()) return r;
    auto sh = [&](u32 i, int field) { return u32at(shoff + i * 40 + field * 4); };
    for (u32 i = 0; i < shnum; i++)
    {
        u32 type = sh(i, 1), off = sh(i, 4), size = sh(i, 5), link = sh(i, 6);
        if (type == 1) r.Text.assign(b.begin() + off, b.begin() + off + size);
        if (type == 9) for (u32 o = off; o < off + size; o += 8) r.Rels.push_back({u32at(o), u32at(o + 4)});
        if (type == 2)
        {
            u32 str = sh(link, 4);
            for (u32 o = off + 16; o < off + size; o += 16)
            {
                std::string name(b.constData() + str + u32at(o));
                r.Syms[name + (r.Syms.count(name) ? "#" + std::to_string(o) : "")] = {u32at(o + 4), (u16)u16at(o + 14)};
            }
        }
    }
    r.Ok = true;
    return r;
}

static void TestCompare()
{
    // ---- the command's answer
    {
        auto o = WBCmp::ParseCompareOutput("{\"score\": 82.5, \"matched\": false, \"compiler\": \"mwcc\", \"flags\": \"-O4\", "
                                           "\"diff\": [{\"orig\": \"bx lr\", \"compiled\": \"bx lr\", \"status\": \"match\"}], \"recorded\": false}");
        CHECK(o.Found && o.Errors.isEmpty() && o.Obj.value("score").toDouble() == 82.5);
        // a tool that logs before answering: the last line that is an object
        o = WBCmp::ParseCompareOutput("compiling...\n{not json\n{\"score\": 100, \"matched\": true, \"diff\": []}\n");
        CHECK(o.Found && o.Errors.isEmpty() && o.Obj.value("matched").toBool());
        o = WBCmp::ParseCompareOutput("cc: error 1\n");
        CHECK(!o.Found && o.Errors.size() == 1 && o.Errors[0] == "no JSON object in the output");
        o = WBCmp::ParseCompareOutput("{\"score\": \"90\", \"diff\": {}}");
        CHECK(o.Errors.contains("`score` must be a number") && o.Errors.contains("`diff` must be a list"));
        o = WBCmp::ParseCompareOutput("{\"score\": 50, \"diff\": [{\"orig\": \"a\", \"compiled\": \"b\", \"status\": \"diff\"}, {\"orig\": 1, \"status\": \"x\"}]}");
        CHECK(o.Errors.size() == 1 && o.Errors[0].startsWith("`diff[1]`"));
        o = WBCmp::ParseCompareOutput("{\"score\": 150, \"diff\": [], \"stage\": \"linking\", \"matched\": 1}");
        CHECK(o.Errors.contains("`score` must be between 0 and 100") && o.Errors.contains("`matched` must be true or false")
              && o.Errors.contains("`stage` must be one of compile, extract, diff, done"));
        o = WBCmp::ParseCompareOutput("{\"score\": 10}");
        CHECK(o.Errors.contains("`diff` is missing"));
        // a failure needs neither score nor diff
        o = WBCmp::ParseCompareOutput("{\"matched\": false, \"stage\": \"compile\", \"error\": \"the compiler failed (exit 1)\"}");
        CHECK(o.Found && o.Errors.isEmpty());
    }

    // ---- explanations
    CHECK(WBCmp::ExplainCompareFailure("compile", "timed out running mwccarm.exe", "").contains("timeout"));
    CHECK(WBCmp::ExplainCompareFailure("compile", "not found: C:/mw/mwccarm.exe", "").contains("compiler could not be found"));
    CHECK(WBCmp::ExplainCompareFailure("diff", "not found: objdiff-cli", "").contains("objdiff-cli could not be found"));
    CHECK(WBCmp::ExplainCompareFailure("diff", "objdiff-cli 2.7.1 is too old; 3.8 or newer is needed", "").contains("too old"));
    CHECK(WBCmp::ExplainCompareFailure("extract", "symbol 'Foo' is not in the compiled object", "").contains("no function with this name"));
    CHECK(WBCmp::ExplainCompareFailure("compile", "the compiler failed (exit 1)", "### mwccarm.exe Driver Error:\n#   The environment variable 'MWCIncludes' is not set").contains("MWCIncludes"));
    CHECK(WBCmp::ExplainCompareFailure("compile", "the compiler failed (exit 1)", "foo.c:3: error: expected ';'").contains("did not compile"));
    CHECK(WBCmp::ExplainCompareFailure("run", "the compare command could not be started", "").contains("Python"));
    CHECK(WBCmp::ExplainCompareFailure("done", "something new", "").isEmpty());

    // ---- compare.json
    {
        WBCmp::CompareConfig c;
        QString err;
        CHECK(c.Parse(WBCmp::CompareConfig::Template("arm9"), err));
        CHECK(c.Rules.size() == 1 && c.Resolve("arm9", 0x02000000).Label == "mwcc-2.0-sp1p5" && c.Resolve("arm9", 0).From == "module");
        CHECK(c.Resolve("ov003", 0x02100000).From.isEmpty() && c.Resolve("ov003", 0).Label.isEmpty());
        CHECK(c.Parse("{\"default\": {\"compiler\": \"game\", \"flags\": \"-O4,p\"},"
                      " \"modules\": [{\"module\": \"arm9\", \"compiler\": \"sdk\", \"flags\": \"-O4,s\", \"context\": false},"
                      "               {\"ranges\": [[\"0x02010000\", \"0x02020000\"]], \"compiler\": \"lib\", \"flags\": \"-O2\"}]}", err));
        WBCmp::CompilerChoice ch = c.Resolve("arm9", 0x02010100);
        CHECK(ch.Label == "lib" && ch.Flags == "-O2" && ch.From == "range" && ch.Context);
        ch = c.Resolve("arm9", 0x02000100);
        CHECK(ch.Label == "sdk" && ch.Flags == "-O4,s" && ch.From == "module" && !ch.Context);
        ch = c.Resolve("ov001", 0x02200000);
        CHECK(ch.Label == "game" && ch.Flags == "-O4,p" && ch.From == "default" && ch.Context);
        // round trip
        WBCmp::CompareConfig c2;
        CHECK(c2.Parse(c.ToJson(), err) && c2.Rules.size() == 2 && c2.Resolve("arm9", 0x02010100).Label == "lib" && !c2.Resolve("arm9", 0).Context);
        CHECK(!c.Parse("{\"modules\": [", err) && err.contains("not valid JSON"));
        CHECK(!c.Parse("{\"modules\": [{\"compiler\": \"x\"}]}", err) && err.contains("needs a `module` or `ranges`"));
        CHECK(!c.Parse("{\"modules\": [{\"module\": \"arm9\", \"ranges\": [[5, 1]]}]}", err) && err.contains("end after start"));
        CHECK(!c.Parse("{\"default\": {\"context\": \"yes\"}}", err) && err.contains("`context`"));
    }

    // ---- fingerprint and line diff
    CHECK(WBCmp::CommandFingerprint("a {file}", "x") != WBCmp::CommandFingerprint("a {file}", "y"));
    CHECK(WBCmp::CommandFingerprint("a {file}", "x") == WBCmp::CommandFingerprint("a {file}", "x"));
    {
        auto d = WBCmp::LineDiff("a\nb\nc\nd", "a\nB\nc\nd\ne");
        std::string s;
        for (auto& [op, line] : d) s += std::string(1, op) + line.toStdString() + "|";
        CHECK_EQ_STR(s, " a|+B|-b| c| d|+e|");
        CHECK(WBCmp::LineDiff("same\ntext", "same\ntext").size() == 2);
    }

    // ---- target object: a Thumb function with a call, a literal-pool data pointer and a function pointer
    //   0  ldr r3, [pc, #12]   -> pool word at 16 (gCar + 4)
    //   2  push {r4, lr}
    //   4  ldr r3, [r3]
    //   6  adds r0, r0, r3
    //   8  bl helper (0x02000100)
    //  12  pop {r4, pc}
    //  14  ldr r2, [pc, #4]    -> pool word at 20 (helper, Thumb)
    //  16  .word 0x02000204
    //  20  .word 0x02000101
    std::vector<u8> code = {0x03, 0x4B, 0x10, 0xB5, 0x1B, 0x68, 0xC0, 0x18, 0x00, 0xF0, 0x7A, 0xF8, 0x10, 0xBD, 0x01, 0x4A,
                            0x04, 0x02, 0x00, 0x02, 0x01, 0x01, 0x00, 0x02};
    auto resolve = [](u32 v, bool call, WBCmp::SymbolRef& s)
    {
        if ((v & ~1u) == 0x02000100) { s.Name = "helper"; s.Function = true; s.FunctionId = 2; return true; }
        if (!call && v >= 0x02000200 && v < 0x02000210) { s.Name = "gCar"; s.Addend = v - 0x02000200; s.DataObjectId = 1; return true; }
        return false;
    };
    WBCmp::CodeAnalysis an = WBCmp::AnalyzeCode(code, 0x02000000, true, resolve);
    CHECK(an.Spans.size() == 2 && !an.Spans[0].Data && an.Spans[0].Size == 16 && an.Spans[1].Data && an.Spans[1].Offset == 16 && an.Spans[1].Size == 8);
    CHECK(an.Relocs.size() == 3);
    if (an.Relocs.size() == 3)
    {
        CHECK(an.Relocs[0].Offset == 8 && an.Relocs[0].Type == WBCmp::CodeReloc::Call && an.Relocs[0].Thumb && an.Relocs[0].Sym.Name == "helper");
        CHECK(an.Relocs[1].Offset == 16 && an.Relocs[1].Type == WBCmp::CodeReloc::Abs32 && an.Relocs[1].Sym.Name == "gCar" && an.Relocs[1].Sym.Addend == 4);
        CHECK(an.Relocs[2].Offset == 20 && an.Relocs[2].Sym.Name == "helper");
    }
    ElfRead e = ReadElf(WBCmp::WriteTargetObject("Car_Speed", code, true, an));
    CHECK(e.Ok && e.Text.size() == code.size());
    CHECK(e.Syms.count("Car_Speed") && e.Syms["Car_Speed"].first == 1 && e.Syms["Car_Speed"].second == 1);   // Thumb: bit 0
    CHECK(e.Syms.count("helper") && e.Syms["helper"].second == 0 && e.Syms.count("gCar") && e.Syms["gCar"].second == 0);
    CHECK(e.Syms.count("$t") && e.Syms["$t"].first == 0 && e.Syms.count("$d") && e.Syms["$d"].first == 16);
    CHECK(e.Rels.size() == 3);
    if (e.Rels.size() == 3 && e.Text.size() == code.size())
    {
        CHECK((e.Rels[0].second & 0xFF) == 10 && e.Rels[0].first == 8);       // R_ARM_THM_CALL
        CHECK((e.Rels[1].second & 0xFF) == 2 && e.Rels[1].first == 16);       // R_ARM_ABS32
        CHECK((e.Rels[2].second & 0xFF) == 2 && (e.Rels[2].second >> 8) == (e.Rels[0].second >> 8));   // the same helper symbol
        // in place: the addends, as an assembler leaves them
        CHECK(e.Text[8] == 0xFF && e.Text[9] == 0xF7 && e.Text[10] == 0xFE && e.Text[11] == 0xFF);
        CHECK(e.Text[16] == 4 && e.Text[17] == 0 && e.Text[18] == 0 && e.Text[19] == 0);
        CHECK(e.Text[20] == 0 && e.Text[23] == 0);
        CHECK(e.Text[0] == 0x03 && e.Text[1] == 0x4B);      // unrelocated bytes kept
    }
    // ARM: bl to a known function, a tail branch to another, a call to an unknown address
    //  0  bl  0x02000100
    //  4  bl  0x02000300 (unknown)
    //  8  b   0x02000100
    std::vector<u8> arm = {0x3E, 0x00, 0x00, 0xEB, 0xBD, 0x00, 0x00, 0xEB, 0x3C, 0x00, 0x00, 0xEA};
    an = WBCmp::AnalyzeCode(arm, 0x02000000, false, resolve);
    CHECK(an.Relocs.size() == 2 && an.Relocs[0].Type == WBCmp::CodeReloc::Call && an.Relocs[1].Type == WBCmp::CodeReloc::Jump && an.Relocs[1].Offset == 8);
    e = ReadElf(WBCmp::WriteTargetObject("tail", arm, false, an));
    CHECK(e.Ok && e.Rels.size() == 2 && (e.Rels[0].second & 0xFF) == 28 && (e.Rels[1].second & 0xFF) == 29);
    if (e.Text.size() == 12)
    {
        CHECK(e.Text[0] == 0xFE && e.Text[1] == 0xFF && e.Text[2] == 0xFF && e.Text[3] == 0xEB);
        CHECK(e.Text[4] == 0xBD && e.Text[7] == 0xEB);      // the unknown call stays as it is
        CHECK(e.Text[8] == 0xFE && e.Text[11] == 0xEA);
    }
    CHECK(e.Syms.count("$a") && e.Syms.count("tail") && e.Syms["tail"].first == 0);

    // ---- context header
    {
        QTemporaryDir dir;
        std::vector<u8> rom = MakeRom();
        NitroROM r;
        r.Load(rom.data(), (u32)rom.size(), (u32)rom.size());
        WBKB::KnowledgeStore kb;
        QString err;
        CHECK(kb.Open(dir.filePath("kb.db"), r, "tester", err));
        CHECK(kb.DefineType("struct Wheel { u32 radius; };", err));
        CHECK(kb.DefineType("struct Car { u32 flags; struct Wheel front; struct Car* next; };", err));
        CHECK(kb.DefineType("typedef s32 Speed;", err));
        qint64 self = 0, helper = 0, other = 0;
        CHECK(kb.UpsertFunction(0, 0x02000400, 0x18, "thumb", "Car_Speed", -1, err, &self));
        CHECK(kb.UpsertFunction(0, 0x02000500, 0x10, "thumb", "Car_Helper", -1, err, &helper));
        CHECK(kb.UpsertFunction(0, 0x02000600, 0x10, "arm", "", -1, err, &other));
        QJsonObject o;
        o["signature"] = "Speed Car_Speed(struct Car* car)";
        CHECK(kb.UpdateFunction(self, o, err));
        o["signature"] = "s32 Car_Helper(Car* c, s32 n);";
        CHECK(kb.UpdateFunction(helper, o, err));
        CHECK(kb.DefineData(0, 0x02000800, 0, "Car", "gPlayerCar", -1, err));
        const WBKB::DataObject* d = kb.DataObjectAt(0, 0x02000800);
        CHECK(d != nullptr);
        WBCmp::CodeAnalysis ca;
        WBCmp::CodeReloc c1;
        c1.Sym.Name = "Car_Helper"; c1.Sym.FunctionId = helper; c1.Type = WBCmp::CodeReloc::Call;
        WBCmp::CodeReloc c2;
        c2.Sym.Name = "sub_02000600"; c2.Sym.FunctionId = other; c2.Type = WBCmp::CodeReloc::Call;
        WBCmp::CodeReloc c3;
        c3.Sym.Name = "gPlayerCar"; c3.Sym.DataObjectId = d ? d->Id : 0;
        ca.Relocs = {c1, c2, c3, c1};
        QString h = WBCmp::GenerateContext(kb, {kb.FunctionById(self)}, "Car_Speed", ca);
        CHECK(h.contains("#ifndef DSWB_CONTEXT_H"));
        CHECK(h.contains("typedef unsigned long u32;") && h.contains("typedef signed long s32;"));
        CHECK(h.contains("typedef struct Car Car;") && h.contains("typedef struct Wheel Wheel;"));
        int wheel = h.indexOf("struct Wheel { u32 radius; };"), car = h.indexOf("struct Car { u32 flags;");
        CHECK(wheel > 0 && car > wheel);                 // what a type holds by value comes first
        CHECK(h.contains("typedef s32 Speed;"));
        CHECK(h.contains("extern Car gPlayerCar;"));
        CHECK(h.contains("s32 Car_Helper(Car* c, s32 n);") && h.count("s32 Car_Helper(") == 1);
        CHECK(h.contains("/* sub_02000600: no signature in the knowledge store */"));
        CHECK(h.contains("Speed Car_Speed(struct Car* car);"));
        CHECK(h.indexOf("typedef s32 Speed;") < h.indexOf("Speed Car_Speed("));
        // a C file with both functions: each is declared once, among the file's own
        h = WBCmp::GenerateContext(kb, {kb.FunctionById(self), kb.FunctionById(helper)}, "car.c", ca);
        CHECK(h.count("s32 Car_Helper(") == 1 && h.indexOf("s32 Car_Helper(") > h.indexOf("/* sub_02000600"));
        CHECK(h.contains("Context for car.c"));
    }
}

// ---------------------------------------------------------------- graphics formats (WBGfx.h)

namespace GfxBuild
{
struct Out
{
    std::vector<u8> V;
    void U8(u32 v) { V.push_back((u8)v); }
    void U16(u32 v) { U8(v & 0xFF); U8(v >> 8); }
    void U32(u32 v) { U16(v & 0xFFFF); U16(v >> 16); }
    void Str(const char* s) { for (; *s; s++) U8((u8)*s); }
    void Align(u32 a) { while (V.size() % a) U8(0); }
    void Put32(u32 at, u32 v) { for (int i = 0; i < 4; i++) V[at + i] = (u8)(v >> (8 * i)); }
    void Put16(u32 at, u32 v) { V[at] = (u8)v; V[at + 1] = (u8)(v >> 8); }
    u32 Size() const { return (u32)V.size(); }
};

// a 2D Nitro file: header, then sections (magic already reversed, payload after the 8-byte section header)
static std::vector<u8> File2D(const char* magic, const std::vector<std::pair<const char*, std::vector<u8>>>& secs)
{
    Out o;
    o.Str(magic);
    o.U16(0xFEFF); o.U16(0x0100); o.U32(0); o.U16(0x10); o.U16((u32)secs.size());
    for (auto& [m, body] : secs)
    {
        o.Str(m);
        o.U32((u32)body.size() + 8);
        o.V.insert(o.V.end(), body.begin(), body.end());
    }
    o.Put32(8, o.Size());
    return o.V;
}

static u16 PalColor(int i) { return (u16)((i & 31) | (((31 - i) & 31) << 5) | (((i * 3) & 31) << 10)); }

// 32 colours: two 16-colour palettes
static std::vector<u8> Nclr()
{
    Out b;
    b.U32(3); b.U32(0); b.U32(64); b.U32(0x10);
    for (int i = 0; i < 32; i++) b.U16(PalColor(i));
    return File2D("RLCN", {{"TTLP", b.V}});
}

// 4 x 2 tiles; the pixel at (x, y) of the whole image is (x + 3y) & 15 at 4bpp, (7x + y) & 255 at 8bpp
static int TilePixel(int bpp, int x, int y) { return bpp == 4 ? (x + 3 * y) & 15 : (7 * x + y) & 255; }

static std::vector<u8> Ncgr(int bpp)
{
    Out b;
    b.U16(2); b.U16(4); b.U32(bpp == 4 ? 3 : 4); b.U32(0); b.U32(0);
    u32 size = 8 * 8 * bpp;
    b.U32(size); b.U32(0x18);
    for (int t = 0; t < 8; t++)
        for (int py = 0; py < 8; py++)
            for (int px = 0; px < 8; px += 8 / bpp)
            {
                int x = (t % 4) * 8 + px, y = (t / 4) * 8 + py;
                if (bpp == 4) b.U8(TilePixel(4, x, y) | (TilePixel(4, x + 1, y) << 4));
                else b.U8(TilePixel(8, x, y));
            }
    return File2D("RGCN", {{"RAHC", b.V}});
}

// 4 x 2 entries over the 4bpp tiles: entry k shows tile k % 8, H-flipped when k is odd, V-flipped
// when bit 1 is set, with palette k % 2
static u16 MapEntry(int k) { return (u16)((k % 8) | ((k & 1) << 10) | (((k >> 1) & 1) << 11) | ((k % 2) << 12)); }

static std::vector<u8> Nscr()
{
    Out b;
    b.U16(32); b.U16(16); b.U16(0); b.U16(0); b.U32(16);
    for (int k = 0; k < 8; k++) b.U16(MapEntry(k));
    return File2D("RCSN", {{"NRCS", b.V}});
}

// one cell: a 16x16 OAM at (-8, -8) from tile 0, and an H-flipped 8x8 one at (8, 0) from tile 4,
// palette 1; 1D 32K mapping
static std::vector<u8> Ncer()
{
    Out b;
    b.U16(1); b.U16(0); b.U32(0x18); b.U32(1); b.U32(0); b.U32(0); b.U32(0);
    while (b.Size() < 0x18) b.U8(0);
    b.U16(2); b.U16(0); b.U32(0);                        // cell 0: 2 OAMs at +0
    b.U16(0xF8); b.U16(0x4000 | 0x1F8); b.U16(0);        // 16x16 square at (-8, -8), tile 0
    b.U16(0x00); b.U16(0x1000 | 0x008); b.U16(0x1004);   // 8x8 at (8, 0), H-flip, tile 4, palette 1
    return File2D("RECN", {{"KBEC", b.V}});
}

// textures, 8x8 each: a3i5, c4, c16, c256, cmp4x4, a5i3, direct (formats 1-7); the texel at (x, y)
// is (x + 8y) masked to the format's index bits (direct: a BGR555 colour of it)
static const char* TexNames[7] = {"a3i5", "c4", "c16", "c256", "cmp4x4", "a5i3", "direct"};

static u32 TexIndex(int fmt, int x, int y)
{
    int v = x + 8 * y;
    switch (fmt)
    {
    case 1: return (u32)((v & 31) | (((v >> 3) & 7) << 5));
    case 2: return (u32)(v & 3);
    case 3: return (u32)(v & 15);
    case 4: return (u32)(v & 255);
    case 6: return (u32)((v & 7) | (((v >> 1) & 31) << 3));
    case 7: return (u32)(PalColor(v) | ((v & 1) << 15));
    }
    return 0;
}

static std::vector<u8> Dict(const std::vector<std::string>& names, const std::vector<std::vector<u8>>& entries, u32 esize)
{
    Out o;
    u32 n = (u32)names.size();
    o.U8(0); o.U8(n); o.U16(0);
    o.U16(8); o.U16(12 + 4 * n); o.U32(0x17F);
    for (u32 i = 0; i < n; i++) o.U32(0);      // the tree: its root (0x17F above) and one node per entry
    o.U16(esize); o.U16(4 + esize * n);
    for (auto& e : entries) o.V.insert(o.V.end(), e.begin(), e.end());
    for (auto& nm : names) { for (int i = 0; i < 16; i++) o.U8(i < (int)nm.size() ? (u8)nm[i] : 0); }
    o.Put16(2, o.Size());
    return o.V;
}

static std::vector<u8> Nsbtx()
{
    Out tex, cmp, idx, pal;
    std::vector<std::vector<u8>> texEntries, palEntries;
    std::vector<std::string> palNames;
    for (int f = 1; f <= 7; f++)
    {
        Out e;
        u32 param;
        if (f == 5)
        {
            // four 4x4 blocks: block k's texels are (x + 4y + k) & 3, its mode is k, its colours start at k * 4
            param = (cmp.Size() >> 3);
            for (int k = 0; k < 4; k++)
            {
                u32 w = 0;
                for (int i = 0; i < 16; i++) w |= (u32)(((i % 4) + 4 * (i / 4) + k) & 3) << (i * 2);
                cmp.U32(w);
                idx.U16((u32)(k * 2) | ((u32)k << 14));     // palette +k*8 bytes (4 colours), mode k
            }
        }
        else
        {
            param = tex.Size() >> 3;
            int bits = f == 1 || f == 4 || f == 6 ? 8 : f == 2 ? 2 : f == 3 ? 4 : 16;
            u64 acc = 0;
            int have = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                {
                    acc |= (u64)TexIndex(f, x, y) << have;
                    have += bits;
                    while (have >= 8) { tex.U8((u32)(acc & 0xFF)); acc >>= 8; have -= 8; }
                }
            tex.Align(8);
        }
        param |= (0u << 20) | (0u << 23) | ((u32)f << 26) | (f == 3 ? (1u << 29) : 0);   // 8x8; c16 has colour 0 transparent
        e.U32(param); e.U32(0x00800008);
        texEntries.push_back(e.V);
        if (f != 7)
        {
            u32 colors = f == 1 ? 32 : f == 2 ? 4 : f == 3 ? 16 : f == 4 ? 256 : f == 5 ? 16 : 8;
            Out pe;
            pe.U16(pal.Size() >> 3); pe.U16(0);
            palEntries.push_back(pe.V);
            palNames.push_back(std::string(TexNames[f - 1]) + "_pl");
            for (u32 i = 0; i < colors; i++) pal.U16(PalColor((int)(i + f)));
            pal.Align(8);
        }
    }
    std::vector<std::string> texNames(TexNames, TexNames + 7);
    std::vector<u8> td = Dict(texNames, texEntries, 8), pd = Dict(palNames, palEntries, 4);
    Out t0;
    t0.Str("TEX0"); t0.U32(0);
    while (t0.Size() < 0x3C) t0.U8(0);
    u32 texDict = t0.Size();
    t0.V.insert(t0.V.end(), td.begin(), td.end()); t0.Align(8);
    u32 palDict = t0.Size();
    t0.V.insert(t0.V.end(), pd.begin(), pd.end()); t0.Align(8);
    u32 texData = t0.Size(); t0.V.insert(t0.V.end(), tex.V.begin(), tex.V.end()); t0.Align(8);
    u32 cmpData = t0.Size(); t0.V.insert(t0.V.end(), cmp.V.begin(), cmp.V.end()); t0.Align(8);
    u32 cmpIdx = t0.Size(); t0.V.insert(t0.V.end(), idx.V.begin(), idx.V.end()); t0.Align(8);
    u32 palData = t0.Size(); t0.V.insert(t0.V.end(), pal.V.begin(), pal.V.end()); t0.Align(8);
    t0.Put32(4, t0.Size());
    t0.Put16(0x0C, tex.Size() >> 3); t0.Put16(0x0E, texDict); t0.Put32(0x14, texData);
    t0.Put16(0x1C, cmp.Size() >> 3); t0.Put16(0x1E, texDict); t0.Put32(0x24, cmpData); t0.Put32(0x28, cmpIdx);
    t0.Put32(0x30, pal.Size() >> 3); t0.Put32(0x34, palDict); t0.Put32(0x38, palData);
    Out f;
    f.Str("BTX0"); f.U16(0xFEFF); f.U16(1); f.U32(0); f.U16(0x10); f.U16(1); f.U32(0x14);
    f.V.insert(f.V.end(), t0.V.begin(), t0.V.end());
    f.Put32(8, f.Size());
    return f.V;
}
}

static void TestGfx()
{
    using namespace GfxBuild;
    auto rgb = [](u16 c) { return WBGfx::Bgr555(c); };

    // ---- NCLR
    std::vector<u8> nclr = Nclr();
    WBGfx::Description pd = WBGfx::Describe(nclr.data(), (u32)nclr.size());
    CHECK(pd.Format == "NCLR" && pd.UnreadableFrom == 0 && pd.PalBpp == 4 && pd.PalOffset == 0x28 && pd.PalSize == 64);
    std::vector<QRgb> pal = WBGfx::PaletteColors(pd, nclr.data(), (u32)nclr.size());
    CHECK(pal.size() == 32 && pal[7] == rgb(PalColor(7)) && pal[31] == rgb(PalColor(31)));
    int palettes = 0;
    for (auto& p : pd.Parts) if (p.Kind == "palette") palettes++;
    CHECK(palettes == 2);
    WBGfx::FieldInfo fi = WBGfx::DescribeAt(pd, nclr.data(), (u32)nclr.size(), 0x28 + 2 * 23);
    CHECK(fi.HasColor && fi.Color == PalColor(23) && fi.Palette == 1);
    CHECK(fi.Text.rfind("NCLR palette 1, colour 7: 0x", 0) == 0);
    CHECK(WBGfx::DescribeAt(pd, nclr.data(), (u32)nclr.size(), 9).Text == "NCLR header, file size");
    CHECK(WBGfx::RenderPalette(pal, 4).size() == QSize(64, 8));

    // ---- NCGR, 4bpp and 8bpp
    for (int bpp : {4, 8})
    {
        std::vector<u8> g = Ncgr(bpp);
        WBGfx::Description gd = WBGfx::Describe(g.data(), (u32)g.size());
        CHECK(gd.Format == "NCGR" && gd.UnreadableFrom == 0 && gd.CharBpp == bpp && gd.TilesX == 4 && gd.TilesY == 2 && !gd.Linear);
        CHECK(gd.CharOffset == 0x30 && gd.CharSize == 64u * bpp);
        std::vector<QRgb> grey;
        for (int i = 0; i < 256; i++) grey.push_back(qRgb(i, i, i));
        QImage img = WBGfx::RenderTiles(gd, g.data(), (u32)g.size(), grey, 0);
        CHECK(img.size() == QSize(32, 16));
        bool all = true;
        for (int y = 0; y < 16 && !img.isNull(); y++)
            for (int x = 0; x < 32; x++)
                if (qRed(img.pixel(x, y)) != TilePixel(bpp, x, y)) all = false;
        CHECK(all);
        // tile 5 is at (8, 8) in the image; row 3 starts 3 rows into it
        u32 tb = 8u * bpp;
        fi = WBGfx::DescribeAt(gd, g.data(), (u32)g.size(), 0x30 + 5 * tb + 3 * (u32)bpp + 1);
        CHECK(fi.Tile == 5 && fi.Text == "NCGR CHAR, tile 5, row 3");
        u32 off, size;
        CHECK(WBGfx::TileBytes(gd, 5, off, size) && off == 0x30 + 5 * tb && size == tb);
        QImage t5 = WBGfx::RenderTile(gd, g.data(), (u32)g.size(), 5, grey, 0);
        CHECK(t5.size() == QSize(8, 8) && qRed(t5.pixel(2, 1)) == TilePixel(bpp, 8 + 2, 8 + 1));
    }

    // ---- NSCR with flips and palettes
    {
        std::vector<u8> s = Nscr(), g = Ncgr(4);
        WBGfx::Description sd = WBGfx::Describe(s.data(), (u32)s.size()), gd = WBGfx::Describe(g.data(), (u32)g.size());
        CHECK(sd.Format == "NSCR" && sd.UnreadableFrom == 0 && sd.MapWidth == 32 && sd.MapHeight == 16 && sd.MapOffset == 0x24 && sd.MapSize == 16);
        QImage img = WBGfx::RenderMap(sd, s.data(), (u32)s.size(), gd, g.data(), (u32)g.size(), pal);
        CHECK(img.size() == QSize(32, 16));
        bool all = true;
        for (int k = 0; k < 8 && !img.isNull(); k++)
        {
            u16 e = MapEntry(k);
            int tile = e & 0x3FF, hf = (e >> 10) & 1, vf = (e >> 11) & 1, pn = e >> 12;
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++)
                {
                    int sx = hf ? 7 - px : px, sy = vf ? 7 - py : py;
                    int v = TilePixel(4, (tile % 4) * 8 + sx, (tile / 4) * 8 + sy);
                    QRgb want = v ? pal[pn * 16 + v] : 0;
                    if (img.pixel((k % 4) * 8 + px, (k / 4) * 8 + py) != want) all = false;
                }
        }
        CHECK(all);
        fi = WBGfx::DescribeAt(sd, s.data(), (u32)s.size(), 0x24 + 2 * 7);
        CHECK(fi.Text == "NSCR entry (3,1): tile 0x7, H-flip, V-flip, palette 1" && fi.Tile == 7 && fi.Palette == 1);
    }

    // ---- NCER
    {
        std::vector<u8> c = Ncer(), g = Ncgr(4);
        WBGfx::Description cd = WBGfx::Describe(c.data(), (u32)c.size()), gd = WBGfx::Describe(g.data(), (u32)g.size());
        CHECK(cd.Format == "NCER" && cd.UnreadableFrom == 0 && cd.Cells.size() == 1 && cd.CellMapping == 1);
        if (cd.Cells.size() == 1 && cd.Cells[0].Oams.size() == 2)
        {
            const WBGfx::Oam& a = cd.Cells[0].Oams[0];
            const WBGfx::Oam& b = cd.Cells[0].Oams[1];
            CHECK(a.X == -8 && a.Y == -8 && a.Width == 16 && a.Height == 16 && a.Tile == 0 && !a.HFlip);
            CHECK(b.X == 8 && b.Y == 0 && b.Width == 8 && b.Height == 8 && b.Tile == 4 && b.HFlip && b.Palette == 1);
            QPoint origin;
            QImage img = WBGfx::RenderCell(cd, 0, gd, g.data(), (u32)g.size(), pal, &origin);
            CHECK(img.size() == QSize(24, 16) && origin == QPoint(8, 8));
            // the 16x16 OAM at (0, 0): tiles 0, 1 / 2, 3 in 1D order; tile 2 is image tile (2, 0) of the NCGR
            int v = TilePixel(4, 2 * 8 + 1, 0 + 2);
            CHECK(!img.isNull() && img.pixel(1, 8 + 2) == (v ? pal[v] : 0));
            // the flipped 8x8 at (16, 8): tile 4 is NCGR tile (0, 1), palette 1
            v = TilePixel(4, 7 - 2, 8 + 5);
            CHECK(!img.isNull() && img.pixel(16 + 2, 8 + 5) == (v ? pal[16 + v] : 0));
            fi = WBGfx::DescribeAt(cd, c.data(), (u32)c.size(), cd.Cells[0].OamOffset + 6 + 2);
            CHECK(fi.Text.rfind("NCER cell 0, OAM 1, attr1: x 8, y 0, 8x8, tile 0x4, palette 1, H-flip", 0) == 0);
        }
        else CHECK(false);
    }

    // ---- NSBTX: every TEX0 format
    {
        std::vector<u8> t = Nsbtx();
        WBGfx::Description td = WBGfx::Describe(t.data(), (u32)t.size());
        CHECK(td.Format == "BTX0" && td.UnreadableFrom == 0 && td.Textures.size() == 7 && td.TexPalettes.size() == 6);
        if (td.UnreadableFrom) printf("  BTX0 problem at 0x%X: %s\n", td.UnreadableFrom, td.Problem.c_str());
        for (int f = 1; f <= 7 && td.Textures.size() == 7; f++)
        {
            const WBGfx::Texture& tx = td.Textures[f - 1];
            CHECK(tx.Name == TexNames[f - 1] && tx.Format == f && tx.Width == 8 && tx.Height == 8);
            int palIdx = WBGfx::TexturePaletteFor(td, f - 1);
            CHECK(f == 7 || (palIdx == f - 1 && td.TexPalettes[palIdx].Name == std::string(TexNames[f - 1]) + "_pl"));
            QImage img = WBGfx::RenderTexture(td, t.data(), (u32)t.size(), f - 1);
            CHECK(img.size() == QSize(8, 8));
            if (img.isNull()) continue;
            auto palc = [&](int i) { return rgb(PalColor(i + f)); };
            bool all = true;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                {
                    u32 v = TexIndex(f, x, y);
                    QRgb want = 0;
                    auto alpha = [](QRgb c, int a) { return qRgba(qRed(c), qGreen(c), qBlue(c), a); };
                    auto up5 = [](int a) { return (a << 3) | (a >> 2); };
                    switch (f)
                    {
                    case 1: want = alpha(palc(v & 31), up5((int)(v >> 5) * 4 + (int)(v >> 5) / 2)); break;
                    case 2: want = palc((int)v); break;
                    case 3: want = v ? palc((int)v) : 0; break;     // colour 0 transparent
                    case 4: want = palc((int)v); break;
                    case 6: want = alpha(palc(v & 7), up5((int)(v >> 3))); break;
                    case 7: want = WBGfx::Bgr555((u16)v, (v >> 15) != 0); break;
                    case 5:
                    {
                        int k = (y / 4) * 2 + x / 4;
                        int i = ((x % 4) + 4 * (y % 4) + k) & 3;
                        QRgb c0 = palc(k * 4), c1 = palc(k * 4 + 1);
                        auto mix = [](QRgb a, QRgb b, int wa, int wb, int div)
                        {
                            return qRgb((qRed(a) * wa + qRed(b) * wb) / div, (qGreen(a) * wa + qGreen(b) * wb) / div, (qBlue(a) * wa + qBlue(b) * wb) / div);
                        };
                        if (i < 2) want = i ? c1 : c0;
                        else if (k == 0) want = i == 2 ? palc(k * 4 + 2) : 0;
                        else if (k == 1) want = i == 2 ? mix(c0, c1, 1, 1, 2) : 0;
                        else if (k == 2) want = palc(k * 4 + i);
                        else want = i == 2 ? mix(c0, c1, 5, 3, 8) : mix(c0, c1, 3, 5, 8);
                        break;
                    }
                    }
                    if (img.pixel(x, y) != want)
                    {
                        if (all) printf("  %s: (%d,%d) %08X != %08X\n", TexNames[f - 1], x, y, (unsigned)img.pixel(x, y), (unsigned)want);
                        all = false;
                    }
                }
            CHECK(all);
            u32 off, size;
            CHECK(WBGfx::TexelBytes(td, f - 1, 3, 2, off, size) && off >= tx.Offset && off + size <= tx.Offset + tx.Size);
            fi = WBGfx::DescribeAt(td, t.data(), (u32)t.size(), off);
            CHECK(fi.Texture == f - 1 && fi.Text.find(TexNames[f - 1]) != std::string::npos);
        }
        bool c16pal = false;
        for (auto& p : td.Parts) if (p.Kind == "tex_palette" && p.Label == "palette 'c16_pl'" && p.Size == 32) c16pal = true;
        CHECK(c16pal);
    }

    // ---- damaged files: described as far as they go, never read past the end
    {
        std::vector<u8> g = Ncgr(4);
        std::vector<u8> cut(g.begin(), g.begin() + 0x50);
        WBGfx::Description gd = WBGfx::Describe(cut.data(), (u32)cut.size());
        CHECK(gd.Format == "NCGR" && gd.UnreadableFrom != 0 && gd.CharOffset + gd.CharSize <= cut.size());
        CHECK(WBGfx::DescribeAt(gd, cut.data(), (u32)cut.size(), 0x4F).Text.find("tile") != std::string::npos);
        std::vector<u8> bad = Nclr();
        bad[4] = 0;     // byte order mark
        CHECK(WBGfx::Describe(bad.data(), (u32)bad.size()).UnreadableFrom == 4);
        CHECK(!WBGfx::Describe((const u8*)"RLCN", 4).Parts.size());
        CHECK(WBGfx::Describe((const u8*)"NARC....", 8).Format.empty());
    }
    // fuzz: random byte changes and cuts; every range stays inside the input, rendering never crashes
    {
        std::vector<std::vector<u8>> seeds = {Nclr(), Ncgr(4), Ncgr(8), Nscr(), Ncer(), Nsbtx()};
        std::vector<u8> g4 = Ncgr(4);
        WBGfx::Description g4d = WBGfx::Describe(g4.data(), (u32)g4.size());
        u32 rng = 1234;
        auto next = [&rng]() { rng = rng * 1103515245u + 12345u; return rng >> 8; };
        bool inBounds = true;
        for (int it = 0; it < 3000; it++)
        {
            std::vector<u8> v = seeds[it % seeds.size()];
            for (int k = 0; k < 4; k++) v[next() % v.size()] = (u8)next();
            if (it % 7 == 0) v.resize(v.size() / 2 + next() % (v.size() / 2));
            WBGfx::Description d = WBGfx::Describe(v.data(), (u32)v.size());
            for (const WBGfx::Part& p : d.Parts)
                if ((u64)p.Offset + p.Size > v.size()) inBounds = false;
            for (const WBGfx::Texture& t : d.Textures)
                if (d.UnreadableFrom == 0 && ((u64)t.Offset + t.Size > v.size())) inBounds = false;
            for (u32 o = 0; o < v.size(); o += 7) (void)WBGfx::DescribeAt(d, v.data(), (u32)v.size(), o);
            std::vector<QRgb> pp = WBGfx::PaletteColors(d, v.data(), (u32)v.size());
            (void)WBGfx::RenderTiles(d, v.data(), (u32)v.size(), pal, 1);
            (void)WBGfx::RenderMap(d, v.data(), (u32)v.size(), g4d, g4.data(), (u32)g4.size(), pal);
            for (int c = 0; c < (int)d.Cells.size(); c++) (void)WBGfx::RenderCell(d, c, g4d, g4.data(), (u32)g4.size(), pal);
            for (int t = 0; t < (int)d.Textures.size(); t++)
                if (d.Textures[t].Width * d.Textures[t].Height <= 64 * 64) (void)WBGfx::RenderTexture(d, v.data(), (u32)v.size(), t);
        }
        CHECK(inBounds);
    }
    // the test ROM's graphics files (make_assets.py) are the same bytes as these builders: two
    // implementations of the layouts that must agree
    {
        QDir dir(QCoreApplication::applicationDirPath() + "/../tools/workbench/testrom/nitrofiles/gfx");
        std::pair<const char*, std::vector<u8>> files[] = {{"t.nclr", Nclr()}, {"t4.ncgr", Ncgr(4)}, {"t8.ncgr", Ncgr(8)},
                                                           {"t.nscr", Nscr()}, {"t.ncer", Ncer()}, {"t.nsbtx", Nsbtx()}};
        if (!dir.exists()) printf("  (skipped: no %s)\n", dir.path().toUtf8().constData());
        else
            for (auto& [name, want] : files)
            {
                QFile f(dir.filePath(name));
                QByteArray got = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
                bool same = got.size() == (int)want.size() && !memcmp(got.constData(), want.data(), want.size());
                if (!same) printf("  %s differs from the builder (%d vs %d bytes)\n", name, (int)got.size(), (int)want.size());
                CHECK(same);
            }
    }
    // raw "View as"
    {
        u8 raw[32];
        for (int i = 0; i < 32; i++) raw[i] = (u8)(i * 17);
        std::vector<QRgb> grey;
        for (int i = 0; i < 16; i++) grey.push_back(qRgb(i * 16, i * 16, i * 16));
        QImage img = WBGfx::DecodeRaw(raw, 32, 4, false, 16, grey, 0, false);
        CHECK(img.size() == QSize(16, 4) && img.pixel(0, 0) == grey[0] && img.pixel(1, 0) == grey[0] && img.pixel(2, 0) == grey[1] && img.pixel(3, 0) == grey[1]);
        CHECK(WBGfx::DecodeRaw(raw, 32, 3, false, 16, grey, 0, false).isNull());
    }
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // a test that crashes ends the run with its exit code, without a crash dialog
    SetErrorMode(0x0001 | 0x0002);     // SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
#endif
    QCoreApplication app(argc, argv);
    // named, so a crash shows which test it was in
    auto run = [](const char* name, void (*test)()) { printf("%s\n", name); fflush(stdout); test(); };
    run("TestExpr", TestExpr);
    run("TestDisasm", TestDisasm);
    run("TestInstrOps", TestInstrOps);
    run("TestNitroROM", TestNitroROM);
    run("TestNitroROMMalformed", TestNitroROMMalformed);
    run("TestOriginIrqEntry", TestOriginIrqEntry);
    run("TestApiParams", TestApiParams);
    run("TestApiConnections", TestApiConnections);
    run("TestOriginBankedPointers", TestOriginBankedPointers);
    run("TestHistoryFilterRanges", TestHistoryFilterRanges);
    run("TestTypes", TestTypes);
    run("TestKnowledgeStore", TestKnowledgeStore);
    run("TestAutoFunctions", TestAutoFunctions);
    run("TestSpanDecide", TestSpanDecide);
    run("TestKbUndoReferenced", TestKbUndoReferenced);
    run("TestKbLibrarySubsystem", TestKbLibrarySubsystem);
    run("TestKbList", TestKbList);
    run("TestArchive", TestArchive);
    run("TestBiosUnpack", TestBiosUnpack);
    run("TestBackwardLz", TestBackwardLz);
    run("TestContainers", TestContainers);
    run("TestClassify", TestClassify);
    run("TestFlowDisasm", TestFlowDisasm);
    run("TestSessionFirstRun", TestSessionFirstRun);
    run("TestHistoryFile", TestHistoryFile);
    run("TestLiteralLoads", TestLiteralLoads);
    run("TestInsnForms", TestInsnForms);
    run("TestDataObjectReach", TestDataObjectReach);
    run("TestDataUnitWidths", TestDataUnitWidths);
    run("TestDebugger", TestDebugger);
    run("TestSessionMerge", TestSessionMerge);
    run("TestOriginLabels", TestOriginLabels);
    run("TestCompare", TestCompare);
    run("TestGfx", TestGfx);
    printf("%d passed, %d failed\n", Passes, Fails);
    return Fails ? 1 : 0;
}
