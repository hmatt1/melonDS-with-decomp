"""Take the documentation's screenshots (docs/img/*.png) with the workbench's own test ROM.

    python docs/tools/capture.py [--melonds build/melonDS.exe] [--rom tools/workbench/testrom/wbtest.nds]

The test ROM (tools/workbench/testrom, built with devkitPro) knows its own addresses: it publishes
them in a manifest in RAM, which this script finds. It starts melonDS with the workbench and the API
(recording, write history and data origin on from boot) and a throwaway knowledge store, so yours
is not touched. It names the guide's struct, points the window at each thing with ui.show and saves
ui.capture's picture. Two pictures need input the API can't give a panel (a right-click menu, and the
Search panel's value scan): those are made with mouse clicks, keystrokes and a screen grab, so leave
the mouse and keyboard alone while it runs. melonDS's own settings file is restored afterwards.

The addresses the guide quotes are printed at the end and written to docs/tools/capture.json. The
orange numbers are not drawn into the pictures: docs/index.html places them over each picture with
callout spans positioned in percent (style="--x:..;--y:.."), so a retaken picture may need them moved.
Windows only (the keystrokes and the screen grab use the Windows API).
"""
import argparse
import base64
import ctypes
import ctypes.wintypes as wt
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "workbench", "python"))
from dswb.client import Client  # noqa: E402

# the test ROM's CarParams (testrom/source/main.c), as a person who doesn't know it yet would type it
STRUCT = """struct UnkCar {
    u32 unk00; u32 unk04; u32 unk08; u32 unk0c; u32 unk10;
    u32 unk14; u32 unk18; u32 unk1c; u32 unk20; u32 unk24;
};"""
STRUCT_SIZE = 0x28

# the manifest's words after its "DSWBTEST" magic (testrom/source/main.c, struct Manifest)
MANIFEST = """self version arm_func arm_func_end thumb_func thumb_func_end thumb_leaf thumb_leaf_end
data_table data_table_end dma_dst dma_size payload_buf payload_size packed_buf packed_size unpacked_buf
unpacked_size car car_speed_offset car_grip_offset car_class_offset write_target writer_func main_func
frame status ready vfs_buf vfs_size tail_target tail_target_end tail_caller tail_caller_end decoded_buf
decoded_size car_ratio_offset car_root_offset kb_target kb_target_end kb_caller kb_caller_end cond_caller
cond_caller_end car_addr_like_offset plain_buf plain_size arm7_box car_mix_offset car_total_offset
narc_title_buf narc_title_size narc_model_buf narc_model_size nested_buf nested_size deep_buf deep_size
carc_buf carc_size sseq_buf sseq_size gfx_nclr_buf gfx_nclr_size gfx_ncgr_buf gfx_ncgr_size gfx_nscr_buf
gfx_nscr_size gfx_ncer_buf gfx_ncer_size gfx_btx_buf gfx_btx_size""".split()

user32 = ctypes.windll.user32
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)   # window rectangles and the screen grab in the same pixels
except Exception:
    pass

VK = {"down": 0x28, "right": 0x27, "up": 0x26, "tab": 0x09, "enter": 0x0D, "esc": 0x1B, "apps": 0x5D,
      "ctrl": 0x11, "shift": 0x10, "alt": 0x12, "home": 0x24, "end": 0x23}


def key(vk, mods=(), wait=0.12):
    for m in mods:
        user32.keybd_event(VK[m], 0, 0, 0)
    user32.keybd_event(vk, 0, 0, 0)
    user32.keybd_event(vk, 0, 2, 0)
    for m in reversed(mods):
        user32.keybd_event(VK[m], 0, 2, 0)
    time.sleep(wait)


def press(name, mods=(), wait=0.12):
    key(VK[name], mods, wait)


def type_text(text):
    for ch in text:
        r = user32.VkKeyScanW(ord(ch))
        key(r & 0xFF, ("shift",) if (r >> 8) & 1 else (), 0.05)


def windows_of(pid):
    """Visible top-level windows of a process: (hwnd, class, title, rect)."""
    out = []
    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            cls = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(h, cls, 256)
            title = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(h, title, 256)
            r = wt.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            out.append((h, cls.value, title.value, (r.left, r.top, r.right, r.bottom)))
        return True
    user32.EnumWindows(cb, 0)
    return out


def focus(hwnd):
    user32.ShowWindow(hwnd, 9)
    # Windows lets a process take the foreground after it has seen input: an Alt tap counts
    user32.keybd_event(VK["alt"], 0, 0, 0)
    user32.keybd_event(VK["alt"], 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)


def rect(window, name="", value=""):
    """Screen rectangle of a control, by its name or the value it shows (rect.ps1), or None."""
    args = ["-Name", name] if name else ["-Value", value]
    out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(HERE, "rect.ps1"),
                          "-Window", window] + args, capture_output=True, text=True).stdout.split()
    return tuple(map(int, out[:4])) if len(out) >= 4 else None


def click_at(x, y, right=False):
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.15)
    down, up = (0x0008, 0x0010) if right else (0x0002, 0x0004)
    user32.mouse_event(down, 0, 0, 0, 0)
    user32.mouse_event(up, 0, 0, 0, 0)
    time.sleep(0.5)


def click(r):
    click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)


def wait_for(pred, timeout, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return None


def find_manifest(c):
    """Run the game until the test ROM says it is ready; its manifest as a dict."""
    for _ in range(40):
        c.execution_control("run_frames", count=30)
        for base in range(0x02000000, 0x02400000, 0x100000):
            data = bytes.fromhex(c.memory_read(base, size=0x100000).hex)
            i = data.find(b"DSWBTEST")
            while i >= 0:
                words = struct.unpack_from("<%dI" % len(MANIFEST), data, i + 8)
                m = dict(zip(MANIFEST, words))
                if m["self"] == base + i and m["ready"]:
                    return m
                i = data.find(b"DSWBTEST", i + 1)
    return None


class Shooter:
    def __init__(self, c, out, dry):
        self.c, self.out, self.dry = c, out, dry

    def save(self, name, png):
        if not self.dry:
            with open(os.path.join(self.out, name), "wb") as f:
                f.write(png)

    def shot(self, name, panel=None, max_width=1600, settle=1.5, show_panel=None, **show):
        """Point the window (ui.show's arguments), wait for the panels to fetch, save the picture of the
        panel (or of the whole window without one)."""
        if show or panel or show_panel:
            self.c.ui_show(panel=panel or show_panel, **show)
        time.sleep(settle)
        r = self.c.ui_capture(panel=panel, max_width=max_width)
        self.save(name, base64.b64decode(r.png))
        print("  %-24s %4dx%-4d %s%s" % (name, r.width, r.height, r.panel or "whole window",
                                       "  (still fetching: retake it)" if r.stale else ""))

    def grab(self, name, box):
        from PIL import ImageGrab
        img = ImageGrab.grab(bbox=box, all_screens=True)
        buf = io.BytesIO()
        img.save(buf, "PNG")
        self.save(name, buf.getvalue())
        print("  %-24s %4dx%-4d screen grab" % (name, img.width, img.height))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--melonds", default=os.path.join(ROOT, "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(ROOT, "tools", "workbench", "testrom", "wbtest.nds"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "img"))
    ap.add_argument("--port", type=int, default=47955)
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
    if os.path.isdir(r"C:\msys64\ucrt64\bin"):
        env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    env["DSWB_WORKSPACE"] = "Default"

    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--workbench",
                             "--kb", os.path.join(kbdir, "knowledge.db"), os.path.abspath(a.rom)],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for(lambda: os.path.exists(info), 20):
            print("melonDS did not start its API (no %s)" % info)
            return 1
        time.sleep(1.0)
        c = Client.connect(info_file=info, agent="docs")
        m = find_manifest(c)
        if not m:
            print("the test ROM's manifest was not found")
            return 1
        car = m["car"]
        frames = car + 0x0c                       # CarParams.frames: written every frame by the main loop
        speed = car + m["car_speed_offset"]       # from packed.lz, unpacked by the BIOS
        c.execution_control("run_frames", count=300)   # some writes, calls and coverage to show

        print("naming the guide's struct")
        c.kb_type_define(definition=STRUCT)
        c.kb_data_define(addr=car, size=STRUCT_SIZE, type="UnkCar", name="gUnkCar")

        wb = next((h for h, cls, title, r in windows_of(proc.pid) if "Decomp Workbench" in title), None)
        if wb:
            # big enough that the bottom tab group has room
            user32.ShowWindow(wb, 1)
            user32.MoveWindow(wb, 40, 20, 1700, 1300, True)
            time.sleep(1.5)
        s = Shooter(c, a.out, a.dry_run)
        print("pictures:")
        # use case 0: the whole window, the cursor on the struct so the Inspector has something to say
        s.shot("default-layout.png", addr=m["arm_func"], show_panel="dock_disasm", settle=2.5)
        # use case 1
        s.shot("inspector.png", panel="dock_inspector", max_width=900, sel_addr=frames, sel_size=4)
        s.shot("memory-define-data.png", panel="dock_memory", max_width=1200, sel_addr=car, sel_size=STRUCT_SIZE)
        s.shot("write-history.png", panel="dock_history", max_width=1400, sel_addr=car, sel_size=STRUCT_SIZE)
        # use case 4: a field computed from a packed ROM file, and a graphics file in RAM
        s.shot("data-origin.png", panel="dock_origin", max_width=1400, sel_addr=speed, sel_size=4)
        s.shot("file-structure.png", panel="dock_memory", max_width=1200, sel_addr=m["gfx_ncgr_buf"], sel_size=m["gfx_ncgr_size"])
        # use case 5
        # the call graph follows the cursor while it is showing: open it, then move the cursor
        c.ui_show(panel="dock_callgraph")
        time.sleep(0.5)
        s.shot("callgraph.png", panel="dock_callgraph", max_width=1000, addr=m["arm_func"] + 4)
        # use cases 7 and 10
        s.shot("coverage.png", panel="dock_coverage", max_width=1400)
        s.shot("recording-panel.png", panel="dock_recording", max_width=1400)

        # use case 2: the Memory view's right-click menu on the selected field. A right-click on the
        # address column keeps the selection (one on a byte outside it would select that byte)
        if wb:
            c.execution_control("pause")
            c.ui_show(sel_addr=frames, sel_size=4, panel="dock_memory")
            time.sleep(1.0)
            focus(wb)
            mem, lay = rect("Decomp Workbench", name="Memory"), rect("Decomp Workbench", name="Layers")
            if mem and lay:
                click_at(mem[0] + 12, lay[3] + 150, right=True)
                press("down")
                press("down")
                press("right", wait=0.8)              # Add watchpoint > its submenu
                menus = [r for h, cls, title, r in windows_of(proc.pid) if "Popup" in cls]
                if menus:
                    s.grab("memory-context-menu.png", (mem[0] - 8, lay[1] - 4, max(r[2] for r in menus) + 16, max(r[3] for r in menus) + 16))
                press("esc")
                press("esc")
            else:
                print("  memory-context-menu.png  the Memory view was not found on screen: take it by hand")
            c.execution_control("resume")

        # use case 5: X on the field in the Memory view
        if wb:
            c.ui_show(sel_addr=frames, sel_size=4, panel="dock_memory")
            time.sleep(1.0)
            focus(wb)
            mem, lay = rect("Decomp Workbench", name="Memory"), rect("Decomp Workbench", name="Layers")
            if mem and lay:
                click_at(mem[0] + 12, lay[3] + 150)
                key(ord("X"), wait=1.5)
                s.shot("xrefs.png", panel="dock_xrefs", max_width=1200, settle=0.5)
            else:
                print("  xrefs.png  the Memory view was not found on screen: take it by hand")

        # use case 2: a write watchpoint on the field, then the break
        c.breakpoint_set(kind="write", addr=frames, size=4)
        c.execution_control("resume")
        brk = wait_for(lambda: c.status_get().break_info, 10)
        if brk:
            time.sleep(1.0)
            s.shot("break-disasm.png", panel="dock_disasm", max_width=1200, cpu="arm9")
            s.shot("callstack.png", panel="dock_callstack", max_width=1200)
            s.shot("registers.png", panel="dock_registers", max_width=700)
            s.shot("breakpoints.png", panel="dock_breakpoints", max_width=1200)
        else:
            print("  the watchpoint did not hit: break-disasm, callstack, registers and breakpoints were not taken")
        c.breakpoint_clear(-1)
        c.execution_control("pause")

        # use case 3: a value scan for the frame counter: unknown, then increased three times, 30 frames apart
        if wb:
            c.ui_show(panel="dock_search")
            time.sleep(0.8)
            focus(wb)
            subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            os.path.join(ROOT, "tools", "workbench", "tests", "uia.ps1"), "-Action", "invoke",
                            "-Window", "Decomp Workbench", "-Name", "Value scan"], capture_output=True)
            time.sleep(0.5)
            cond = rect("Decomp Workbench", value="equal to")
            first, nxt = rect("Decomp Workbench", name="First scan"), rect("Decomp Workbench", name="Next scan")
            if cond and first and nxt:
                click(cond)
                press("end")                          # the last condition: unknown (first scan)
                press("enter")
                click(first)
                time.sleep(1.5)
                for _ in range(3):
                    c.execution_control("run_frames", count=30)
                    click(cond)
                    press("home")
                    for _ in range(4):                # equal to, not equal to, changed, unchanged, increased
                        press("down")
                    press("enter")
                    click(nxt)
                    time.sleep(1.5)
                s.shot("value-scan.png", panel="dock_search", max_width=1400, settle=0.5)
            else:
                print("  value-scan.png  the Search panel's controls were not found: take it by hand")

        print()
        print(json.dumps({k: "%08x" % v for k, v in m.items() if k.endswith(("buf", "func", "car", "table", "target"))} |
                         {"frames": "%08x" % frames, "speed": "%08x" % speed,
                          "break_pc": "%08x" % brk.pc if brk else None}, indent=1))
        with open(os.path.join(HERE, "capture.json"), "w") as f:
            json.dump({"manifest": m, "frames": frames, "speed": speed, "break_pc": brk.pc if brk else None}, f, indent=1)
        c.close()
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        if toml_orig is not None:
            open(toml, "w", encoding="utf-8").write(toml_orig)
    return 0


if __name__ == "__main__":
    sys.exit(main())
