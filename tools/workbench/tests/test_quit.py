"""Quitting melonDS with the workbench in use: closing the main window ends the process with exit code 0.

    python tools/workbench/tests/test_quit.py --melonds build/melonDS.exe --rom tools/workbench/testrom/wbtest.nds

Each case starts melonDS with workbench options, lets the game run, and closes the main window the way a
person does (the workbench window too, first, in one case). Opens and closes windows: don't use the
mouse or keyboard while this runs.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client  # noqa: E402

user32 = ctypes.windll.user32
WM_CLOSE = 0x0010
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def windows_of(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            found.append((h, buf.value))
        return True
    user32.EnumWindows(cb, 0)
    return found


def run_case(exe, rom, flags, clients=0, close_workbench_first=False, port=47930):
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    kb = os.path.join(tempfile.mkdtemp(prefix="dswb_quit_"), "knowledge.db")
    args = [exe] + flags + (["--api-port", str(port)] if "--api" in flags else []) + ["--kb", kb, rom]
    p = subprocess.Popen(args, cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    conns = []
    try:
        time.sleep(3.0)
        if clients:
            d = json.load(open(info, encoding="utf-8"))
            for i in range(clients):
                c = Client(d["read_port"], d["control_port"], d["token"], agent="quit%d" % i)
                c.status_get()
                conns.append(c)
        wins = windows_of(p.pid)
        if close_workbench_first:
            for h, t in wins:
                if "Decomp Workbench" in t:
                    user32.PostMessageW(h, WM_CLOSE, 0, 0)
            time.sleep(1.0)
        main = [h for h, t in wins if "melonDS" in t and "Decomp Workbench" not in t]
        for h in main:
            user32.PostMessageW(h, WM_CLOSE, 0, 0)
        try:
            rc = p.wait(30)
        except subprocess.TimeoutExpired:
            rc = None
        return rc, len(main)
    finally:
        if p.poll() is None:
            p.kill()
        for c in conns:
            try:
                c.close()
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--repeat", type=int, default=2, help="runs per case: the crashes on quit depend on timing")
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    rom = os.path.abspath(a.rom)
    toml = os.path.join(os.path.dirname(exe), "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else None
    cases = [
        ("plain", [], 0, False),
        ("workbench window open", ["--workbench"], 0, False),
        ("workbench window closed first", ["--workbench"], 0, True),
        ("recording", ["--record"], 0, False),
        ("API with two clients connected", ["--api"], 2, False),
        ("everything", ["--api", "--record", "--workbench"], 2, False),
    ]
    try:
        for name, flags, clients, wb_first in cases:
            print(name)
            for i in range(a.repeat):
                rc, mains = run_case(exe, rom, flags, clients, wb_first)
                code = "still running after 30 s" if rc is None else hex(rc & 0xFFFFFFFF)
                check(mains == 1 and rc == 0, "run %d: closing the main window exits with 0 (%s)" % (i + 1, code))
    finally:
        if toml_orig is not None:
            open(toml, "w", encoding="utf-8").write(toml_orig)
    print("\n%d failed" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
