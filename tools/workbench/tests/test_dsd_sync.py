"""Tests for dswb.dsd_sync that need no emulator: names from a stand-in store go into a small
dsd configuration, the names file records them, and a failing build check puts everything back.

  python tools/workbench/tests/test_dsd_sync.py
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "python")))

from dswb import dsd, dsd_sync  # noqa: E402
from dswb.dsd_sync import Entry  # noqa: E402

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + ("" if cond or not detail else ": " + str(detail)))


MAIN = (b"Known_Func kind:function(arm,size=0x20) addr:0x02000000\r\n"
        b"func_02000020 kind:function(thumb,size=0x10) addr:0x02000020 // alternatives: A, B\r\n"
        b"func_02000030 kind:function(arm,size=0x10) addr:0x02000030\r\n"
        b"func_02000040 kind:function(arm,size=0x10) addr:0x02000040\r\n"
        b"_d2f kind:label(arm) addr:0x02000040\r\n"
        b"data_02000100 kind:data(any) addr:0x02000100\r\n"
        b"bss_02000200 kind:bss addr:0x02000200\r\n")
OV = b"func_ov003_020ffe00 kind:function(arm,size=0x3c) addr:0x020ffe00\n"


def make(tmp):
    cfg = os.path.join(tmp, "proj", "dsd-config", "arm9")
    os.makedirs(os.path.join(cfg, "overlays", "ov003"))
    for rel, data in (("symbols.txt", MAIN), ("relocs.txt", b""), ("overlays/ov003/symbols.txt", OV), ("overlays/ov003/relocs.txt", b"")):
        with open(os.path.join(cfg, rel), "wb") as f:
            f.write(data)
    return os.path.join(tmp, "proj", "dsd-config")


def store():
    return [Entry("main", 0x02000020, "function", "Car_Update", "me"),
            Entry("main", 0x02000000, "function", "My_Other_Name", "me"),       # dsd's own name: a conflict
            Entry("main", 0x02000030, "function", "Known_Func", "agent-1"),     # used elsewhere
            Entry("main", 0x02000040, "function", "bad name", "me"),            # not a symbol
            Entry("main", 0x02000100, "data", "gCars", "me"),
            Entry("main", 0x02000200, "data", "gTimer", "me"),                  # a bss symbol
            Entry("main", 0x02000300, "data", "gNowhere", "me"),                # no symbol there
            Entry("ov003", 0x020ffe00, "function", "Race_Start", "me")]


def main():
    tmp = tempfile.mkdtemp(prefix="wb_sync_")
    try:
        config = make(tmp)
        project = os.path.join(tmp, "proj")
        npath = dsd_sync.names_path(config)
        check("the names file sits next to dsd-config", npath == os.path.join(project, "workbench_names.tsv"), npath)

        files = dsd_sync.load_symbols(config)
        p = dsd_sync.plan(files, [], store())
        wrote = {(e.module, e.addr): (old, e.name) for e, old in p.write}
        check("placeholders get the workbench names: function, data, bss, and in an overlay",
              wrote == {("main", 0x02000020): ("func_02000020", "Car_Update"), ("main", 0x02000100): ("data_02000100", "gCars"),
                        ("main", 0x02000200): ("bss_02000200", "gTimer"), ("ov003", 0x020ffe00): ("func_ov003_020ffe00", "Race_Start")}, wrote)
        check("a name dsd has from elsewhere stays, as a conflict", [(e.name, n) for e, n in p.conflicts] == [("My_Other_Name", "Known_Func")], p.conflicts)
        check("an invalid name and a name used elsewhere are not written",
              sorted(e.name for e, _ in p.invalid) == ["Known_Func", "bad name"], p.invalid)
        check("an address with no symbol of that kind is listed", [e.name for e in p.no_symbol] == ["gNowhere"], p.no_symbol)
        text = dsd_sync.plan_text(p)
        check("the plan says what it does", "4 names to write" in text and "func_02000020 -> Car_Update" in text and "conflicts" in text, text)

        ok = dsd_sync.write(files, p, [], npath, project, build_cmd=None)
        raw = open(os.path.join(config, "arm9", "symbols.txt"), "rb").read()
        check("written, the rest of each line and the CRLF line ends kept",
              ok and b"Car_Update kind:function(thumb,size=0x10) addr:0x02000020 // alternatives: A, B\r\n" in raw
              and b"gTimer kind:bss addr:0x02000200\r\n" in raw and raw.count(b"\r\n") == MAIN.count(b"\r\n"), raw)
        check("the overlay's file too", b"Race_Start kind:function" in open(os.path.join(config, "arm9", "overlays", "ov003", "symbols.txt"), "rb").read())
        names = open(npath, encoding="utf-8").read()
        check("the names file records what was written",
              "main\t0x02000020\tfunction\tCar_Update" in names and "ov003\t0x020ffe00\tfunction\tRace_Start" in names and names.count("\n") == 6, names)
        check("dsd reads the renamed file", any(s.name == "Car_Update" for s in dsd.load(config)[0].symbols))

        # again: nothing to do
        files = dsd_sync.load_symbols(config)
        recorded = dsd_sync.read_names(npath)
        p = dsd_sync.plan(files, recorded, recorded + store())
        check("a second run writes nothing", not p.write and len(p.already) == 4, (p.write, p.already))

        # a name the workbench wrote can be changed from the workbench; one dsd owns cannot
        p = dsd_sync.plan(files, recorded, recorded + [Entry("main", 0x02000020, "function", "Car_Tick", "me")])
        check("a name from the names file is renamed again", [(old, e.name) for e, old in p.write] == [("Car_Update", "Car_Tick")], p.write)
        dsd_sync.write(files, p, recorded, npath, project, build_cmd=None)
        recorded = dsd_sync.read_names(npath)
        check("and the names file follows", [e.name for e in recorded if e.addr == 0x02000020] == ["Car_Tick"], recorded)

        # dsd-config made again from scratch: the names file puts the names back
        for rel, data in (("symbols.txt", MAIN), ("overlays/ov003/symbols.txt", OV)):
            with open(os.path.join(config, "arm9", rel), "wb") as f:
                f.write(data)
        files = dsd_sync.load_symbols(config)
        p = dsd_sync.plan(files, recorded, recorded)
        check("from scratch, the names file alone puts its names back",
              sorted(e.name for e, _ in p.write) == ["Car_Tick", "Race_Start", "gCars", "gTimer"], p.write)

        # a failing build check puts every file back
        before = {n: open(f.path, "rb").read() for n, f in files.items()}
        names_before = open(npath, "rb").read()
        ok = dsd_sync.write(files, p, recorded, npath, project, build_cmd='"%s" -c "import sys; sys.exit(1)"' % sys.executable)
        check("a failing build check puts every file back",
              not ok and all(open(f.path, "rb").read() == before[n] for n, f in files.items()) and open(npath, "rb").read() == names_before)
        files = dsd_sync.load_symbols(config)
        p = dsd_sync.plan(files, recorded, recorded)
        ok = dsd_sync.write(files, p, recorded, npath, project, build_cmd='"%s" -c "print(\'build ok\')"' % sys.executable)
        check("a passing build check keeps them", ok and b"Car_Tick" in open(os.path.join(config, "arm9", "symbols.txt"), "rb").read())
        files = dsd_sync.load_symbols(config)
        check("no build.ninja: the check is skipped, and says so", dsd_sync.build(project, None)[0] and "not checked" in dsd_sync.build(project, None)[1])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed" % (len(passed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
