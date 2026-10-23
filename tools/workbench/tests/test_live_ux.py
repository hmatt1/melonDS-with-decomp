"""Live checks the unit tests cannot see: automatic functions, breakpoints, and a paused configure.

Launches one melonDS with the workbench and the test ROM. Recording, write history and data
origin stay at their boot defaults (all on).
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client  # noqa: E402

FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def main():
    exe = os.path.abspath(os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    rom = os.path.abspath(os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    kb = os.path.join(tempfile.mkdtemp(prefix="dswb_ux_"), "knowledge.db")
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    proc = subprocess.Popen(
        [exe, "--api", "--api-port", "47970", "--workbench", "--kb", kb, rom],
        cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        c = Client.connect(info)
        st = c.status_get()
        check(st.recording.enabled and st.recording.write_history and st.recording.data_origin,
              "boot defaults: recording, write history and data origin are on")
        c.execution_control("run_frames", count=180)
        subs = []
        for _ in range(20):
            listed = c.kb_list("functions", limit=1000)
            subs = [f for f in (listed.functions or []) if f.name.startswith("sub_") and f.author_kind == "system"]
            if subs:
                break
            time.sleep(0.4)
        check(len(subs) > 0, "executed entries became sub_ functions with a system author (%d)" % len(subs))

        c.execution_control("pause")
        time.sleep(0.3)
        st = c.status_get()
        check(st.state == "paused", "pause leaves the game paused (state %s)" % st.state)
        bp = c.breakpoint_set("exec", subs[0].addr if subs else 0x02000000, size=4, cpu="arm9")
        off = c.breakpoint_update(bp.id, enabled=False)
        check(bp.id > 0 and not off.enabled, "a breakpoint can be disabled (id %d)" % bp.id)
        c.breakpoint_clear(bp.id)
        left = c.breakpoints_list()
        check(all(b.id != bp.id for b in (left.breakpoints or [])), "deleting a breakpoint removes it")
        c.recording_configure(write_history=False)
        st = c.status_get()
        check(st.state == "paused" and st.recording.enabled and not st.recording.write_history,
              "changing a recording switch while paused does not resume (state %s)" % st.state)
        c.close()
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
    print("\n%d failed" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
