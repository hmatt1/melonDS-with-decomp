"""Compare setup by clicking, as a person would: the checklist's buttons, the file dialogs, the
confirmation before a command first runs, the Compare button, and Reset to template.

    python tools/workbench/tests/ui_compare_clicks.py [--melonds build/melonDS.exe] [--gcc PATH] [--objdiff PATH]

Drives the window through Windows UI Automation (uia.ps1: buttons by their text, fields by their
label), so it needs a desktop session and must not be disturbed while it runs; it sends no raw
keystrokes. devkitARM's gcc stands in for mwccarm. Skips (exit 0) without gcc, objdiff-cli or the
test ROM's ELF.

 1. a fresh setup: the checklist lists what is missing
 2. Locate python.exe, Browse for objdiff-cli, Choose folder: each opens the Windows file dialog;
    the test types the path and clicks Open / Select Folder
 3. Add settings for <module>: the label dialog; then Browse for the compiler
 4. Run self-test: the confirmation shows the command; Run it; the self-test passes
 5. Compare in the Source and diff panel: no second confirmation; the attempt is recorded, matched
 6. the project's script changes: Compare asks again; Cancel records nothing; the API is refused
 7. Compare setup > Script > Reset to template: Yes; Compare runs again without asking
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import ApiError  # noqa: E402
from test_compare_gcc import elf_symbols, find_tool, toml_set, FLAGS, FUNCS, REPO, TESTROM  # noqa: E402
from ui_compare_setup import launch, stop, capture  # noqa: E402

WINDOW = "Decomp Workbench"
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


def uia(action, window, name="", text="", type_="", timeout=10):
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(HERE, "uia.ps1"),
           "-Action", action, "-Window", window, "-Timeout", str(timeout)]
    if name:
        cmd += ["-Name", name]
    if text:
        cmd += ["-Text", text]
    if type_:
        cmd += ["-Type", type_]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 60)
    return r.returncode == 0, r.stdout.strip()


def click(window, name, timeout=10):
    ok, out = uia("invoke", window, name, timeout=timeout)
    if not ok:
        print("       (%s)" % out)
    return ok


def pick_file(title, path):
    """The Windows open dialog titled `title`: type the path, click Open."""
    return pick_dialog(title, "File name:", path, "Open")


def pick_folder(title, path):
    return pick_dialog(title, "Folder:", path, "Select Folder")


def pick_dialog(title, field, path, button):
    ok, out = uia("settext", title, field, path, "Edit", timeout=15)
    if not ok:
        print("       (%s)" % out)
    else:
        ok = click(title, button)
    if not ok:
        uia("invoke", title, "Cancel", timeout=2)
    return ok


def wait_row(c, addr, rid, want=True, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        rows = {r.id: r for r in c.build_setup_check(addr=addr).rows}
        if rid in rows and rows[rid].ok == want:
            return rows[rid]
        time.sleep(0.5)
    return {r.id: r for r in c.build_setup_check(addr=addr).rows}.get(rid)


def attempts(c, addr):
    return c.kb_function_get(addr).attempts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(REPO, "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(TESTROM, "wbtest.nds"))
    ap.add_argument("--gcc")
    ap.add_argument("--objdiff")
    ap.add_argument("--port", type=int, default=47985)
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
    leaf = syms["wb_match_leaf"][0] & ~1
    srcroot = os.path.abspath(os.path.join(TESTROM, "match_src"))

    exe = os.path.abspath(a.melonds)
    toml = os.path.join(os.path.dirname(exe), "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    kbdir = tempfile.mkdtemp(prefix="dswb_clicks_")
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    fresh = {k: None for k in ("CompareCommand", "SourceRoot", "PythonPath", "ObjdiffPath", "Compilers", "CompareApproved",
                               "CompareSelfTestOk", "CompareTimeoutSec")}
    fresh["PythonPath"] = "'%s'" % os.path.join(kbdir, "no-python.exe")
    open(toml, "w", encoding="utf-8").write(toml_set(toml_orig, fresh))
    # a shared project's compare.json may already hold the flags, before anyone picks a compiler
    with open(os.path.join(kbdir, "compare.json"), "w") as f:
        f.write('{"version": 1, "default": {"compiler": "", "flags": "%s"}, "modules": []}' % FLAGS)
    proc, c = launch(exe, rom, a.port, kbdir, env)
    try:
        print("1. a fresh setup")
        c.execution_control("run_frames", count=30)
        for name, sig in FUNCS.items():
            addr, n = syms[name]
            c.kb_function_define(addr & ~1, size=n, mode="arm" if name == "wb_match_arm" else "thumb", name=name)
            c.kb_function_update(addr & ~1, signature=sig, source_file="wb_match.c")
        c.kb_data_define(syms["wb_match_gain"][0], size=4, type="s32", name="wb_match_gain")
        module = c.kb_function_get(leaf).function.module
        c.ui_show(addr=leaf, panel="source")
        time.sleep(3)
        rows = {r.id: r for r in c.build_setup_check(addr=leaf).rows}
        check(not rows["python"].ok and not rows["objdiff"].ok and not rows["source_root"].ok and not rows["settings"].ok,
              "python, objdiff-cli, the source tree and the settings need setting up")
        ok, listing = uia("list", WINDOW)
        check("Locate python.exe..." in listing and "Choose folder..." in listing, "the checklist shows its fix buttons")

        print("2. the file dialogs")
        check(click(WINDOW, "Locate python.exe...") and pick_file("Locate python.exe", sys.executable), "Locate python.exe: picked in the dialog")
        r = wait_row(c, leaf, "python")
        check(r and r.ok, "python row passes: %s" % (r.message if r else ""))
        check(click(WINDOW, "Browse for objdiff-cli...") and pick_file("Locate objdiff-cli", os.path.abspath(objdiff)), "Browse for objdiff-cli: picked")
        r = wait_row(c, leaf, "objdiff")
        check(r and r.ok, "objdiff row passes: %s" % (r.message if r else ""))
        check(click(WINDOW, "Choose folder...") and pick_folder("Source tree", srcroot), "Choose folder: picked")
        r = wait_row(c, leaf, "source_root")
        check(r and r.ok and os.path.normcase(r.message) == os.path.normcase(srcroot), "source tree row passes: %s" % (r.message if r else ""))

        print("3. compiler settings")
        check(click(WINDOW, "Add settings for %s..." % module), "Add settings for %s" % module)
        check(uia("settext", "Compiler settings", "", "devkitarm-gcc", "Edit")[0] and click("Compiler settings", "OK"), "label devkitarm-gcc entered")
        r = wait_row(c, leaf, "settings")
        check(r and r.ok and "devkitarm-gcc" in r.message and "-mthumb" in r.message, "settings row passes with the project's flags: %s" % (r.message if r else ""))
        c.ui_show(addr=leaf, panel="source")
        time.sleep(1)
        check(click(WINDOW, "Browse for the compiler...") and pick_file("Locate the compiler for 'devkitarm-gcc'", os.path.abspath(gcc)),
              "Browse for the compiler: picked")
        r = wait_row(c, leaf, "compiler")
        check(r and r.ok, "compiler row passes: %s" % (r.message if r else ""))

        print("4. self-test and the confirmation")
        c.ui_show(addr=leaf, panel="source")
        time.sleep(1)
        check(click(WINDOW, "Run self-test"), "Run self-test")
        ok, cmd = uia("gettext", "Run this compare command?", "", type_="Edit|Document", timeout=15)
        check(ok and "compare.py" in cmd and "--self-test" in cmd, "the confirmation shows the exact command: %s" % cmd[:100])
        check(click("Run this compare command?", "Run it"), "Run it")
        r = wait_row(c, leaf, "self_test", seconds=60)
        check(r and r.ok, "the self-test passes: %s" % (r.message if r else ""))
        res = c.build_setup_check(addr=leaf)
        check(res.ready and all(x.ok for x in res.rows), "every row passes")

        print("5. Compare")
        c.ui_show(addr=leaf, panel="source")
        time.sleep(2)
        capture(c, "compare_clicks_ready.png")
        check(click(WINDOW, "Compare"), "Compare clicked")
        time.sleep(1)
        check(not uia("exists", "Run this compare command?", timeout=2)[0], "no second confirmation for the same command")
        end = time.time() + 30
        while time.time() < end and not attempts(c, leaf):
            time.sleep(0.5)
        at = attempts(c, leaf)
        check(len(at) == 1 and at[0].matched and at[0].score == 100.0, "the attempt is recorded, matched")

        print("6. the script changes")
        script = os.path.join(kbdir, "compare", "compare.py")
        with open(script, "a") as f:
            f.write("\n# an edit\n")
        check(click(WINDOW, "Compare"), "Compare clicked again")
        check(uia("exists", "Run this compare command?", timeout=10)[0], "the changed script is confirmed first")
        check(click("Run this compare command?", "Cancel"), "Cancel")
        time.sleep(2)
        check(len(attempts(c, leaf)) == 1, "nothing is recorded")
        try:
            c.kb_build_compare(leaf, record=False)
            check(False, "the API refuses the unapproved script")
        except ApiError as e:
            check("approved" in e.message, "the API refuses the unapproved script: %s" % e.message[:80])

        print("7. Reset to template")
        c.ui_show(panel="compare_setup")
        time.sleep(1)
        check(click(WINDOW, "Script"), "Script tab")
        check(click(WINDOW, "Reset to template..."), "Reset to template...")
        check(click("Reset to template", "Yes"), "Yes")
        time.sleep(1)
        with open(script) as f:
            check("# an edit" not in f.read(), "the project's script is the template again")
        c.ui_show(addr=leaf, panel="source")
        time.sleep(1)
        check(click(WINDOW, "Compare"), "Compare clicked")
        check(not uia("exists", "Run this compare command?", timeout=3)[0], "the approved command runs without asking")
        end = time.time() + 30
        while time.time() < end and len(attempts(c, leaf)) < 2:
            time.sleep(0.5)
        check(len(attempts(c, leaf)) == 2, "a second attempt is recorded")
    except ApiError as e:
        check(False, "API error: %s" % e)
    finally:
        stop(proc)
        open(toml, "w", encoding="utf-8").write(toml_orig)
        shutil.rmtree(kbdir, ignore_errors=True)
    print("\n%d passed, %d failed" % (PASSES, len(FAILS)))
    for f in FAILS:
        print("  FAIL " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
