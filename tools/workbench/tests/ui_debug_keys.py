"""The Debug menu's keys work in the workbench window: F5 resumes a paused game and pauses a running one,
F9 then F5 runs to a breakpoint.

    python tools/workbench/tests/ui_debug_keys.py --rom tools/workbench/testrom/wbtest.nds

Keystrokes are synthesized with the Windows input API into the workbench window; the API pauses the
game first (as an agent would) and checks the results. Do not use the mouse or keyboard while it runs.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "python"))
from dswb import Client  # noqa: E402
from ui_person_flow import FAILS, check, dialog, find_window, focus, key, press, type_text, wait_ready  # noqa: E402

VK_F5 = 0x74        # not 0x75: that is F6
VK_F9 = 0x78


def state(c):
    return c.status_get().state


def fg_title():
    import ctypes
    u = ctypes.windll.user32
    h = u.GetForegroundWindow()
    buf = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(h, buf, 256)
    return buf.value


def wait_state(c, want, timeout=3.0):
    end = time.time() + timeout
    s = state(c)
    while s != want and time.time() < end:
        time.sleep(0.1)
        s = state(c)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--port", type=int, default=47971)
    ap.add_argument("--log", help="write melonDS's output here (Qt warnings, e.g. ambiguous shortcuts)")
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    import tempfile
    kbpath = os.path.join(tempfile.mkdtemp(prefix="dswb_kb_"), "knowledge.db")
    toml = os.path.join(exedir, "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else None
    out = open(a.log, "w") if a.log else subprocess.DEVNULL

    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--record", "--workbench", "--kb", kbpath, os.path.abspath(a.rom)],
                            cwd=exedir, env=env, stdout=out, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(1.0)
        c = Client.connect(info)
        m = wait_ready(c)
        check(m is not None, "test ROM running, manifest found")
        if m is None:
            return 1
        hwnd = find_window("Decomp Workbench")
        check(hwnd is not None, "workbench window is open")
        focus(hwnd)

        print("F5 after the API paused the game")
        c.execution_control("pause")
        check(wait_state(c, "paused") == "paused", "the API pauses the game")
        focus(hwnd)
        key(VK_F5)
        s = wait_state(c, "running")
        check(s == "running", "F5 resumes it: %s (foreground: %s)" % (s, fg_title()))

        print("F5 while running")
        focus(hwnd)
        key(VK_F5)
        s = wait_state(c, "paused")
        check(s == "paused", "F5 pauses it: %s" % s)

        print("F9 then F5 runs to the breakpoint")
        key(ord("G"), ("ctrl",))
        dialog("Go to")
        type_text("%08x" % m["arm_func"])
        press("enter")
        time.sleep(0.5)
        focus(hwnd)
        key(VK_F9)
        time.sleep(0.5)
        bl = c.breakpoints_list().breakpoints
        check(any(b.addr == m["arm_func"] for b in bl), "F9 sets a breakpoint at the function")
        key(VK_F5)
        s = wait_state(c, "break", timeout=5.0)
        bl = c.breakpoints_list().breakpoints
        hits = [b.hit_count for b in bl if b.addr == m["arm_func"]]
        check(s == "break" and hits and hits[0] >= 1, "F5 runs until the breakpoint hits: %s, hits %s" % (s, hits))
        c.close()
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        if toml_orig is not None:
            open(toml, "w", encoding="utf-8").write(toml_orig)
    print("\n%d failed" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
