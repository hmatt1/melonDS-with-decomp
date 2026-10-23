"""The workbench settings dialog and an API started from the command line.

    python tools/workbench/tests/ui_settings.py --melonds build/melonDS.exe --rom tools/workbench/testrom/wbtest.nds

melonDS starts with --api on a port other than the saved one, while the saved settings have the API off.
The dialog must show the API as it is (on, on that port), so changing another API option keeps it running
there, and only ticking or unticking "Enable" turns it on or off. Sends keystrokes to the workbench
window: don't use the keyboard while this runs.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
sys.path.insert(0, HERE)

from dswb import Client  # noqa: E402
from ui_person_flow import find_window, focus, key, press, dialog  # noqa: E402

FAILS = []
ENABLE_API, ALLOW_MEMORY_WRITE = 6, 8     # Tab presses from the first field: the dialog's focus order


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def settings_toggle(wb, tabs):
    """File > Workbench settings..., toggle the checkbox `tabs` Tab presses from the first field, OK."""
    focus(wb)
    key(ord("F"), ("alt",))
    time.sleep(0.4)
    key(0x26)                  # up: Close
    key(0x26)                  # up: Workbench settings...
    press("enter")
    if not dialog("Workbench settings"):
        return False
    for _ in range(tabs):
        press("tab")
    key(0x20)                  # space
    press("enter")             # OK
    time.sleep(1.5)
    return True


def saved(toml, name):
    m = re.search(r"(?ms)^\[Workbench\].*?^%s = (\S+)" % name, open(toml, encoding="utf-8").read())
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--port", type=int, default=47950)
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    toml = os.path.join(exedir, "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    cfg = toml_orig if "[Workbench]\n" in toml_orig else toml_orig + "\n[Workbench]\n"
    cfg = re.sub(r"(?m)^(ApiEnabled|ApiPort|ApiMemoryWrite) = .*\n", "", cfg)
    cfg = cfg.replace("[Workbench]\n", "[Workbench]\nApiEnabled = false\nApiPort = 47800\nApiMemoryWrite = false\n", 1)
    open(toml, "w", encoding="utf-8").write(cfg)
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    kb = os.path.join(tempfile.mkdtemp(prefix="dswb_settings_"), "knowledge.db")
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--workbench", "--kb", kb, os.path.abspath(a.rom)],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(1.0)
        first = json.load(open(info, encoding="utf-8"))
        c = Client(first["read_port"], first["control_port"], first["token"], agent="settings")
        check(not c.status_get().memory_write_enabled, "started with --api on port %d, memory writes off" % first["read_port"])
        wb = find_window("Decomp Workbench")
        check(wb is not None, "workbench window open")

        print("changing 'Allow memory.write' only")
        check(settings_toggle(wb, ALLOW_MEMORY_WRITE), "settings dialog: ticked 'Allow memory.write', OK")
        check(proc.poll() is None, "melonDS still running")
        running = os.path.exists(info)
        check(running, "the API is still on")
        if running:
            now = json.load(open(info, encoding="utf-8"))
            check(now["read_port"] == a.port, "still on the command line's port %d (now %d)" % (a.port, now["read_port"]))
            c2 = Client(now["read_port"], now["control_port"], now["token"], agent="settings2")
            check(c2.status_get().memory_write_enabled, "memory writes are on")
            c2.close()
        check(saved(toml, "ApiEnabled") == "false", "the saved setting still has the API off (it was on for this run only): %s" % saved(toml, "ApiEnabled"))
        check(saved(toml, "ApiMemoryWrite") == "true", "'Allow memory.write' was saved")

        print("unticking 'Enable the API'")
        check(settings_toggle(wb, ENABLE_API), "settings dialog: unticked 'Enable the API', OK")
        time.sleep(0.5)
        check(proc.poll() is None and not os.path.exists(info), "the API is off")
        try:
            c.close()
        except OSError:
            pass
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        open(toml, "w", encoding="utf-8").write(toml_orig)
    print("\n%d failed" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
