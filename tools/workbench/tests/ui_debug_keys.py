"""The Debug menu's keys work in the workbench window: Ctrl+F5 continues a paused game, Ctrl+Shift+F5 breaks a
running one, Ctrl+F9 then Ctrl+F5 runs to a breakpoint, F10 steps over a call. F5 alone is the game window's
(Load state 5): in the workbench it neither pauses nor resumes. Analysis > Mark unreached code and
Clear unreached marks show up in the API.

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
from dswb import ApiError, Client  # noqa: E402
from test_recording import find_manifest, read_manifest  # noqa: E402
from ui_person_flow import FAILS, check, dialog, find_window, focus, key, press, type_text, wait_ready  # noqa: E402

VK_F5 = 0x74        # not 0x75: that is F6
VK_F9 = 0x78
VK_F10 = 0x79
VK_F11 = 0x7A
VK_DOWN = 0x28


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

        print("F5 alone is not a workbench key")
        c.execution_control("pause")
        check(wait_state(c, "paused") == "paused", "the API pauses the game")
        focus(hwnd)
        key(VK_F5)
        time.sleep(0.5)
        check(state(c) == "paused", "F5 in the workbench leaves the game paused: %s" % state(c))

        print("Ctrl+F5 after the API paused the game")
        focus(hwnd)
        key(VK_F5, ("ctrl",))
        s = wait_state(c, "running")
        check(s == "running", "Ctrl+F5 continues it: %s (foreground: %s)" % (s, fg_title()))

        print("Ctrl+Shift+F5 while running")
        focus(hwnd)
        key(VK_F5, ("ctrl", "shift"))
        s = wait_state(c, "break")
        check(s == "break", "Ctrl+Shift+F5 breaks it: %s" % s)
        key(VK_F11)
        time.sleep(0.5)
        check(state(c) == "break", "F11 steps and stays in the break: %s" % state(c))
        focus(hwnd)
        key(VK_F5, ("ctrl",))
        s = wait_state(c, "running")
        check(s == "running", "Ctrl+F5 continues from the break: %s" % s)
        c.execution_control("pause")
        wait_state(c, "paused")

        print("Ctrl+F9 then Ctrl+F5 runs to the breakpoint")
        focus(hwnd)
        key(ord("G"))
        dialog("Go to")
        type_text("%08x" % m["arm_func"])
        press("enter")
        time.sleep(0.5)
        focus(hwnd)
        key(VK_F9, ("ctrl",))
        time.sleep(0.5)
        bl = c.breakpoints_list().breakpoints
        check(any(b.addr == m["arm_func"] for b in bl), "Ctrl+F9 sets a breakpoint at the function")
        key(VK_F5, ("ctrl",))
        s = wait_state(c, "break", timeout=5.0)
        bl = c.breakpoints_list().breakpoints
        hits = [b.hit_count for b in bl if b.addr == m["arm_func"]]
        check(s == "break" and hits and hits[0] >= 1, "Ctrl+F5 runs until the breakpoint hits: %s, hits %s" % (s, hits))

        print("F10 steps over a call")
        full = read_manifest(c, find_manifest(c))
        leaf = full["thumb_leaf"] & ~1
        lines = c.disasm_get(full["thumb_func"] & ~1, count=16).lines
        bl = next((ln for ln in lines if (ln.target & ~1) == leaf), None)
        check(bl is not None, "wb_thumb_func contains bl wb_thumb_leaf")
        if bl is not None:
            nxt = next(ln for ln in lines if ln.addr > bl.addr)
            c.breakpoint_clear(-1)
            c.breakpoint_set("exec", bl.addr)
            focus(hwnd)
            key(VK_F5, ("ctrl",))
            s = wait_state(c, "break", timeout=5.0)
            info = c.status_get().break_info
            pc = info.pc if info else 0
            check(s == "break" and pc == bl.addr, "continued to the call: %s at %08x" % (s, pc))
            focus(hwnd)
            key(VK_F10)
            end = time.time() + 3.0
            info = c.status_get().break_info
            pc = info.pc if info else 0
            while pc != nxt.addr and time.time() < end:
                time.sleep(0.1)
                info = c.status_get().break_info
                pc = info.pc if info else 0
            check(c.status_get().state == "break" and pc == nxt.addr and "pop" in nxt.text.lower(),
                  "F10 stops on the instruction after the call (%08x, next %08x %s)" % (pc, nxt.addr, nxt.text))
            c.breakpoint_clear(-1)

        print("Analysis menu marks and clears, and the API sees it")
        # Pause() while already in a break leaves the state at break. The menu treats both as held.
        c.execution_control("pause")
        held = state(c)
        check(held in ("paused", "break"), "the game is held before marking unreached code (%s)" % held)

        def drop_cond():
            try:
                c.kb_function_delete(full["cond_caller"] & ~1)
            except ApiError:
                pass

        bx = (full["cond_caller_end"] & ~1) - 4
        found = False
        for _ in range(3):
            drop_cond()
            focus(hwnd)
            key(ord("A"), ("alt",))
            time.sleep(0.2)
            key(VK_DOWN)        # Import -> Mark unreached code
            press("enter")
            end = time.time() + 8.0
            while time.time() < end:
                if c.analysis_get(limit=1).ranges:
                    break
                time.sleep(0.1)
            hit = c.analysis_get(addr=bx, size=4)
            if any(r.start <= bx < r.end for r in hit.ranges):
                found = True
                break
            try:
                c.analysis_clear()
            except ApiError:
                pass
        check(found, "Analysis > Mark unreached code marks the untaken bx lr")
        conf = ""
        if found:
            for _ in range(3):
                drop_cond()
                conf = c.disasm_get(bx, count=1).lines[0].confidence
                if conf == "from analysis":
                    break
            check(conf == "from analysis", "the window's mark is what disasm.get calls from analysis (%s)" % conf)
            focus(hwnd)
            key(ord("A"), ("alt",))
            time.sleep(0.2)
            key(VK_DOWN)        # Import -> Mark
            key(VK_DOWN)        # Mark -> Clear (Cancel is disabled once the walk has finished)
            press("enter")
            time.sleep(0.4)
            left = c.analysis_get(addr=bx, size=4)
            check(not any(r.start <= bx < r.end for r in left.ranges),
                  "Analysis > Clear unreached marks removes it from analysis.get")
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
