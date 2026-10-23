"""Measures uncapped emulation speed with the recording toggles, against an optional upstream build.

    python tools/workbench/tests/bench_speed.py --rom game.nds --melonds build/melonDS.exe [--upstream ../melonDS-upstream/build/melonDS.exe]

The frame limiter is switched off in each build's melonDS.toml for the run and
restored afterwards. FPS is read from the window title ("[fps/target] melonDS").
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
from dswb import Client  # noqa: E402


def title_fps(pid):
    out = subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-Process -Id {pid}).MainWindowTitle"],
                         capture_output=True, text=True).stdout.strip()
    m = re.search(r"\[(\d+)/", out)
    return int(m.group(1)) if m else None


def set_limit(toml, limit):
    text = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    orig = text
    for key, val in (("LimitFPS", limit), ("AudioSync", False)):
        if re.search(rf"^{key} = ", text, re.M):
            text = re.sub(rf"^{key} = .*$", f"{key} = {'true' if val else 'false'}", text, flags=re.M)
        else:
            text = f"{key} = {'true' if val else 'false'}\n" + text
    open(toml, "w", encoding="utf-8").write(text)
    return orig


def run(exe, rom, args, warmup, seconds, configure=None):
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    exedir = os.path.dirname(exe)
    p = subprocess.Popen([exe] + args + [rom], cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(warmup)
        if configure:
            configure(os.path.join(exedir, "workbench", "api.json"))
            time.sleep(3)
        samples = []
        for _ in range(seconds):
            time.sleep(1)
            f = title_fps(p.pid)
            if f:
                samples.append(f)
        return samples
    finally:
        p.kill()
        p.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--upstream")
    ap.add_argument("--warmup", type=int, default=25)
    ap.add_argument("--seconds", type=int, default=12)
    ap.add_argument("--port", type=int, default=47950)
    ap.add_argument("--state", help="savestate to load before measuring (needs the API, so upstream is skipped)")
    ap.add_argument("--hold", default="", help="buttons to hold while measuring, e.g. a")
    a = ap.parse_args()
    rom = os.path.abspath(a.rom)
    exe = os.path.abspath(a.melonds)

    def cfg(**kw):
        def f(info):
            c = Client.connect(info, timeout=600)
            if a.state:
                c.state_load(path=os.path.abspath(a.state))
            if kw:
                c.recording_configure(**kw)
            if a.hold:
                import threading
                threading.Thread(target=lambda: c.input_send(buttons=a.hold.split("+"), frames=100000, capture=False), daemon=True).start()
        return f

    runs = []
    if a.upstream and not a.state:
        runs.append(("upstream melonDS", os.path.abspath(a.upstream), [], None))
    runs += [
        ("workbench, recording off", exe, ["--api", "--api-port", str(a.port)], cfg() if a.state else None),
        ("workbench, recording on", exe, ["--record", "--api", "--api-port", str(a.port)], cfg() if a.state else None),
        ("workbench, recording + write history", exe, ["--record", "--api", "--api-port", str(a.port)], cfg(write_history=True)),
        ("workbench, recording + history + data origin", exe, ["--record", "--api", "--api-port", str(a.port)], cfg(write_history=True, data_origin=True)),
    ]
    tomls = {}
    for _, e, _, _ in runs:
        t = os.path.join(os.path.dirname(e), "melonDS.toml")
        if t not in tomls:
            tomls[t] = set_limit(t, False)
    try:
        results = []
        for name, e, args, conf in runs:
            s = run(e, rom, args, a.warmup, a.seconds, conf)
            med = statistics.median(s) if s else 0
            results.append((name, med, min(s) if s else 0, max(s) if s else 0))
            print(f"{name:48s} median {med:6.1f} fps  (min {results[-1][2]}, max {results[-1][3]}, {len(s)} samples)", flush=True)
    finally:
        for t, orig in tomls.items():
            open(t, "w", encoding="utf-8").write(orig)
    return 0


if __name__ == "__main__":
    sys.exit(main())
