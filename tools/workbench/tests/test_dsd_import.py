"""Tests for dswb.dsd and dswb.dsd_import that need no emulator: a small dsd configuration with a
static module, ITCM and two overlays at one address is converted to the import JSON.

  python tools/workbench/tests/test_dsd_import.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY = os.path.abspath(os.path.join(HERE, "..", "python"))
sys.path.insert(0, PY)

from dswb import dsd, dsd_import  # noqa: E402

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + ("" if cond or not detail else ": " + str(detail)))


FILES = {
    "arm9/symbols.txt": """\
Main_Entry kind:function(arm,size=0x2c) addr:0x02000800
func_02000900 kind:function(thumb,size=0x6) addr:0x02000900 // alternatives: A, B
func_02000a00 kind:function(arm,size=0x10) addr:0x02000b00
gTable kind:data(any) addr:0x020d0000
data_020d0010 kind:data(word) addr:0x020d0010
gCount kind:bss addr:0x020e0000
_d2f kind:label(arm) addr:0x02000810
_d_dtof kind:label(arm) addr:0x02000810
""",
    "arm9/relocs.txt": """\
from:0x02000804 kind:arm_call to:0x02000900 module:main
from:0x02000808 kind:arm_call_thumb to:0x020ffe00 module:overlay(3)
from:0x0200080c kind:thumb_call_arm to:0x020ffe10 module:overlays(2,3)
from:0x02000810 kind:arm_branch to:0x02000b00 module:main
from:0x02000814 kind:load to:0x01ff8000 module:itcm
from:0x02000818 kind:link_time_const to:0x00000004 module:main
""",
    "arm9/delinks.txt": """\
    .text       start:0x02000000 end:0x020d0000 kind:code align:32
    .data       start:0x020d0000 end:0x020e0000 kind:data align:32

src/a.o:
    .text       start:0x02000800 end:0x02000900
""",
    "arm9/itcm/symbols.txt": "OS_IrqHandler kind:function(arm,size=0x68) addr:0x01ff8000\n",
    "arm9/itcm/relocs.txt": "",
    "arm9/overlays/ov002/symbols.txt": "Menu_Init kind:function(thumb,size=0x20) addr:0x020ffe00\n",
    "arm9/overlays/ov002/relocs.txt": "from:0x020ffe04 kind:thumb_call to:0x020ffe00 module:overlay(2)\n",
    "arm9/overlays/ov003/symbols.txt": "Race_Init kind:function(arm,size=0x40) addr:0x020ffe00\n"
                                       "func_ov003_020ffe40 kind:function(arm,size=0x8) addr:0x020ffe40\n"
                                       "data_ov003_02121c68 kind:data(any) addr:0x02121c68\n",
    "arm9/overlays/ov003/relocs.txt": "from:0x020ffe20 kind:arm_call to:0x02000800 module:main\n"
                                      "from:0x020ffe24 kind:load to:0x020ffe00 module:overlay(3)\n",
}


def main():
    tmp = tempfile.mkdtemp(prefix="wb_dsd_")
    try:
        cfg = os.path.join(tmp, "dsd-config")
        for rel, text in FILES.items():
            p = os.path.join(cfg, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)

        mods = dsd.load(cfg)
        names = [(m.name, m.overlay) for m in mods]
        check("modules: main, itcm and both overlays", names == [("main", -1), ("itcm", -1), ("ov002", 2), ("ov003", 3)], names)
        check("the arm9 folder is accepted too", [m.name for m in dsd.load(os.path.join(cfg, "arm9"))] == [n for n, _ in names])
        main_mod = mods[0]
        f1 = main_mod.symbols[1]
        check("a symbol line with a trailing comment parses", f1.kind == "function" and f1.mode == "thumb" and f1.size == 6 and f1.addr == 0x02000900, f1)
        check("placeholder names are told apart by their own address",
              f1.placeholder and not main_mod.symbols[2].placeholder and not main_mod.symbols[0].placeholder)
        sec = main_mod.section(".data")
        check("sections come from the top of delinks.txt only", len(main_mod.sections) == 2 and sec and sec.start == 0x020d0000 and sec.kind == "data",
              main_mod.sections)
        r = main_mod.relocs[2]
        check("an undecided overlay keeps both ids", r.to_module == "overlays" and r.to_overlays == (2, 3), r)

        doc = dsd_import.convert(mods)
        fn = {(f["addr"], f["overlay"]): f for f in doc["functions"]}
        check("every function is exported", len(doc["functions"]) == 7, len(doc["functions"]))
        check("an overlay's placeholder names are recognised", "name" not in fn[("0x020ffe40", 3)] and doc["counts"]["placeholder_labels"] == 2)
        check("two overlays at one address stay apart",
              fn[("0x020ffe00", 2)]["name"] == "Menu_Init" and fn[("0x020ffe00", 2)]["mode"] == "thumb"
              and fn[("0x020ffe00", 3)]["name"] == "Race_Init" and fn[("0x020ffe00", 3)]["size"] == 0x40)
        check("a function dsd has not named is exported without a name key, so the store keeps its own",
              "name" not in fn[("0x02000900", -1)] and fn[("0x02000b00", -1)]["name"] == "func_02000a00")
        cov = {(c["module"], c["overlay"]): (c["start"], c["end"]) for c in doc["covers"]}
        check("each module with sections is a covered range; one without (ITCM here) is not",
              cov == {("main", -1): ("0x02000000", "0x020e0000")}, cov)
        lab = [(l["addr"], l["name"]) for l in doc["labels"]]
        check("labels: named data, bss and the first label at an address",
              lab == [("0x020d0000", "gTable"), ("0x020e0000", "gCount"), ("0x02000810", "_d2f")], lab)
        check("the second label at an address and the unnamed data are reported",
              len(doc["skipped"]["duplicate_address"]) == 1 and doc["counts"]["placeholder_labels"] == 2, doc["skipped"])
        x = {x["from"]: x for x in doc["xrefs"]}
        check("call, jump and data_ref kinds", x["0x02000804"]["kind"] == "call" and x["0x02000810"]["kind"] == "jump"
              and x["0x02000814"]["kind"] == "data_ref")
        check("a reference into an overlay names it", x["0x02000808"]["to_overlay"] == 3 and x["0x02000808"]["from_overlay"] == -1)
        check("a reference out of an overlay names it", x["0x020ffe20"]["from_overlay"] == 3 and x["0x020ffe20"]["to_overlay"] == -1)
        check("a reference inside an overlay names both ends", x["0x020ffe24"]["from_overlay"] == 3 and x["0x020ffe24"]["to_overlay"] == 3
              and x["0x020ffe04"]["from_overlay"] == 2 and x["0x020ffe04"]["to_overlay"] == 2)
        check("an undecided overlay and a link-time constant are left out",
              "0x0200080c" not in x and "0x02000818" not in x and len(doc["skipped"]["ambiguous_overlay"]) == 1
              and doc["counts"]["link_time_const"] == 1)
        check("converting twice gives the same document", dsd_import.convert(dsd.load(cfg)) == doc)

        # what an import would do to a store
        def sf(addr, name, author_kind, size=0x2c, mode="arm", overlay=-1, author=None):
            return {"addr": addr, "overlay": overlay, "size": size, "mode": mode, "name": name,
                    "author": author or {"system": "workbench", "agent": "matcher", "person": "me"}[author_kind], "author_kind": author_kind}
        store = [sf(0x02000800, "Wrong_Name", "agent", size=0x80),       # dsd names it Main_Entry, size 0x2c
                 sf(0x02000900, "MyName", "person", size=6, mode="thumb"),  # dsd has only a placeholder: kept
                 sf(0x02000b00, "func_02000a00", "person", size=0x10),      # already as dsd has it: nothing
                 sf(0x02000804, "sub_02000804", "system", size=8),          # automatic, inside Main_Entry: removed
                 sf(0x02000c00, "", "agent", size=8),                       # an agent's, not in dsd: a conflict
                 sf(0x02000400, "sub_02000400", "system", size=8, overlay=-1),  # automatic, matches nothing: removed
                 sf(0x020f0000, "sub_020f0000", "system", size=8),          # outside every covered range: left alone
                 sf(0x02000800, "Other_Overlay", "person", overlay=3),      # not in a covered range (no ov003 sections)
                 sf(0x02000b80, "Gone", "agent", author="dsd"),             # an earlier dsd import's, no longer listed: removed
                 sf(0x02000d00, "", "agent", author="dsd")]                 # the same, with no name
        rv = dsd_import.review(doc, store)
        rep = [(e["addr"], e["name"], e["dsd_name"], e["dsd_size"]) for e in rv["replaced"]]
        check("review: the agent's name and size that dsd replaces", rep == [("0x02000800", "Wrong_Name", "Main_Entry", 0x2c)], rep)
        check("review: a name only the workbench has is kept and listed",
              [e["name"] for e in rv["workbench_names"]] == ["MyName"] and not any(e["name"] == "MyName" for e in rv["replaced"]), rv["workbench_names"])
        check("review: an agent's function dsd does not list is a conflict", [e["addr"] for e in rv["conflicts"]] == ["0x02000c00"], rv["conflicts"])
        check("review: automatic functions in a covered range that dsd lacks are removed, others left",
              sorted(e["addr"] for e in rv["removed"]) == ["0x02000400", "0x02000804", "0x02000b80", "0x02000d00"], rv["removed"])
        check("review: rows of the import's own author are dsd's, never a person's or agent's",
              not any(e["author"] == "dsd" for k in ("replaced", "workbench_names", "conflicts") for e in rv[k]))
        text = dsd_import.review_text(rv)
        check("review text says each", "change:" in text and "only in the workbench" in text and "not in dsd's list" in text
              and "4 functions are not in dsd's list" in text and "0x02000b80" in text, text)

        out = os.path.join(tmp, "out.json")
        env = dict(os.environ, PYTHONPATH=PY)
        p = subprocess.run([sys.executable, "-m", "dswb.dsd_import", cfg, "-o", out], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        text = p.stdout.decode("utf-8", "replace")
        ok = p.returncode == 0 and os.path.exists(out)
        check("the command writes the JSON", ok, p.stderr.decode("utf-8", "replace"))
        if ok:
            with open(out, "r", encoding="utf-8") as f:
                check("the written file is the converted document", json.load(f) == doc)
        check("the command says what it left out", "could not place in one overlay" in text and "functions 7 (5 named)" in text, text)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed" % (len(passed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
