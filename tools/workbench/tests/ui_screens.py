"""Screenshots of the workbench window for review (UI_REQUIREMENTS GD-4).

Each built-in workspace, each panel's empty state (nothing recorded yet), and the views that show
recorded data: the coverage map, the disassembly's mode gutter, the Getting started panel.
Writes PNGs and an index.html to --out.

It opens and closes melonDS for every shot and takes window screenshots: don't use the computer
while it runs.
"""
import argparse
import html
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client  # noqa: E402

WORKSPACES = ["Default", "Decomp", "Debug", "Data tracing", "Coverage", "Minimal"]
EMPTY = [("dock_callgraph", "Call graph"), ("dock_callstack", "Call stack"), ("dock_coverage", "Coverage map"),
         ("dock_history", "Write history"), ("dock_origin", "Data origin"), ("dock_overlays", "Overlays"),
         ("dock_breakpoints", "Breakpoints"), ("dock_archive", "Archive"), ("dock_containers", "Containers"), ("dock_start", "Getting started"),
         ("dock_game", "Game screen (placeholder)")]


def ps_env():
    """PowerShell's Add-Type compiles shot.ps1's helper in the temp folder. Started through MSYS (its
    `timeout`, for one) the environment can lack TEMP, TMP and even LOCALAPPDATA; PowerShell then falls
    back to the Windows folder and Add-Type fails. Give it a temp folder, asking Windows if need be."""
    env = dict(os.environ)
    temp = next((d for d in (env.get("TEMP"), env.get("TMP")) if d and os.path.isdir(d) and ":" in d), None)
    if not temp:
        import ctypes
        buf = ctypes.create_unicode_buffer(260)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 0x1C, None, 0, buf) == 0:     # CSIDL_LOCAL_APPDATA
            temp = os.path.join(buf.value, "Temp")
    if temp and os.path.isdir(temp):
        env["TEMP"] = env["TMP"] = temp.replace("/", "\\")
    return env


def shot(exe, rom, out, name, env_extra, flags, prepare=None, wait=7.0):
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    env.update(env_extra)
    proc = subprocess.Popen([exe, "--api", "--workbench"] + flags + [rom], cwd=exedir, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    path = os.path.join(out, name + ".png")
    # a failed capture must not leave the last run's image in place, looking current
    if os.path.exists(path):
        os.remove(path)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        else:
            print("  %s: melonDS didn't start its API (no %s; exit code %s)" % (name, info, proc.poll()))
            return None
        if prepare:
            time.sleep(0.5)
            c = Client.connect(info)
            prepare(c)
            c.close()
        time.sleep(wait)
        r = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", os.path.join(HERE, "shot.ps1"),
                            "-Title", "Decomp Workbench", "-Out", path], capture_output=True, text=True, env=ps_env())
        if r.returncode != 0 or not os.path.exists(path):
            print("  %s: screenshot failed (exit code %d): %s" % (name, r.returncode, (r.stderr or r.stdout).strip()[:400]))
            return None
        return path
    finally:
        proc.kill()
        proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "..", "build", "ui_screens"))
    a = ap.parse_args()
    exe, rom, out = os.path.abspath(a.melonds), os.path.abspath(a.rom), os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    toml = os.path.join(os.path.dirname(exe), "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    shots = []
    try:
        for ws in WORKSPACES:
            shots.append(("Workspace: " + ws, shot(exe, rom, out, "ws_" + ws.replace(" ", "_"), {"DSWB_WORKSPACE": ws}, [])))
            open(toml, "w", encoding="utf-8").write(toml_orig)
        for dock, title in EMPTY:
            shots.append(("Nothing recorded: " + title, shot(exe, rom, out, "empty_" + dock, {"DSWB_SHOW_PANEL": dock}, [])))
            open(toml, "w", encoding="utf-8").write(toml_orig)

        def run(c):
            c.execution_control("run_frames", count=240)
        shots.append(("Recorded: coverage map", shot(exe, rom, out, "rec_coverage", {"DSWB_SHOW_PANEL": "dock_coverage"}, ["--record"], run)))
        open(toml, "w", encoding="utf-8").write(toml_orig)
        shots.append(("Recorded: disassembly mode gutter at wb_thumb_func",
                      shot(exe, rom, out, "rec_gutter", {"DSWB_SHOW_PANEL": "dock_disasm", "DSWB_GOTO": "02000000"}, ["--record"], run, wait=8.0)))
        open(toml, "w", encoding="utf-8").write(toml_orig)
        shots.append(("Recorded with data origin from boot: Data origin", shot(exe, rom, out, "rec_origin", {"DSWB_SHOW_PANEL": "dock_origin"},
                                                                             ["--data-origin"], run)))
    finally:
        if os.path.exists(toml):
            open(toml, "w", encoding="utf-8").write(toml_orig)
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as f:
        f.write("<!doctype html><meta charset=utf-8><title>Workbench screens</title><body style='font-family:sans-serif'>")
        for title, path in shots:
            f.write("<h3>%s</h3>" % html.escape(title))
            f.write("<img src='%s' style='max-width:100%%'>" % os.path.basename(path) if path else "<p>(no screenshot)</p>")
    missing = [t for t, p in shots if not p]
    print("%d screenshots in %s%s" % (len(shots) - len(missing), out, (", missing: %s" % missing) if missing else ""))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
