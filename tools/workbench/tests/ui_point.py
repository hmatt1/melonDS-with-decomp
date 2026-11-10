"""ui.show and ui.capture: an agent points the workbench window, then sees it.

Boots melonDS with the API and no workbench window. A bad call must not open one.
The Minimal workspace leaves Write history closed, so capturing it before ui.show
is an error. Also imported by test_recording.py, which skips the closed-panel case
because that run keeps the person's current workspace.

    python tools/workbench/tests/ui_point.py --melonds build/melonDS.exe --rom tools/workbench/testrom/wbtest.nds
"""
import argparse
import base64
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client, ApiError  # noqa: E402

FAILS = []
PASSES = 0


def check(cond, what):
    global PASSES
    if cond:
        PASSES += 1
        print("  ok   %s" % what)
    else:
        FAILS.append(what)
        print("  FAIL %s" % what)


def png_info(b64):
    raw = base64.b64decode(b64)
    if not raw.startswith(b"\x89PNG\r\n\x1a\n") or len(raw) < 24:
        return raw, 0, 0
    w, h = struct.unpack(">II", raw[16:24])
    return raw, w, h


def check_ui(c, check, addr, sel_addr, closed_panel=None, out=None):
    """The window must not be open yet. `closed_panel` is a panel the workspace leaves closed."""
    refused = False
    try:
        c.read.call("ui.show", {})
    except ApiError as e:
        refused = "control endpoint" in e.message
    check(refused, "ui.show is refused on the read endpoint")
    refused = False
    try:
        c.read.call("ui.capture", {})
    except ApiError as e:
        refused = "control endpoint" in e.message
    check(refused, "ui.capture is refused on the read endpoint")

    try:
        c.ui_show()
        check(False, "ui.show with nothing to show is refused")
    except ApiError as e:
        check("what to show" in e.message, "ui.show with nothing to show is refused: %s" % e.message)
    try:
        c.ui_show(panel="not a panel")
        check(False, "ui.show refuses an unknown panel")
    except ApiError as e:
        check("unknown panel" in e.message, "ui.show refuses an unknown panel: %s" % e.message)
    try:
        c.ui_capture()
        check(False, "ui.capture before the window is open is refused")
    except ApiError as e:
        check("not open" in e.message, "ui.capture before the window is open is refused: %s" % e.message)

    shown = c.ui_show(addr=addr, sel_size=16, panel="disasm", cpu="arm9", note="look at the ARM function")
    check(shown.cpu == "arm9" and shown.addr == addr and shown.sel_addr == addr and shown.sel_size == 16
          and shown.panel == "dock_disasm" and not shown.follow_pc,
          "ui.show goes to %08x, selects 16 bytes, opens the disassembly, and stops following the PC: cpu %s addr %08x sel %08x+%d %s follow %s"
          % (addr, shown.cpu, shown.addr, shown.sel_addr, shown.sel_size, shown.panel, shown.follow_pc))
    # the first show applies its workspace on the next turn
    time.sleep(0.5)

    if closed_panel:
        try:
            c.ui_capture(panel=closed_panel)
            check(False, "ui.capture of a closed panel is refused")
        except ApiError as e:
            check("closed" in e.message, "ui.capture of a closed panel is refused: %s" % e.message)

    cursor = shown.addr
    hist = c.ui_show(panel="Write history", sel_addr=sel_addr, sel_size=16)
    check(hist.addr == cursor and hist.sel_addr == sel_addr and hist.sel_size == 16 and hist.panel == "dock_history"
          and not hist.follow_pc,
          "a selection in Write history leaves the cursor at %08x: cursor %08x sel %08x+%d"
          % (cursor, hist.addr, hist.sel_addr, hist.sel_size))

    shot = c.ui_capture()
    raw, w, h = png_info(shot.png)
    check(w == shot.width and h == shot.height and w >= 200 and h >= 150 and len(raw) > 8000 and not shot.stale,
          "ui.capture is a PNG of the window, %d x %d, %d bytes, stale %s" % (w, h, len(raw), shot.stale))
    panel = c.ui_capture(panel="disasm")
    praw, pw, ph = png_info(panel.png)
    check(panel.panel == "dock_disasm" and pw >= 160 and ph >= 90 and (pw, ph) != (w, h) and len(praw) > 2000,
          "ui.capture of the disassembly is its own picture, %d x %d against the window's %d x %d" % (pw, ph, w, h))
    small = c.ui_capture(max_width=320)
    check(small.width <= 320 and small.width >= 200 and (w <= 320 or small.width == 320) and small.height < h,
          "max_width 320 scales the window from %d x %d to %d x %d" % (w, h, small.width, small.height))

    arm7 = c.ui_show(cpu="arm7")
    check(arm7.cpu == "arm7" and arm7.addr != cursor and not arm7.follow_pc and arm7.panel == "",
          "ui.show of the ARM7 moves to that CPU's PC (%08x) and does not follow it" % arm7.addr)

    if out and raw:
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, "window.png"), "wb").write(raw)
        open(os.path.join(out, "disasm.png"), "wb").write(praw)
        print("  screenshots in %s" % out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--port", type=int, default=47920)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    rom = os.path.abspath(a.rom)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    env["DSWB_WORKSPACE"] = "Minimal"
    toml = os.path.join(exedir, "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), rom], cwd=exedir, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        else:
            print("melonDS didn't start its API")
            return 1
        time.sleep(0.5)
        c = Client.connect(info)
        for _ in range(50):
            if c.status_get().rom_gamecode:
                break
            time.sleep(0.1)
        print("pointing the window")
        out = os.path.join(exedir, "ui_point")
        check_ui(c, check, 0x02000000, 0x02000100, closed_panel="history", out=out)
        c.close()
    finally:
        if not a.keep:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        if os.path.exists(toml) or toml_orig:
            open(toml, "w", encoding="utf-8").write(toml_orig)
    print("\n%d passed, %d failed" % (PASSES, len(FAILS)))
    for f in FAILS:
        print("  FAILED:", f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
