"""UI test of the Compare setup checklist, from a fresh setup to a passing self-test.

    python tools/workbench/tests/ui_compare_setup.py [--melonds build/melonDS.exe] [--gcc PATH] [--objdiff PATH]

1. A fresh setup (every Workbench.Compare*, tool path and source root cleared, a new project): the
   Source and diff panel's checklist lists what is missing; build.setup.check gives the same rows.
   The panel is captured to build/ui_screens/compare_checklist_fresh.png.
2. The paths a person would pick with the checklist's buttons are set (devkitARM's gcc stands in for
   mwccarm), the workbench restarts on the same project, and the self-test passes: every row passes
   and the checklist hides. Captured to build/ui_screens/compare_checklist_ready.png.

Opens a window but sends no keys or mouse input. Skips (exit 0) without gcc, objdiff-cli or the
test ROM's ELF.
"""
import argparse
import base64
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client, ApiError  # noqa: E402
import compare_approval  # noqa: E402
from test_compare_gcc import elf_symbols, find_tool, toml_set, FLAGS, FUNCS, REPO, TESTROM  # noqa: E402

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


def launch(exe, rom, port, kbdir, env):
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    proc = subprocess.Popen([exe, "--api", "--api-port", str(port), "--workbench", "--kb", os.path.join(kbdir, "knowledge.db"), rom],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(info):
            break
        time.sleep(0.1)
    time.sleep(0.5)
    return proc, Client.connect(info, agent="setup-test")


def stop(proc):
    proc.terminate()
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()


def capture(c, name):
    out = os.path.join(REPO, "build", "ui_screens")
    os.makedirs(out, exist_ok=True)
    cap = c.ui_capture(panel="source", max_width=0)
    with open(os.path.join(out, name), "wb") as f:
        f.write(base64.b64decode(cap.png))
    return cap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(REPO, "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(TESTROM, "wbtest.nds"))
    ap.add_argument("--gcc")
    ap.add_argument("--objdiff")
    ap.add_argument("--port", type=int, default=47950)
    a = ap.parse_args()
    dka = os.environ.get("DEVKITARM", "")
    gcc = find_tool(a.gcc, ["arm-none-eabi-gcc.exe", "arm-none-eabi-gcc"],
                    [os.path.join(dka, "bin") if dka and os.path.isdir(dka) else "", r"C:\devkitPro\devkitARM\bin"])
    objdiff = find_tool(a.objdiff, ["objdiff-cli.exe", "objdiff-cli"], [os.path.join(REPO, "build", "tools")])
    rom = os.path.abspath(a.rom)
    elf = os.path.splitext(rom)[0] + ".elf"
    if not gcc or not objdiff or not os.path.isfile(elf):
        print("SKIP: needs arm-none-eabi-gcc, objdiff-cli and the built test ROM")
        return 0
    syms = elf_symbols(elf)
    leaf, size = syms["wb_match_leaf"]
    leaf &= ~1

    exe = os.path.abspath(a.melonds)
    toml = os.path.join(os.path.dirname(exe), "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    kbdir = tempfile.mkdtemp(prefix="dswb_cmpui_")
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    q = lambda s: "'%s'" % s
    try:
        print("a fresh setup")
        fresh = {k: None for k in ("CompareCommand", "SourceRoot", "PythonPath", "ObjdiffPath", "Compilers", "CompareApproved",
                                   "CompareSelfTestOk", "CompareTimeoutSec")}
        fresh["PythonPath"] = q(os.path.join(kbdir, "no-python.exe"))   # as on a computer without Python
        open(toml, "w", encoding="utf-8").write(toml_set(toml_orig, fresh))
        proc, c = launch(exe, rom, a.port, kbdir, env)
        try:
            c.execution_control("run_frames", count=30)
            # the C file's functions and global, as test_compare_gcc.py names them
            for name, sig in FUNCS.items():
                addr, n = syms[name]
                c.kb_function_define(addr & ~1, size=n, mode="arm" if name == "wb_match_arm" else "thumb", name=name)
                c.kb_function_update(addr & ~1, signature=sig, source_file="wb_match.c")
            c.kb_data_define(syms["wb_match_gain"][0], size=4, type="s32", name="wb_match_gain")
            rows = {r.id: r for r in c.build_setup_check(addr=leaf).rows}
            for rid in ("python", "compiler", "objdiff", "source_root", "settings", "self_test"):
                check(rid in rows and not rows[rid].ok, "%s needs setting up: %s" % (rid, rows[rid].message if rid in rows else "no row"))
            check(not c.build_setup_check(addr=leaf).ready, "Compare is not ready")
            c.ui_show(addr=leaf, panel="source")
            time.sleep(3)
            cap = capture(c, "compare_checklist_fresh.png")
            check(cap.width > 0, "checklist captured (%dx%d)" % (cap.width, cap.height))
            try:
                r = c.kb_build_compare(leaf, record=False)
                check(not r.ok, "Compare before setup fails with a reason: %s" % r.message)
            except ApiError as e:
                check(True, "Compare before setup is refused: %s" % e.message)
        finally:
            stop(proc)

        print("after the checklist's fixes")
        # what the buttons set: Python, the compiler for compare.json's label, objdiff-cli, the source tree
        with open(os.path.join(kbdir, "compare.json"), "w") as f:
            f.write('{"version": 1, "default": {"compiler": "devkitarm-gcc", "flags": "%s"}, "modules": []}' % FLAGS)
        open(toml, "w", encoding="utf-8").write(toml_set(toml_orig, {
            "CompareCommand": "''", "SourceRoot": q(os.path.abspath(os.path.join(TESTROM, "match_src"))),
            "PythonPath": q(sys.executable), "ObjdiffPath": q(os.path.abspath(objdiff)),
            "Compilers": "[%s]" % q("devkitarm-gcc=" + os.path.abspath(gcc)),
            "CompareApproved": q(compare_approval.fingerprint(compare_approval.DEFAULT_COMMAND)), "CompareSelfTestOk": None}))
        proc, c = launch(exe, rom, a.port, kbdir, env)
        try:
            c.execution_control("run_frames", count=30)
            res = c.build_setup_check(addr=leaf)
            check(res.ready and not {r.id: r for r in res.rows}["self_test"].ok, "ready to compare; the self-test is still to run")
            st = c.build_selftest(addr=leaf)
            check(st.ok and st.score == 100.0, "the self-test passes (%s)" % st.engine)
            res = c.build_setup_check(addr=leaf)
            check(all(r.ok for r in res.rows), "every checklist row passes")
            c.ui_show(addr=leaf, panel="source")
            time.sleep(3)
            capture(c, "compare_checklist_ready.png")
            r = c.kb_build_compare(leaf, record=False)
            check(r.ok and r.matched, "Compare matches wb_match_leaf (score %s %s %s)" % (r.score, r.message, r.log))
        finally:
            stop(proc)
    except ApiError as e:
        check(False, "API error: %s" % e)
    finally:
        open(toml, "w", encoding="utf-8").write(toml_orig)
        shutil.rmtree(kbdir, ignore_errors=True)
    print("\n%d passed, %d failed" % (PASSES, len(FAILS)))
    for f in FAILS:
        print("  FAIL " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
