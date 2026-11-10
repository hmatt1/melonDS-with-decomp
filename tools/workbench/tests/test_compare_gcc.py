"""End-to-end Compare with no proprietary tools: devkitARM's gcc stands in for mwccarm.

    python tools/workbench/tests/test_compare_gcc.py [--melonds build/melonDS.exe] [--gcc PATH] [--objdiff PATH]

The test ROM builds match_src/wb_match.c into the ROM (source/match.c) with arm-none-eabi-gcc. This
test boots it, names those functions and their global in the knowledge store, points compare.json
at the same gcc and flags, and runs the workbench's own compare path (kb.build.compare): the
reference script compiles the C file again with the generated context header, and objdiff-cli
diffs it against the target object the workbench writes from RAM. Every function must match;
match_src/wb_match_wrong.c must not. It also checks build.setup.check and build.selftest.

Needs the built test ROM (wbtest.nds and wbtest.elf), arm-none-eabi-gcc and objdiff-cli 3.8+;
skips (exit 0) when one is missing.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
TESTROM = os.path.join(HERE, "..", "testrom")
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client, ApiError  # noqa: E402
import compare_approval  # noqa: E402

# the test ROM's ARM9 CFLAGS (testrom/Makefile), less -g and the include paths
FLAGS = "-O2 -ffunction-sections -fdata-sections -march=armv5te -mtune=arm946e-s -mthumb -DARM9"
FUNCS = {
    "wb_match_leaf": "s32 wb_match_leaf(s32 a, s32 b)",
    "wb_match_caller": "s32 wb_match_caller(s32 x)",
    "wb_match_arm": "u32 wb_match_arm(const u32* p, s32 n)",
}

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


def elf_symbols(path):
    """name -> (value, size) of the defined symbols in a 32-bit little-endian ELF."""
    data = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for s in secs:
        if s[1] != 2:
            continue
        stroff = secs[s[6]][4]
        for o in range(s[4], s[4] + s[5], 16):
            nm, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, o)
            if shndx:
                end = data.find(b"\0", stroff + nm)
                out[data[stroff + nm:end].decode()] = (value, size)
    return out


def find_tool(given, names, dirs):
    # the given one, then the folders named (devkitARM's own gcc, which built the test ROM), then PATH
    if given:
        return given
    for d in dirs:
        for n in names:
            if d and os.path.isfile(os.path.join(d, n)):
                return os.path.join(d, n)
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    return None


def toml_set(cfg, keys):
    """Sets [Workbench] keys (values already TOML-quoted) and drops the ones given as None."""
    cfg = cfg if "[Workbench]\n" in cfg else cfg + "\n[Workbench]\n"
    for k, v in keys.items():
        cfg = re.sub(r"(?m)^%s = .*\n" % k, "", cfg)
        if v is not None:
            cfg = cfg.replace("[Workbench]\n", "[Workbench]\n%s = %s\n" % (k, v), 1)
    return cfg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(REPO, "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(TESTROM, "wbtest.nds"))
    ap.add_argument("--gcc")
    ap.add_argument("--objdiff")
    ap.add_argument("--port", type=int, default=47920)
    a = ap.parse_args()

    dka = os.environ.get("DEVKITARM", "")
    gcc = find_tool(a.gcc, ["arm-none-eabi-gcc.exe", "arm-none-eabi-gcc"],
                    [os.path.join(dka, "bin") if dka and os.path.isdir(dka) else "", r"C:\devkitPro\devkitARM\bin"])
    objdiff = find_tool(a.objdiff, ["objdiff-cli.exe", "objdiff-cli"], [os.path.join(REPO, "build", "tools")])
    rom = os.path.abspath(a.rom)
    elf = os.path.splitext(rom)[0] + ".elf"
    for what, ok in (("arm-none-eabi-gcc", gcc), ("objdiff-cli", objdiff), ("the test ROM", os.path.isfile(rom)),
                     ("wbtest.elf (build the test ROM)", os.path.isfile(elf))):
        if not ok:
            print("SKIP: %s not found" % what)
            return 0
    syms = elf_symbols(elf)
    missing = [n for n in list(FUNCS) + ["wb_match_gain"] if n not in syms]
    if missing:
        print("FAIL: the test ROM has no %s (rebuild it)" % ", ".join(missing))
        return 1

    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    kbdir = tempfile.mkdtemp(prefix="dswb_cmp_")
    srcroot = os.path.abspath(os.path.join(TESTROM, "match_src"))
    with open(os.path.join(kbdir, "compare.json"), "w") as f:
        json.dump({"version": 1, "default": {"compiler": "devkitarm-gcc", "flags": FLAGS}, "modules": []}, f, indent=2)
    toml = os.path.join(exedir, "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    q = lambda s: "'%s'" % s
    open(toml, "w", encoding="utf-8").write(toml_set(toml_orig, {
        "CompareCommand": "''",
        "SourceRoot": q(srcroot),
        "PythonPath": q(sys.executable),
        "ObjdiffPath": q(os.path.abspath(objdiff)),
        "Compilers": "[%s]" % q("devkitarm-gcc=" + os.path.abspath(gcc)),
        "CompareTimeoutSec": "60",
        "CompareApproved": q(compare_approval.fingerprint(compare_approval.DEFAULT_COMMAND)),
        "CompareSelfTestOk": None,
    }))
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--kb", os.path.join(kbdir, "knowledge.db"), rom],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        c = Client.connect(info, agent="compare-test")
        c.execution_control("run_frames", count=60)

        print("knowledge store")
        for name, sig in FUNCS.items():
            addr, size = syms[name]
            thumb = name != "wb_match_arm"
            r = c.kb_function_define(addr & ~1, size=size, mode="thumb" if thumb else "arm", name=name)
            check(r.ok, "define %s at %08x (%d bytes)" % (name, addr & ~1, size))
            c.kb_function_update(addr & ~1, signature=sig, source_file="wb_match.c")
        gain, _ = syms["wb_match_gain"]
        check(c.kb_data_define(gain, size=4, type="s32", name="wb_match_gain").ok, "define wb_match_gain")

        print("setup check")
        leaf = syms["wb_match_leaf"][0] & ~1
        rows = {r.id: r for r in c.build_setup_check(addr=leaf).rows}
        for rid in ("python", "compiler", "objdiff", "source_root", "settings"):
            check(rid in rows and rows[rid].ok, "%s: %s" % (rid, rows[rid].message if rid in rows else "missing row"))
        check("self_test" in rows and not rows["self_test"].ok, "self-test not run yet")

        print("self-test")
        st = c.build_selftest(addr=leaf)
        check(st.ok and st.score == 100.0 and st.matched and st.stage == "done", "build.selftest scores 100 (%s, %s)" % (st.score, st.message))
        check(st.engine.startswith("objdiff-cli "), "engine reported: %s" % st.engine)
        res = c.build_setup_check(addr=leaf)
        check(res.ready and all(r.ok for r in res.rows), "every checklist row passes after the self-test")

        print("compare (dry runs)")
        for name in FUNCS:
            addr = syms[name][0] & ~1
            r = c.kb_build_compare(addr, record=False)
            rows = json.loads(r.diff) if r.diff else []
            check(r.ok and r.matched and r.score == 100.0 and r.stage == "done",
                  "%s matches byte for byte (score %s, stage %s%s)" % (name, r.score, r.stage, (": " + r.message + " " + r.explanation) if r.message else ""))
            check(not r.recorded and rows and all(x["status"] == "match" for x in rows), "%s: a dry run, every row matches" % name)
        cr = c.kb_build_compare(syms["wb_match_caller"][0] & ~1, record=False)
        check(any("wb_match_leaf" in x["orig"] for x in json.loads(cr.diff)), "the call is relocated to wb_match_leaf in the target object")
        check(any("wb_match_gain" in x["orig"] for x in json.loads(cr.diff)), "the literal-pool word is relocated to wb_match_gain")
        ctx = os.path.join(kbdir, "compare", "run")
        headers = [os.path.join(dp, f) for dp, _, fs in os.walk(ctx) for f in fs if f == "context.h"]
        text = open(headers[0]).read() if headers else ""
        check("extern s32 wb_match_gain;" in text and "s32 wb_match_leaf(s32 a, s32 b);" in text,
              "the context header declares the file's global and functions")
        check(not c.kb_function_get(leaf).attempts, "dry runs record nothing")

        print("compare (recorded)")
        r = c.kb_build_compare(leaf)
        check(r.ok and r.recorded and r.matched, "a recorded compare")
        det = c.kb_function_get(leaf)
        check(len(det.attempts) == 1 and det.attempts[0].matched and det.function.status == "matched",
              "the attempt is recorded and the function is matched: %s" % det.function.status)

        print("a wrong reconstruction")
        for name in FUNCS:
            c.kb_function_update(syms[name][0] & ~1, source_file="wb_match_wrong.c")
        for name in FUNCS:
            r = c.kb_build_compare(syms[name][0] & ~1, record=False)
            check(r.ok and not r.matched and r.score < 100.0, "%s does not match (score %.1f)" % (name, r.score))

        print("failures are explained")
        c.kb_function_update(leaf, source_file="no_such_file.c")
        r = c.kb_build_compare(leaf, record=False)
        check(not r.ok and r.stage == "compile" and "no source file" in r.message and r.explanation, "missing C file: %s / %s" % (r.message, r.explanation))
        for name in FUNCS:
            c.kb_function_update(syms[name][0] & ~1, source_file="wb_match.c")
        c.kb_function_update(leaf, name="wb_match_leaf_renamed")
        r = c.kb_build_compare(leaf, record=False)
        check(not r.ok and r.stage == "extract" and "is not in the compiled object" in r.message, "a name the C file doesn't define: %s %s" % (r.message, r.log))
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
