"""UI test of the Graphics panel on the test ROM: open a file, click a tile, check the selection.

    python tools/workbench/tests/ui_graphics.py [--melonds build/melonDS.exe]

Starts the workbench with DSWB_GRAPHICS="/gfx/t4.ncgr;9,9": once the game has read its files, the
Graphics panel shows t4.ncgr and clicks pixel (9, 9), which is in tile 5 (tile column 1, row 1). The
click must select that tile's 32 bytes in the file's RAM copy. The panel is captured to
build/ui_screens/graphics.png and must show the tiles' colours.

Sends no keys or mouse input; it does open a window.
"""
import argparse
import base64
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client, ApiError  # noqa: E402
from test_recording import find_manifest, read_manifest  # noqa: E402
from test_assets import png_pixels, rgb, pal_color  # noqa: E402

FAILS = []
PASSES = 0


def check(cond, what):
    global PASSES
    if cond:
        PASSES += 1
        print(f"  ok   {what}")
    else:
        FAILS.append(what)
        print(f"  FAIL {what}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--port", type=int, default=47940)
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    env["DSWB_GRAPHICS"] = "/gfx/t4.ncgr;9,9"
    kbdir = tempfile.mkdtemp(prefix="dswb_uigfx_")
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--workbench", "--kb", os.path.join(kbdir, "knowledge.db"),
                             os.path.abspath(a.rom)], cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        c = Client.connect(info)
        maddr = None
        for _ in range(40):
            maddr = maddr or find_manifest(c)
            if maddr and read_manifest(c, maddr)["ready"]:
                break
            time.sleep(0.25)
        m = read_manifest(c, maddr) if maddr else None
        check(m is not None and m["ready"], "the test ROM is running")
        time.sleep(11)          # the hook opens the file after 7 s and clicks 1.5 s later
        st = c.ui_show(panel="graphics")
        want = m["gfx_ncgr_buf"] + 0x30 + 5 * 32 if m else 0
        check(st.sel_addr == want and st.sel_size == 32,
              "clicking tile 5 selects its 32 bytes in the RAM copy: %08x+%d (want %08x+32)" % (st.sel_addr, st.sel_size, want))
        cap = c.ui_capture(panel="graphics", max_width=0)
        out = os.path.join(HERE, "..", "..", "..", "build", "ui_screens")
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "graphics.png"), "wb") as f:
            f.write(base64.b64decode(cap.png))
        w, h, px = png_pixels(cap.png)
        seen = {p[:3] for row in px for p in row}
        colours = [rgb(pal_color(v))[:3] for v in range(1, 16)]
        found = sum(1 for col in colours if col in seen)
        check(found >= 8, "the panel shows the tiles' colours (%d of 15 found in a %dx%d capture)" % (found, w, h))
    except ApiError as e:
        check(False, "API error: %s" % e)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
    print("\n%d passed, %d failed" % (PASSES, len(FAILS)))
    for f in FAILS:
        print("  FAIL " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
