"""Tests for compare.py that need no emulator: every run prints one valid JSON object, and each
failure stops at the right stage with the error the workbench explains.

  python tools/workbench/compare/test_compare_script.py [--gcc PATH] [--objdiff PATH]

Fake compilers (.cmd wrappers around Python) cover failing, hanging and garbage-writing compilers.
With devkitARM's arm-none-eabi-gcc and objdiff-cli (found on PATH, in DEVKITARM, or build/tools),
it also runs the self-test and a real mismatch; without them those cases are skipped.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "compare.py")
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + ("" if cond or not detail else ": " + str(detail)))


def run(*args, timeout=60):
    p = subprocess.run([sys.executable, "-I", SCRIPT] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    out = p.stdout.decode("utf-8", "replace")
    lines = [l for l in out.splitlines() if l.strip()]
    try:
        obj = json.loads(out)
    except ValueError:
        obj = None
    return p.returncode, obj, lines, out


def fake(tmp, name, body):
    """A .cmd 'compiler' that runs body (Python) with the compiler's arguments in sys.argv."""
    py = os.path.join(tmp, name + ".py")
    with open(py, "w", encoding="utf-8") as f:
        f.write("import sys, time\nargs = sys.argv[1:]\nout = args[args.index('-o') + 1] if '-o' in args else None\n" + body)
    cmd = os.path.join(tmp, name + ".cmd")
    with open(cmd, "w", encoding="utf-8") as f:
        f.write('@"%s" -I "%s" %%*\n' % (sys.executable, py))
    return cmd


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcc")
    ap.add_argument("--objdiff")
    a = ap.parse_args()
    dka = os.environ.get("DEVKITARM", "")
    gcc = find_tool(a.gcc, ["arm-none-eabi-gcc", "arm-none-eabi-gcc.exe"],
                    [os.path.join(dka, "bin") if dka and os.path.isdir(dka) else "", r"C:\devkitPro\devkitARM\bin"])
    objdiff = find_tool(a.objdiff, ["objdiff-cli", "objdiff-cli.exe"], [os.path.join(REPO, "build", "tools")])
    tmp = tempfile.mkdtemp(prefix="dswb_cmptest_")
    try:
        src = os.path.join(tmp, "f.c")
        with open(src, "w") as f:
            f.write("int f(int x) { return x + 1; }\n")
        target = os.path.join(tmp, "target.o")
        with open(target, "wb") as f:
            f.write(b"\x7fELF")

        # bad arguments still answer
        code, obj, lines, out = run("--timeout", "notanumber")
        check("bad arguments: JSON", obj is not None and obj.get("matched") is False and len(lines) == 1, out)

        # missing inputs
        code, obj, lines, out = run("--compiler", "cc", "--objdiff", "x")
        check("no --file: stage compile", obj and obj["stage"] == "compile" and "--file is required" in obj["error"], out)
        code, obj, lines, out = run("--file", os.path.join(tmp, "nope.c"), "--symbol", "f", "--target", target, "--compiler", "cc", "--objdiff", "x")
        check("missing source", obj and obj["stage"] == "compile" and "no source file" in obj["error"], out)
        code, obj, lines, out = run("--file", src, "--symbol", "f", "--target", target, "--objdiff", "x")
        check("no compiler configured", obj and "no compiler is configured" in obj["error"], out)
        code, obj, lines, out = run("--file", src, "--symbol", "f", "--target", target, "--compiler", os.path.join(tmp, "missing-gcc.exe"), "--objdiff", "x")
        check("compiler not found", obj and obj["stage"] == "compile" and obj["error"].startswith("not found"), out)

        # a compiler that fails: its messages come back in log
        bad = fake(tmp, "fail_gcc", "print('f.c:1: error: expected ;', file=sys.stderr)\nsys.exit(1)\n")
        code, obj, lines, out = run("--file", src, "--symbol", "f", "--target", target, "--compiler", bad, "--objdiff", "x")
        check("compiler fails: stage compile", obj and obj["stage"] == "compile" and "compiler failed (exit 1)" in obj["error"], out)
        check("compiler fails: log kept", obj and "expected ;" in obj.get("log", ""), out)
        check("exit code 0 and one line", code == 0 and len(lines) == 1, out)

        # a compiler that hangs: the timeout ends it
        slow = fake(tmp, "slow_gcc", "time.sleep(6)\n")
        code, obj, lines, out = run("--file", src, "--symbol", "f", "--target", target, "--compiler", slow, "--objdiff", "x", "--timeout", "2")
        check("compiler hangs: timed out", obj and obj["stage"] == "compile" and "timed out" in obj["error"], out)

        # a compiler that writes garbage: extract can't read it
        junk = fake(tmp, "junk_gcc", "open(out, 'wb').write(b'not an object')\n")
        code, obj, lines, out = run("--file", src, "--symbol", "f", "--target", target, "--compiler", junk, "--objdiff", "x")
        check("garbage object: stage extract", obj and obj["stage"] == "extract" and "cannot read the compiled object" in obj["error"], out)

        # flags reach the compiler; the context header is prefixed with -include
        echo = fake(tmp, "echo_gcc", "open(out + '.args', 'w').write(repr(args))\nsys.exit(3)\n")
        ctx = os.path.join(tmp, "context.h")
        open(ctx, "w").close()
        run("--file", src, "--symbol", "f", "--target", target, "--compiler", echo, "--flags", '-O4,p -proc arm946e -DNAME="a b"', "--context", ctx, "--objdiff", "x",
            "--work", tmp)
        argsfile = os.path.join(tmp, "compiled.o.args")
        got = open(argsfile).read() if os.path.exists(argsfile) else ""
        check("flags and context passed", "'-O4,p'" in got and "'-proc'" in got and "'-include'" in got and repr(ctx) in got and "'-DNAME=a b'" in got, got)

        if not gcc:
            print("SKIP the real-compiler cases: arm-none-eabi-gcc not found (set DEVKITARM or pass --gcc)")
        else:
            flags = "-march=armv5te -mthumb -O2"
            # extract: the symbol isn't there
            code, obj, lines, out = run("--file", src, "--symbol", "g", "--target", target, "--compiler", gcc, "--flags", flags, "--objdiff", "x")
            check("symbol missing: stage extract", obj and obj["stage"] == "extract" and "'g' is not in the compiled object" in obj["error"] and "f" in obj["log"], out)
            # diff: objdiff missing or too old
            code, obj, lines, out = run("--file", src, "--symbol", "f", "--target", target, "--compiler", gcc, "--flags", flags, "--objdiff", os.path.join(tmp, "nope.exe"))
            check("objdiff not found: stage diff", obj and obj["stage"] == "diff" and obj["error"].startswith("not found"), out)
            old = fake(tmp, "old_objdiff", "print('objdiff-cli 2.7.1')\n")
            code, obj, lines, out = run("--file", src, "--symbol", "f", "--target", target, "--compiler", gcc, "--flags", flags, "--objdiff", old)
            check("objdiff too old", obj and obj["stage"] == "diff" and "too old" in obj["error"], out)
            if not objdiff:
                print("SKIP the objdiff cases: objdiff-cli not found (pass --objdiff, or put it in build/tools)")
            else:
                code, obj, lines, out = run("--self-test", "--compiler", gcc, "--flags", flags, "--objdiff", objdiff, "--compiler-label", "devkitarm-gcc")
                check("self-test scores 100", obj and obj.get("stage") == "done" and obj.get("score") == 100.0 and obj.get("matched") is True, out)
                check("self-test result fields", obj and obj.get("compiler") == "devkitarm-gcc" and obj.get("engine", "").startswith("objdiff-cli ")
                      and isinstance(obj.get("diff"), list) and len(obj["diff"]) > 0, out)
                # a real mismatch against a target compiled from different C
                tsrc = os.path.join(tmp, "t.c")
                with open(tsrc, "w") as f:
                    f.write("int f(int x) { return x * 3 + 1; }\n")
                tobj = os.path.join(tmp, "t.o")
                subprocess.run([gcc, "-c", "-march=armv5te", "-mthumb", "-O2", "-o", tobj, tsrc], check=True)
                code, obj, lines, out = run("--file", src, "--symbol", "f", "--target", tobj, "--compiler", gcc, "--flags", flags, "--objdiff", objdiff)
                check("mismatch scores under 100", obj and obj.get("stage") == "done" and obj.get("matched") is False and 0 <= obj.get("score", -1) < 100, out)
                # C++: the compiled object names f as _Z1fi; the target carries the plain name and is
                # renamed to match
                cpp = os.path.join(tmp, "f.cpp")
                with open(cpp, "w") as f:
                    f.write("int f(int x) { return x + 1; }\n")
                code, obj2, lines, out = run("--file", cpp, "--symbol", "f", "--target", tobj, "--compiler", gcc, "--flags", flags, "--objdiff", objdiff)
                check("C++: paired by its mangled name", obj2 and obj2.get("stage") == "done" and "_Z1fi" in obj2.get("log", ""), out)
                cobj = os.path.join(tmp, "c.o")
                subprocess.run([gcc, "-c", "-march=armv5te", "-mthumb", "-O2", "-o", cobj, src], check=True)
                code, obj2, lines, out = run("--file", cpp, "--symbol", "f", "--target", cobj, "--compiler", gcc, "--flags", flags, "--objdiff", objdiff)
                check("C++: the same code as C matches", obj2 and obj2.get("matched") is True, out)
                statuses = {r["status"] for r in obj.get("diff", [])} if obj else set()
                check("mismatch rows carry statuses", statuses and statuses <= {"match", "diff", "arg", "reloc", "insert", "delete"} and statuses != {"match"}, str(statuses))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed" % (len(passed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
