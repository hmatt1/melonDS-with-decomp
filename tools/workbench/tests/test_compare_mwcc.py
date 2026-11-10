"""Compare with the real compiler against a real game: CodeWarrior's mwccarm and TrackMania DS.

    python tools/workbench/tests/test_compare_mwcc.py --rom "TrackMania DS (USA) (En,Fr,Es).nds" [--mwcc PATH] [--objdiff PATH]

Boots the game, names four of its small ARM functions in a fresh knowledge store, writes C++ for
them, and runs the workbench's compare (kb.build.compare) with mwccarm 2.0 and the game's flags
(C++, so the compiled object holds the mangled names, which compare.py pairs with the target's):

  func_0202773c  ldr r0, [r0, #4]; bx lr                       a field getter
  func_0201dd24  cmp r0, #0; rsblt r0, r0, #0; bx lr           abs
  func_0202dc30  mov r1, #0; str r1, [r0]; bx lr               clears an int
  func_0202c9bc  ldr r1, =0x020dc070; str r1, [r0]; bx lr      stores a global's address (a relocation
                                                               to a named data object)

Each must match byte for byte, the self-test must pass, and a wrong version of each must not. Skips
(exit 0) without the ROM, mwccarm or objdiff-cli. mwccarm is never shipped or downloaded: point
--mwcc at your own copy.
"""
import argparse
import json
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
from test_compare_gcc import find_tool, toml_set, REPO  # noqa: E402

# TrackMania DS's flags (C++), with -nostdinc: the context header replaces the SDK's headers
FLAGS = ("-O4 -enum min -lang c++ -Cpp_exceptions off -proc arm946e -interworking -char signed -little -fp soft "
         "-rostr -sdatathreshold 8 -inline on -nostdinc")
TARGET = 0x020dc070
FUNCS = [  # address, size, name, expected first word
    (0x0202773c, 8, "tm_get_b", 0xe5900004),
    (0x0201dd24, 12, "tm_abs", 0xe3500000),
    (0x0202dc30, 12, "tm_clear", 0xe3a01000),
    (0x0202c9bc, 16, "tm_set_target", 0xe59f1004),
]
SOURCE = """// four of TrackMania DS's functions, for the workbench's compare test
struct TmPair { int a; int b; };

int tm_get_b(TmPair* p) { return p->b; }
int tm_abs(int x) { return x < 0 ? -x : x; }
void tm_clear(int* p) { *p = 0; }
void tm_set_target(u32** p) { *p = &gTmTarget; }
"""
WRONG = """struct TmPair { int a; int b; };

int tm_get_b(TmPair* p) { return p->a; }
int tm_abs(int x) { return x; }
void tm_clear(int* p) { *p = 1; }
void tm_set_target(u32** p) { *p = 0; }
"""

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
    ap.add_argument("--melonds", default=os.path.join(REPO, "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(REPO, "TrackMania DS (USA) (En,Fr,Es).nds"))
    ap.add_argument("--mwcc")
    ap.add_argument("--objdiff")
    ap.add_argument("--port", type=int, default=47965)
    a = ap.parse_args()
    mwcc = a.mwcc or next((p for p in [
        r"C:\Program Files (x86)\Freescale\CW for NINTENDO DS V2.0\ARM_Tools\Command_Line_Tools\mwccarm.exe"] if os.path.isfile(p)), None)
    objdiff = find_tool(a.objdiff, ["objdiff-cli.exe", "objdiff-cli"], [os.path.join(REPO, "build", "tools")])
    for what, ok in (("the TrackMania DS ROM", os.path.isfile(a.rom)), ("mwccarm (pass --mwcc)", mwcc), ("objdiff-cli", objdiff)):
        if not ok:
            print("SKIP: %s not found" % what)
            return 0

    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    kbdir = tempfile.mkdtemp(prefix="dswb_mwcc_")
    src = os.path.join(kbdir, "src")
    os.makedirs(src)
    open(os.path.join(src, "tm_match.cpp"), "w").write(SOURCE)
    open(os.path.join(src, "tm_wrong.cpp"), "w").write(WRONG)
    with open(os.path.join(kbdir, "compare.json"), "w") as f:
        json.dump({"version": 1, "default": {"compiler": "mwcc-2.0-sp1p7", "flags": FLAGS}, "modules": []}, f, indent=2)
    toml = os.path.join(exedir, "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    q = lambda s: "'%s'" % s
    open(toml, "w", encoding="utf-8").write(toml_set(toml_orig, {
        "CompareCommand": "''", "SourceRoot": q(src), "PythonPath": q(sys.executable), "ObjdiffPath": q(os.path.abspath(objdiff)),
        "Compilers": "[%s]" % q("mwcc-2.0-sp1p7=" + str(mwcc)), "CompareTimeoutSec": "60",
        "CompareApproved": q(compare_approval.fingerprint(compare_approval.DEFAULT_COMMAND)), "CompareSelfTestOk": None}))
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--kb", os.path.join(kbdir, "knowledge.db"), os.path.abspath(a.rom)],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        c = Client.connect(info, agent="mwcc-test", timeout=120)
        c.execution_control("run_frames", count=120)

        print("the game's functions")
        for addr, size, name, first in FUNCS:
            word = int.from_bytes(bytes.fromhex(c.memory_read(addr, size=4).hex), "little")
            check(word == first, "%s at %08x starts with %08x" % (name, addr, word))
            c.kb_function_define(addr, size=size, mode="arm", name=name)
            c.kb_function_update(addr, source_file="tm_match.cpp")
        check(c.kb_data_define(TARGET, size=4, type="u32", name="gTmTarget").ok, "the global at %08x is named gTmTarget" % TARGET)

        print("setup")
        res = c.build_setup_check(addr=FUNCS[0][0])
        rows = {r.id: r for r in res.rows}
        check(rows["compiler"].ok, "compiler: %s" % rows["compiler"].message)
        st = c.build_selftest(addr=FUNCS[0][0])
        check(st.ok and st.matched and st.score == 100.0, "the self-test passes with mwccarm and the game's C++ flags (%s)" % st.log)

        print("compare")
        for addr, _, name, _ in FUNCS:
            r = c.kb_build_compare(addr, record=False)
            check(r.ok and r.matched and r.score == 100.0, "%s matches byte for byte (score %s%s)" % (name, r.score,
                  "" if r.ok else ": %s %s" % (r.message, r.log)))
            check("compared as its C++ name _Z" in r.log, "%s is paired by its C++ name: %s" % (name, r.log.splitlines()[-1] if r.log else ""))
        r = c.kb_build_compare(FUNCS[3][0], record=False)
        check(any("gTmTarget" in x["orig"] and x["status"] == "match" for x in json.loads(r.diff)),
              "the literal-pool word is relocated to gTmTarget on both sides")

        print("a wrong reconstruction")
        for addr, _, name, _ in FUNCS:
            c.kb_function_update(addr, source_file="tm_wrong.cpp")
        for addr, _, name, _ in FUNCS:
            r = c.kb_build_compare(addr, record=False)
            check(r.ok and not r.matched, "%s does not match (score %.1f)" % (name, r.score))
    except ApiError as e:
        check(False, "API error: %s" % e)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        open(toml, "w", encoding="utf-8").write(toml_orig)
        shutil.rmtree(kbdir, ignore_errors=True)
    print("\n%d passed, %d failed" % (PASSES, len(FAILS)))
    for f in FAILS:
        print("  FAIL " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
