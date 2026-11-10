"""Take the documentation's screenshots (docs/img/*.png) through the workbench API.

    python docs/tools/capture.py --rom path/to/atc.nds --slot 2

It starts melonDS with the workbench and the API, loads the save state, gives the struct and the
function of the guide their names (in a fresh knowledge store, so yours is not touched), then
points the window at each thing with ui.show and saves ui.capture's picture. melonDS's own
settings file is restored afterwards.

The defaults are the guide's: Air Traffic Chaos, save state 2, gUnkCtl at 02078d30. Another game
works with other addresses (see --help), but the captions describe these.

Two pictures need a person and are listed at the end: a right-click menu (a popup, which
ui.capture does not see) and the Memory view's File structure layer (no API switch for it).
The orange numbers are not drawn into the pictures: guide.html places them over each picture
with callout spans positioned in percent (style="--x:..;--y:.."), so a retaken picture may need
their positions adjusted.
"""
import argparse
import base64
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "workbench", "python"))
from dswb.client import Client  # noqa: E402

STRUCT = """struct UnkCtl {
    u32 unk00; u32 unk04; u32 unk08;
    u16 unk0c; u16 unk0e; u32 unk10;
    u8  pad14[0x24];
    u32 unk38; u32 unk3c;
    u8  pad40[8];
    u32 unk48;
};"""

MANUAL = [
    ("memory-context-menu.png",
     "Select the two bytes of unk0c in the Memory view, right-click, open Add watchpoint and take a "
     "screenshot of the menu (Win+Shift+S on Windows)."),
    ("file-structure.png",
     "Select the bytes of the NCGR at the data-origin address, turn on Layers > File structure in the "
     "Memory view and hover a part, then capture the Memory panel."),
]


def wait_for(pred, timeout, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return None


class Shooter:
    def __init__(self, c, out, dry):
        self.c, self.out, self.dry = c, out, dry
        self.saved = []

    def shot(self, name, panel=None, max_width=1600, settle=1.5, **show):
        """Point the window (ui.show's arguments), wait for the panels to fetch, save the picture."""
        if show or panel:
            self.c.ui_show(panel=panel, **show)
        time.sleep(settle)
        r = self.c.ui_capture(panel=panel, max_width=max_width)
        path = os.path.join(self.out, name)
        if not self.dry:
            with open(path, "wb") as f:
                f.write(base64.b64decode(r.png))
        self.saved.append(name)
        print("  %-26s %4dx%-4d %s%s" % (name, r.width, r.height, r.panel or "whole window",
                                         "  (still fetching: retake it)" if r.stale else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--melonds", default=os.path.join(ROOT, "build", "melonDS.exe" if os.name == "nt" else "melonDS"))
    ap.add_argument("--rom", required=True, help="the game (the guide uses Air Traffic Chaos, atc.nds)")
    ap.add_argument("--slot", type=int, default=2, help="save state slot to load (the guide uses 2)")
    ap.add_argument("--state", help="a save state file instead of a slot")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "img"))
    ap.add_argument("--port", type=int, default=47950)
    ap.add_argument("--struct", type=lambda s: int(s, 16), default=0x02078d30, help="gUnkCtl (hex)")
    ap.add_argument("--field", type=lambda s: int(s, 16), default=0x02078d3c, help="gUnkCtl.unk0c (hex)")
    ap.add_argument("--func", type=lambda s: int(s, 16), default=0x02061960, help="the function that writes it (hex)")
    ap.add_argument("--origin", type=lambda s: int(s, 16), default=0x02216a00, help="bytes loaded from a ROM file (hex)")
    ap.add_argument("--dry-run", action="store_true", help="drive the window but write no pictures")
    a = ap.parse_args()

    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    os.makedirs(a.out, exist_ok=True)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    toml = os.path.join(exedir, "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else None
    kbdir = tempfile.mkdtemp(prefix="dswb_docs_")
    env = dict(os.environ)
    if os.name == "nt" and os.path.isdir(r"C:\msys64\ucrt64\bin"):
        env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")

    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--workbench",
                             "--kb", os.path.join(kbdir, "knowledge.db"), os.path.abspath(a.rom)],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for(lambda: os.path.exists(info), 20):
            print("melonDS did not start its API (no %s)" % info)
            return 1
        time.sleep(1.0)
        c = Client.connect(info_file=info, agent="docs")
        print("loading the save state")
        c.execution_control("run_frames", count=60)
        if a.state:
            c.state_load(path=os.path.abspath(a.state))
        else:
            c.state_load(slot=a.slot)
        c.execution_control("run_frames", count=240)       # some writes, calls and coverage to show

        print("naming the guide's struct and function")
        c.kb_type_define(definition=STRUCT)
        c.kb_data_define(addr=a.struct, size=0x4c, type="UnkCtl", name="gUnkCtl")
        c.kb_rename(addr=a.func, name="UnkCtl_Update")

        s = Shooter(c, a.out, a.dry_run)
        print("pictures:")
        # use case 0: the whole window, the cursor in the struct so the Inspector has something to say
        s.shot("default-layout.png", addr=a.struct, sel_addr=a.struct, sel_size=4, panel="dock_disasm", settle=2.5)
        # use case 1
        s.shot("inspector-unkctl.png", panel="dock_inspector", max_width=900, sel_addr=a.field, sel_size=2)
        s.shot("memory-define-data.png", panel="dock_memory", max_width=1200, sel_addr=a.struct, sel_size=0x4c)
        s.shot("write-history.png", panel="dock_history", max_width=1400, sel_addr=a.struct, sel_size=0x4c)
        # use case 5 (before the break, while the recording is fresh)
        s.shot("xrefs.png", panel="dock_xrefs", max_width=1000, addr=a.field)
        s.shot("callgraph.png", panel="dock_callgraph", max_width=1000, addr=a.func + 4)
        # use case 4
        s.shot("data-origin.png", panel="dock_origin", max_width=1200, sel_addr=a.origin, sel_size=32)
        # use cases 7 and 10
        s.shot("coverage.png", panel="dock_coverage", max_width=1400)
        s.shot("recording-panel.png", panel="dock_recording", max_width=1400)
        # use case 2: a write watchpoint on the field, then the break
        c.breakpoint_set(kind="write", addr=a.field, size=2)
        c.execution_control("resume")
        if wait_for(lambda: c.status_get().state == "break", 15):
            # the break may come from another writer first: go on to the function of the guide
            for _ in range(20):
                bi = c.status_get().break_info
                if bi and a.func <= bi.pc < a.func + 0x400:
                    break
                c.execution_control("resume")
                wait_for(lambda: c.status_get().state == "break", 5)
            s.shot("break-disasm.png", panel="dock_disasm", max_width=1200, cpu="arm9")
            s.shot("callstack.png", panel="dock_callstack", max_width=1200)
            s.shot("breakpoints.png", panel="dock_breakpoints", max_width=1200)
        else:
            print("  the watchpoint did not hit in 15 s: break-disasm, callstack and breakpoints were not taken")
        c.execution_control("resume")
        # use case 3: the value scan is a person's search in the Search panel
        print("\nTake these by hand (see the guide's captions):")
        for name, how in MANUAL + [("value-scan.png", "Run a value scan in Search > Value scan (unknown, then "
                                    "increased three times 30 frames apart) and capture the Search panel.")]:
            print("  %-26s %s" % (name, how))
        c.close()
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        if toml_orig is not None:
            open(toml, "w", encoding="utf-8").write(toml_orig)
        shutil.rmtree(kbdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
