"""Tests for dswb.vtables that need no emulator: a small module with two related vtables, a callback
table, and one indirect call per instruction form.

  python tools/workbench/tests/test_vtables.py
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY = os.path.abspath(os.path.join(HERE, "..", "python"))
sys.path.insert(0, PY)

from dswb import dsd, vtables  # noqa: E402

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + ("" if cond or not detail else ": " + str(detail)))


BASE = 0x02000000
A_DTOR, A_F, B_F, BASE_G, CTOR_A = 0x02000100, 0x02000110, 0x02000120, 0x02000130, 0x02000200
ARM_CALLER, THUMB_CALLER, CB_CALLER, FAR_CALLER = 0x02000300, 0x02000400, 0x02000500, 0x02000600
VT_A, VT_B, CALLBACKS = 0x02001008, 0x0200101c, 0x02001030

SYMBOLS = """\
A_dtor kind:function(arm,size=0x10) addr:0x02000100
A_f kind:function(arm,size=0x10) addr:0x02000110
B_f kind:function(arm,size=0x10) addr:0x02000120
Base_g kind:function(thumb,size=0x10) addr:0x02000130
A_ctor kind:function(arm,size=0x20) addr:0x02000200
ArmCaller kind:function(arm,size=0x40) addr:0x02000300
ThumbCaller kind:function(thumb,size=0x20) addr:0x02000400
CallbackCaller kind:function(arm,size=0x20) addr:0x02000500
FarCaller kind:function(arm,size=0x40) addr:0x02000600
gNamedTable kind:data(any) addr:0x0200101c
"""
DELINKS = """\
    .text       start:0x02000000 end:0x02001000 kind:code align:32
    .data       start:0x02001000 end:0x02002000 kind:data align:32
"""


def build(tmp):
    img = bytearray(0x2000)

    def w32(addr, v):
        struct.pack_into("<I", img, addr - BASE, v)

    def w16(addr, v):
        struct.pack_into("<H", img, addr - BASE, v)

    relocs = []
    for start, funcs in ((VT_A, [A_DTOR, A_F, BASE_G]), (VT_B, [A_DTOR, B_F, BASE_G]), (CALLBACKS, [A_F, B_F])):
        for k, f in enumerate(funcs):
            w32(start + 4 * k, f | (1 if f == BASE_G else 0))
            relocs.append("from:0x%08x kind:load to:0x%08x module:main" % (start + 4 * k, f))
    w32(CALLBACKS - 4, 0x12345678)                  # no zero header in front of the callback table
    w32(CTOR_A + 0x1c, VT_A)                        # the constructor's literal pool
    relocs.append("from:0x%08x kind:load to:0x%08x module:main" % (CTOR_A + 0x1c, VT_A))
    # ldr r1, [r0]; ldr r2, [r1, #4]; blx r2
    w32(ARM_CALLER, 0xE5901000); w32(ARM_CALLER + 4, 0xE5912004); w32(ARM_CALLER + 8, 0xE12FFF32)
    # ldr r1, [r0]; ldr r3, [r1, #8]; blx r3
    w16(THUMB_CALLER, 0x6801); w16(THUMB_CALLER + 2, 0x688B); w16(THUMB_CALLER + 4, 0x4798)
    # ldr r2, [pc, #4]; blx r2: a function pointer constant
    w32(CB_CALLER, 0xE59F2004); w32(CB_CALLER + 4, 0xE12FFF32)
    # ldr r12, [r4]; ldr r12, [r12, #4]; cmp r0, #0; mov r1, r5; mov lr, pc; bx r12
    w32(FAR_CALLER, 0xE594C000); w32(FAR_CALLER + 4, 0xE59CC004); w32(FAR_CALLER + 8, 0xE3500000)
    w32(FAR_CALLER + 12, 0xE1A01005); w32(FAR_CALLER + 16, 0xE1A0E00F); w32(FAR_CALLER + 20, 0xE12FFF1C)

    cfg = os.path.join(tmp, "dsd-config", "arm9")
    ext = os.path.join(tmp, "extract", "arm9")
    os.makedirs(cfg)
    os.makedirs(ext)
    for name, text in (("symbols.txt", SYMBOLS), ("relocs.txt", "\n".join(relocs) + "\n"), ("delinks.txt", DELINKS)):
        with open(os.path.join(cfg, name), "w", encoding="utf-8") as f:
            f.write(text)
    with open(os.path.join(ext, "arm9.bin"), "wb") as f:
        f.write(img)
    return os.path.join(tmp, "dsd-config"), os.path.join(tmp, "extract")


def main():
    tmp = tempfile.mkdtemp(prefix="wb_vt_")
    try:
        cfg, ext = build(tmp)
        mods = dsd.load(cfg)
        images = vtables.load_images(ext, mods)
        tables = vtables.find_tables(mods, images)
        by = {t.addr: t for t in tables}
        check("three runs of function pointers", sorted(by) == [VT_A, VT_B, CALLBACKS], [hex(a) for a in by])
        check("the vtables have the two-zero header, the callback table does not",
              by[VT_A].header and by[VT_B].header and not by[CALLBACKS].header and not by[CALLBACKS].candidate)
        check("slots hold the functions, without the Thumb bit", by[VT_A].slots == [A_DTOR, A_F, BASE_G], by[VT_A].slots)
        check("the function whose literal pool holds the table is its constructor",
              by[VT_A].constructors == [("main", CTOR_A, CTOR_A + 0x1c)] and not by[VT_B].constructors, by[VT_A].constructors)

        img = images["main"]
        d = vtables.decode_site(img, ARM_CALLER + 8, False, ARM_CALLER)
        check("ARM: blx through [reg, #4] is slot 1, table pointer at offset 0",
              d["shape"] == "slot" and d["slot_offset"] == 4 and d.get("vptr_offset") == 0, d)
        d = vtables.decode_site(img, THUMB_CALLER + 4, True, THUMB_CALLER)
        check("Thumb: blx through [reg, #8] is slot 2", d["shape"] == "slot" and d["slot_offset"] == 8 and d.get("vptr_offset") == 0, d)
        d = vtables.decode_site(img, CB_CALLER + 4, False, CB_CALLER)
        check("a register loaded from the literal pool is not a virtual call", d["shape"] == "literal" and "slot_offset" not in d, d)
        d = vtables.decode_site(img, FAR_CALLER + 20, False, FAR_CALLER)
        check("ARM: mov lr, pc; bx reg with other instructions between the load and the call",
              d["shape"] == "slot" and d["slot_offset"] == 4 and d.get("vptr_offset") == 0, d)
        check("an indirect call is told from a direct one",
              vtables.is_indirect_call(0xE12FFF32, False) and vtables.is_indirect_call(0x4798, True)
              and not vtables.is_indirect_call(0xEB000010, False) and not vtables.is_indirect_call(0xF000, True))

        # what sessions would have recorded: the ARM site reaches both classes' method, the Thumb one the shared one
        rt = vtables.Runtime(mods, images, tables)
        rt.edges = {("main", ARM_CALLER + 8, A_F): 5, ("main", ARM_CALLER + 8, B_F): 3, ("main", THUMB_CALLER + 4, BASE_G): 7,
                    ("main", CB_CALLER + 4, A_F): 2, ("main", ARM_CALLER + 8, 0xFFFF06D8): 9, ("main", ARM_CALLER, A_F): 1}
        by[VT_A].readers[1].add(ARM_CALLER + 4)         # the load of slot 1, just before the call
        by[VT_B].readers[1].add(0x02000800)             # some other reader: too far from the call
        rep = vtables.build_report(mods, images, tables, rt)
        sites = {s["addr"]: s for s in rep["sites"]}
        check("only blx/bx to a register is an indirect site", sorted(sites) == [ARM_CALLER + 8, THUMB_CALLER + 4, CB_CALLER + 4],
              [hex(a) for a in sites])
        arm = {t["callee"]: t for t in sites[ARM_CALLER + 8]["targets"]}
        check("a call into the BIOS (an interrupt older builds recorded as a call) is not a target", sorted(arm) == [A_F, B_F])
        check("decoding ties each target to its class's table",
              arm[A_F]["by_decode"] == [{"module": "main", "table": VT_A, "slot": 1}]
              and arm[B_F]["by_decode"] == [{"module": "main", "table": VT_B, "slot": 1}], arm)
        check("a reader just before the call ties it; a far one does not",
              arm[A_F]["by_readers"] == [{"module": "main", "table": VT_A, "slot": 1}] and arm[B_F]["by_readers"] == [], arm)
        th = sites[THUMB_CALLER + 4]["targets"][0]
        check("a method both classes share is tied to both tables", [r["table"] for r in th["by_decode"]] == [VT_A, VT_B], th)
        cb = sites[CB_CALLER + 4]["targets"][0]
        check("a callback through a constant is tied to no table, though its target sits in one",
              cb["in_slot"] and cb["by_decode"] == [] and sites[CB_CALLER + 4]["shape"] == "literal", cb)
        s = rep["summary"]["main"]
        check("summary counts", s["runs"] == 3 and s["candidates"] == 2 and s["indirect_sites"] == 3 and s["sites_tied_by_decode"] == 2
              and s["sites_tied_by_readers"] == 1 and s["tables_confirmed_by_readers"] == 1 and s["sites_with_several_targets"] == 1, s)
        ta = next(t for t in rep["tables"] if t["addr"] == VT_A)
        check("tables sharing slots are listed as related",
              ta["related"] == [{"module": "main", "addr": VT_B, "same_slots": 2, "slots": 3}], ta["related"])
        check("a table lists its call sites and slot readers",
              ta["sites_by_readers"] == [{"module": "main", "addr": ARM_CALLER + 8}] and ta["slots"][1]["readers"] == [ARM_CALLER + 4]
              and ta["confirmed"], ta)

        lab = vtables.labels_doc(mods, tables)["labels"]
        check("labels: candidate tables dsd has not named", lab == [{"addr": "0x%08x" % VT_A, "name": "vt_%08x" % VT_A, "cpu": "arm9", "overlay": -1}], lab)

        out = os.path.join(tmp, "report.json")
        p = subprocess.run([sys.executable, "-m", "dswb.vtables", "--dsd", cfg, "--extract", ext, "-o", out],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=dict(os.environ, PYTHONPATH=PY))
        check("the command writes the static report", p.returncode == 0 and os.path.exists(out) and b"2 candidates" in p.stdout,
              p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed" % (len(passed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
