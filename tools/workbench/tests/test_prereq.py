"""Prerequisites (UI_REQUIREMENTS PR-1..PR-4) on the test ROM.

1. `--data-origin` turns Recording and Data origin on from the first frame: a struct field the ROM
   fills from a file at boot carries that file's label.
2. With `--no-record`, turning Data origin on from the Recording menu starts Recording too (D-4).
3. Recording > Reset game with tracking restarts the game with Recording on.

The second and third parts send keystrokes to the workbench window: don't use the keyboard while
this runs.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
sys.path.insert(0, HERE)

from dswb import Client  # noqa: E402
from test_recording import find_manifest, read_manifest  # noqa: E402
from ui_person_flow import find_window, focus, key, press, dialog  # noqa: E402

FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def launch(exe, rom, port, flags):
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    proc = subprocess.Popen([exe, "--api", "--api-port", str(port)] + flags + [rom], cwd=exedir, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(info):
            break
        time.sleep(0.1)
    time.sleep(0.5)
    return proc, Client.connect(info)


def stop(proc):
    proc.terminate()
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()


def manifest(c):
    for _ in range(20):
        c.execution_control("run_frames", count=30)
        addr = find_manifest(c)
        if addr:
            m = read_manifest(c, addr)
            if m["ready"]:
                return m
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--port", type=int, default=47940)
    a = ap.parse_args()
    exe, rom = os.path.abspath(a.melonds), os.path.abspath(a.rom)
    toml = os.path.join(os.path.dirname(exe), "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    try:
        print("--data-origin")
        proc, c = launch(exe, rom, a.port, ["--data-origin"])
        try:
            st = c.status_get().recording
            check(st.enabled and st.data_origin and not st.write_history,
                  "--data-origin boots with Recording and Data origin on, write history off")
            m = manifest(c)
            o = c.origin_get(m["car"] + m["car_speed_offset"], size=4) if m else None
            srcs = [s for t in (o.taint if o else []) for s in t.sources]
            check(any(s.endswith("packed.lz") for s in srcs), "a field filled at boot from packed.lz carries its label: %s" % srcs)
        finally:
            c.close()
            stop(proc)

        print("turning data origin on starts recording")
        proc, c = launch(exe, rom, a.port, ["--workbench", "--no-record"])
        try:
            c.execution_control("run_frames", count=60)
            st = c.status_get().recording
            check(not st.enabled and not st.data_origin, "nothing is recorded with --no-record")
            hwnd = None
            for _ in range(50):
                hwnd = find_window("Decomp Workbench")
                if hwnd:
                    break
                time.sleep(0.1)
            check(hwnd is not None, "the workbench window is open")
            focus(hwnd)
            key(ord("D"), ("ctrl", "shift"))  # Recording > Data origin
            time.sleep(1.0)
            st = c.status_get().recording
            check(st.enabled and st.data_origin, "Recording > Data origin turns data origin on and starts recording with it")

            print("reset game with tracking")
            c.execution_control("run_frames", count=30)
            start_before = c.status_get().recording.session_frame_start
            focus(hwnd)
            key(ord("R"), ("ctrl", "shift"))  # Recording > Reset game with tracking...
            dlg = dialog("Reset game with tracking")
            check(dlg is not None, "Reset game with tracking asks what to record")
            press("enter")                    # Reset
            time.sleep(1.5)
            st = c.status_get().recording
            # a new session started with the reset (the frame counter itself keeps counting)
            check(st.enabled and st.data_origin and st.session_frame_start > start_before,
                  "the game restarted in a new session with recording and data origin on (session from frame %d, was %d)"
                  % (st.session_frame_start, start_before))
        finally:
            c.close()
            stop(proc)
    finally:
        # after melonDS exits: it saves its configuration on the way out
        if os.path.exists(toml):
            open(toml, "w", encoding="utf-8").write(toml_orig)
    print("\n%d failed" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
