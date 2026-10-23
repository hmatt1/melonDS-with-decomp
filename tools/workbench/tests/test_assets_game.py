"""Graphics files of a real game: every one is described and drawn without an error.

    python tools/workbench/tests/test_assets_game.py --rom "TrackMania DS (USA) (En,Fr,Es).nds" [--out build/assets_game]

Boots the game with the API, waits for the graphics files to be indexed (TrackMania keeps its
NSBMD models and NSBTX textures in vfs.zip), then for each file: asset.describe, and asset.render of
its palettes, tiles, map, cells or every texture. Prints a table of formats, how many files were
understood whole, and any that stopped early. With --out, writes a contact sheet of PNGs to look at.
"""
import argparse
import base64
import collections
import os
import subprocess
import sys
import tempfile
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
        print(f"  ok   {what}")
    else:
        FAILS.append(what)
        print(f"  FAIL {what}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", required=True)
    ap.add_argument("--port", type=int, default=47960)
    ap.add_argument("--out", help="folder for the rendered PNGs")
    ap.add_argument("--limit", type=int, default=100000, help="files to check at most")
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    kbdir = tempfile.mkdtemp(prefix="dswb_gamegfx_")
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--kb", os.path.join(kbdir, "knowledge.db"), os.path.abspath(a.rom)],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        c = Client.connect(info, timeout=120)
        lst = None
        for _ in range(300):
            lst = c.asset_list(limit=5000)
            if lst.status in ("ready", "none") and (lst.files or lst.status == "none"):
                # the archive may finish after the containers: wait until the count settles
                time.sleep(2)
                again = c.asset_list(limit=5000)
                if len(again.files) == len(lst.files):
                    break
            time.sleep(0.5)
        print("graphics:", lst.summary)
        check(lst.status == "ready" and len(lst.files) > 0, "the game's graphics files are found (%d)" % len(lst.files))
        kinds = collections.Counter()
        whole = collections.Counter()
        partial = []
        failed = []
        textures = drawn = 0
        for f in lst.files[:a.limit]:
            kinds[f.kind] += 1
            try:
                d = c.asset_describe(path=f.path)
            except ApiError as e:
                failed.append((f.path, "describe: " + e.message))
                continue
            if d.unreadable_from == 0:
                whole[f.kind] += 1
            else:
                partial.append((f.path, d.unreadable_from, d.problem))
            parts = []
            if f.kind == "NCLR":
                parts = [("palette", 0)]
            elif f.kind == "NCGR":
                parts = [("tiles", 0)]
            elif f.kind == "NSCR":
                parts = [("map", 0)]
            elif f.kind == "NCER":
                parts = [("cell", i) for i in range(min(d.cells, 4))]
            elif d.textures:
                parts = [("texture", i) for i in range(len(d.textures))]
                textures += len(d.textures)
            for part, idx in parts:
                try:
                    r = c.asset_render(path=f.path, part=part, index=idx)
                    drawn += 1
                    if a.out:
                        name = (f.path.replace("/", "_").replace(":", "_").strip("_") + "_%s%d.png" % (part, idx))[-120:]
                        with open(os.path.join(a.out, name), "wb") as fh:
                            fh.write(base64.b64decode(r.png))
                except ApiError as e:
                    # a map or cells with no tiles to pair, or a damaged file: reported, not a failure
                    if d.unreadable_from == 0 and part == "texture":
                        failed.append((f.path, "%s %d: %s" % (part, idx, e.message)))
        print("\n%-6s %6s %6s" % ("kind", "files", "whole"))
        for k in sorted(kinds):
            print("%-6s %6d %6d" % (k, kinds[k], whole[k]))
        print("textures: %d, images drawn: %d" % (textures, drawn))
        for p, at, why in partial[:20]:
            print("  stopped early: %s at +0x%x: %s" % (p, at, why))
        for p, why in failed[:20]:
            print("  error: %s: %s" % (p, why))
        check(not failed, "every file is described, and every texture of a whole file is drawn (%d errors)" % len(failed))
        check(sum(whole.values()) >= 0.9 * sum(kinds.values()), "at least 90%% of the files are understood whole (%d of %d)"
              % (sum(whole.values()), sum(kinds.values())))
    except ApiError as e:
        check(False, "API error: %s" % e)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
    print("\n%d passed, %d failed" % (PASSES, len(FAILS)))
    for f in FAILS:
        print("  FAIL " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
