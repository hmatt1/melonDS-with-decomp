"""Phase 3 exit check: a person names and annotates a function, its data and a register
using only the workbench window and its keyboard shortcuts.

    python tools/workbench/tests/ui_person_flow.py --rom tools/workbench/testrom/wbtest.nds

Keystrokes are synthesized with the Windows input API into the workbench window. The API is
used only to find the test ROM's manifest and to verify results; every edit is made through the UI.
Do not use the mouse or keyboard while it runs.
"""
import argparse
import base64
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import sqlite3
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
from dswb import Client  # noqa: E402

user32 = ctypes.windll.user32
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def find_window(title_part):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        n = user32.GetWindowTextLengthW(h)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(h, buf, n + 1)
        if user32.IsWindowVisible(h) and title_part in buf.value:
            found.append(h)
        return True
    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def focus(hwnd):
    """Bring a window to the foreground and give it keyboard focus."""
    if user32.GetForegroundWindow() == hwnd:
        return
    kernel32 = ctypes.windll.kernel32
    fg = user32.GetForegroundWindow()
    fg_thread = user32.GetWindowThreadProcessId(fg, None)
    me = kernel32.GetCurrentThreadId()
    target = user32.GetWindowThreadProcessId(hwnd, None)
    # sharing input state with the current foreground thread allows SetForegroundWindow
    user32.AttachThreadInput(me, fg_thread, True)
    user32.AttachThreadInput(me, target, True)
    user32.ShowWindow(hwnd, 5)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.SetFocus(hwnd)
    user32.AttachThreadInput(me, target, False)
    user32.AttachThreadInput(me, fg_thread, False)
    time.sleep(0.4)


VK = {"enter": 0x0D, "tab": 0x09, "esc": 0x1B, "f9": 0x78, "shift": 0x10, "ctrl": 0x11, "alt": 0x12}


def layout(toml):
    """The current workspace and each saved workspace's layout (ADS XML), as the window saved them."""
    text = open(toml, encoding="utf-8").read()
    cur = re.search(r'^Workspace = "(.*)"$', text, re.M)
    arr = re.search(r'^Workspaces = "(.*)"$', text, re.M)
    states = {}
    if arr:
        for w in json.loads(arr.group(1).replace('\\"', '"').replace('\\\\', '\\')):
            states[w["name"]] = base64.b64decode(w["state"]).decode("utf-8", "replace")
    return (cur.group(1) if cur else ""), states


def panel_open(xml, name):
    m = re.search(r'<Widget Name="%s"[^>]*Closed="(\d)"' % name, xml)
    return bool(m) and m.group(1) == "0"


def key(vk, mods=()):
    for m in mods:
        user32.keybd_event(VK[m], 0, 0, 0)
    user32.keybd_event(vk, 0, 0, 0)
    user32.keybd_event(vk, 0, 2, 0)
    for m in reversed(mods):
        user32.keybd_event(VK[m], 0, 2, 0)
    time.sleep(0.08)


def press(name, mods=()):
    key(VK[name], mods)


def type_text(text):
    for ch in text:
        r = user32.VkKeyScanW(ord(ch))
        vk, shift = r & 0xFF, (r >> 8) & 1
        key(vk, ("shift",) if shift else ())


def dialog(*titles, timeout=5.0):
    """Wait for one of the dialogs to open and give it the keyboard focus."""
    end = time.time() + timeout
    while time.time() < end:
        for t in titles:
            h = find_window(t)
            if h:
                focus(h)
                return h
        time.sleep(0.1)
    return None


def set_file_name(dlg, path, timeout=5.0):
    """Fill the file name box of a native open-file dialog (typing races its start-up)."""
    WM_SETTEXT = 0x000C
    end = time.time() + timeout
    while time.time() < end:
        edit = user32.GetDlgItem(dlg, 0x47C)               # cmb13: file name combo box
        if edit:
            inner = user32.FindWindowExW(edit, None, "ComboBox", None) or edit
            box = user32.FindWindowExW(inner, None, "Edit", None)
            if box:
                user32.SendMessageW(box, WM_SETTEXT, 0, ctypes.c_wchar_p(path))
                focus(dlg)
                user32.SetFocus(box)
                return True
        time.sleep(0.1)
    type_text(path)                                        # fall back to typing
    return False


def wait_ready(c):
    for _ in range(30):
        c.execution_control("run_frames", count=30)
        for base in range(0x02000000, 0x02400000, 0x100000):
            data = bytes.fromhex(c.memory_read(base, size=0x100000).hex)
            i = data.find(b"DSWBTEST")
            while i >= 0:
                self_ptr, version = struct.unpack_from("<II", data, i + 8)
                if self_ptr == base + i:
                    vals = struct.unpack_from("<10I", data, i + 16)
                    if struct.unpack_from("<I", data, i + 8 + 4 * 27)[0]:   # ready
                        vfs_buf, vfs_size = struct.unpack_from("<II", data, i + 8 + 4 * 28)
                        return dict(arm_func=vals[0], data_table=vals[6], vfs_buf=vfs_buf, vfs_size=vfs_size)
                i = data.find(b"DSWBTEST", i + 1)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--port", type=int, default=47970)
    ap.add_argument("--shot", help="save a screenshot of the workbench at the end (PNG path)")
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    import tempfile
    kbdir = tempfile.mkdtemp(prefix="dswb_kb_")
    kbpath = os.path.join(kbdir, "knowledge.db")

    # a decomp source tree and the stand-in compare command, set in melonDS.toml for this run
    srcroot = os.path.join(kbdir, "decomp")
    os.makedirs(os.path.join(srcroot, "src"))
    with open(os.path.join(srcroot, "src", "test.c"), "w") as f:
        f.write("extern int gTestTable[4];\n\nint Test_ArmSum(void)\n{\n    return gTestTable[0] + gTestTable[1];\n}\n")
    toml = os.path.join(exedir, "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    fwd = lambda p: p.replace(os.sep, "/")
    compare = 'python "%s" --function {addr} --file {file}' % fwd(os.path.join(HERE, "fake_compare.py"))
    import re
    cfg = toml_orig if "[Workbench]\n" in toml_orig else toml_orig + "\n[Workbench]\n"
    import compare_approval
    for k, v in (("SourceRoot", '"%s"' % fwd(srcroot)), ("CompareCommand", "'%s'" % compare),
                 ("CompareApproved", "'%s'" % compare_approval.fingerprint(compare))):
        cfg = re.sub(r"(?m)^%s = .*\n" % k, "", cfg)
        cfg = cfg.replace("[Workbench]\n", "[Workbench]\n%s = %s\n" % (k, v), 1)
    # a saved workspace remembers whether the game is embedded; start from none so the toggle below is the first
    cfg = re.sub(r"(?m)^EmbeddedScreen = .*\n", "", cfg)
    cfg = re.sub(r"(?m)^HideMainWindow = .*\n", "", cfg)
    wm = re.search(r'(?m)^Workspaces = "(.*)"$', cfg)
    if wm:
        saved = json.loads(wm.group(1).replace('\\"', '"').replace("\\\\", "\\"))
        for wspace in saved:
            wspace.pop("game_screen", None)
        packed = json.dumps(saved, separators=(",", ":")).replace("\\", "\\\\").replace('"', '\\"')
        cfg = cfg[:wm.start()] + 'Workspaces = "%s"' % packed + cfg[wm.end():]
    open(toml, "w", encoding="utf-8").write(cfg)
    env["DSWB_SHOW_PANEL"] = "dock_decompiler"

    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--record", "--workbench", "--kb", kbpath, os.path.abspath(a.rom)],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(1.0)
        c = Client.connect(info)
        m = wait_ready(c)
        check(m is not None, "test ROM running, manifest found")
        if m is None:
            return 1
        c.execution_control("pause")
        time.sleep(0.5)
        check(os.path.exists(kbpath), "knowledge store opened for the ROM")
        db = sqlite3.connect(kbpath)

        def q(sql, args=()):
            db.commit()
            return db.execute(sql, args).fetchall()
        FN = "SELECT f.name, f.size, f.mode FROM function f JOIN module m ON m.id = f.module_id WHERE m.load_address + f.offset = ?"

        hwnd = find_window("Decomp Workbench")
        check(hwnd is not None, "workbench window is open")
        focus(hwnd)

        print("find and name the function")
        key(ord("G"), ("ctrl",))
        dialog("Go to")
        type_text("%08x" % m["arm_func"])
        press("enter")
        time.sleep(0.5)
        focus(hwnd)
        type_text("p")                      # define function; the dialog detects size and mode from the recording
        check(dialog("Define function", "Edit function") is not None, "P opens the function dialog")
        type_text("TestArmSum")
        press("enter")
        time.sleep(0.8)
        focus(hwnd)
        rows = q(FN, (m["arm_func"],))
        check(rows and rows[0][0] == "TestArmSum", "P defines the function with its name: %s" % rows)
        check(rows and rows[0][1] == 24 and rows[0][2] == "arm", "size and mode detected from the recording: 20 bytes of ARM code and its 4-byte literal pool")

        print("comment")
        type_text(";")
        check(dialog("Add annotation") is not None, "; opens the annotation dialog")
        type_text("adds the first two table words")
        press("tab")
        type_text("test")
        press("enter")
        time.sleep(0.8)
        focus(hwnd)
        rows = q("SELECT a.comment, a.tags, a.target_kind FROM annotation a JOIN module m ON m.id = a.module_id WHERE m.load_address + a.offset = ?", (m["arm_func"],))
        check(any(r[0] == "adds the first two table words" and r[1] == "test" for r in rows), "; adds an annotation with tags: %s" % rows)

        print("register annotation")
        type_text(";")
        check(dialog("Add annotation") is not None, "; opens the annotation dialog")
        press("tab", ("shift",))
        press("tab", ("shift",))            # target kind
        type_text("register")
        time.sleep(0.3)
        press("tab")                        # address
        press("tab")                        # register
        type_text("r1")
        press("tab")                        # whole function
        press("tab")                        # comment
        type_text("pointer to the data table")
        press("tab")
        press("enter")
        time.sleep(0.8)
        focus(hwnd)
        rows = q("SELECT register, comment, function_id FROM annotation WHERE target_kind = 'register'")
        check(any(r[0] == "r1" and r[1] == "pointer to the data table" and r[2] for r in rows), "register annotation attached to the function: %s" % rows)

        print("rename, undo, redo")
        type_text("n")
        dialog("Rename")
        type_text("Test_ArmSum")
        press("enter")
        time.sleep(0.6)
        focus(hwnd)
        check(q(FN, (m["arm_func"],))[0][0] == "Test_ArmSum", "N renames the function")
        # an agent edits meanwhile; the person's Ctrl+Z must still undo the person's own rename
        ag = Client.connect(info, agent="ui-agent")
        check(ag.kb_label_set(m["data_table"] + 0x40, name="agent_label").ok, "an agent sets a label while the person works")
        ag.close()
        time.sleep(0.4)
        key(ord("Z"), ("ctrl",))
        time.sleep(0.6)
        check(q(FN, (m["arm_func"],))[0][0] == "TestArmSum", "Ctrl+Z undoes the rename")
        check(q("SELECT count(*) FROM label WHERE name = 'agent_label'") == [(1,)], "and leaves the agent's label alone")
        key(ord("Y"), ("ctrl",))
        time.sleep(0.6)
        check(q(FN, (m["arm_func"],))[0][0] == "Test_ArmSum", "Ctrl+Y redoes it")

        print("data")
        key(ord("G"), ("ctrl",))
        dialog("Go to")
        type_text("%08x" % m["data_table"])
        press("enter")
        time.sleep(0.5)
        focus(hwnd)
        type_text("d")
        check(dialog("Define data", "Edit data object") is not None, "D opens the data dialog")
        type_text("gTestTable")
        press("enter")
        time.sleep(0.8)
        focus(hwnd)
        rows = q("SELECT d.name FROM data_object d LEFT JOIN module m ON m.id = d.module_id WHERE ifnull(m.load_address, 0) + d.offset = ?", (m["data_table"],))
        check(rows and rows[0][0] == "gTestTable", "D defines the data object: %s" % rows)

        print("breakpoint and bookmark")
        key(ord("G"), ("ctrl",))
        dialog("Go to")
        type_text("%08x" % m["arm_func"])
        press("enter")
        time.sleep(0.5)
        focus(hwnd)
        press("f9")
        time.sleep(0.6)
        bl = c.breakpoints_list().breakpoints
        check(any(b.addr == m["arm_func"] and b.kind == "exec" for b in bl), "F9 sets an execute breakpoint at the cursor")
        press("f9")
        time.sleep(0.6)
        check(not any(b.addr == m["arm_func"] for b in c.breakpoints_list().breakpoints), "F9 again removes it")
        key(ord("B"), ("ctrl",))
        dialog("Bookmark")
        press("enter")
        time.sleep(0.6)
        focus(hwnd)
        check(len(q("SELECT id FROM bookmark")) >= 1, "Ctrl+B adds a bookmark")

        print("source, compare and decompiler")
        key(ord("G"), ("ctrl",))
        dialog("Go to")
        type_text("%08x" % m["arm_func"])
        press("enter")
        time.sleep(0.5)
        focus(hwnd)
        type_text("p")
        check(dialog("Edit function") is not None, "P on an existing function opens it for editing")
        press("tab")
        press("tab")
        press("tab")                        # name -> signature -> subsystem -> source file
        type_text("src/test.c")
        press("enter")
        time.sleep(0.8)
        focus(hwnd)
        rows = q("SELECT f.source_file FROM function f JOIN module m ON m.id = f.module_id WHERE m.load_address + f.offset = ?", (m["arm_func"],))
        check(rows and rows[0][0] == "src/test.c", "source file recorded for the function: %s" % rows)
        STATUS = ("SELECT s.status, s.last_score FROM function_status s JOIN function f ON f.id = s.function_id "
                  "JOIN module m ON m.id = f.module_id WHERE m.load_address + f.offset = ?")
        key(ord("B"), ("ctrl", "shift"))   # Build > Compare
        time.sleep(3.0)
        rows = q(STATUS, (m["arm_func"],))
        check(rows == [("partially_matched", 82.5)], "Compare records a build result and the status derives from it: %s" % rows)
        with open(os.path.join(srcroot, "src", "test.c"), "a") as f:
            f.write("/* MATCH */\n")
        key(ord("B"), ("ctrl", "shift"))
        time.sleep(3.0)
        rows = q(STATUS, (m["arm_func"],))
        check(rows == [("matched", 100.0)], "a matching compare marks the function matched: %s" % rows)
        check(len(q("SELECT id FROM build_result")) == 2, "every compare attempt is kept")
        js = os.path.join(kbdir, "analysis.json")
        text = "int FUN_%08x(void)\n{\n    return DAT_%08x[0] + DAT_%08x[1];\n}\n" % (m["arm_func"], m["data_table"], m["data_table"])
        with open(js, "w") as f:
            json.dump({"tool": "ghidra", "decompiled": [{"addr": "0x%08x" % m["arm_func"], "text": text}]}, f)
        key(ord("I"), ("ctrl", "shift"))    # File > Import static analysis
        dlg = dialog("Static analysis export")
        check(dlg is not None, "Ctrl+Shift+I opens the import dialog")
        if dlg:
            set_file_name(dlg, js.replace("/", "\\"))   # the dialog rejects forward slashes
            press("enter")
        rows = []
        for _ in range(40):
            rows = q("SELECT tool FROM decompiler_output")
            if rows:
                break
            time.sleep(0.2)
        focus(hwnd)
        check(rows == [("ghidra",)], "decompiler output imported: %s" % rows)

        print("game archive")
        key(ord("F"), ("ctrl", "shift"))     # Navigate > Locate archived file
        dlg = dialog("Locate archived file")
        check(dlg is not None, "Ctrl+Shift+F asks for a file in the game archive")
        key(ord("A"), ("ctrl",))
        type_text("models/test.nsbmd")
        press("enter")
        time.sleep(1.5)
        focus(hwnd)
        key(ord("B"), ("ctrl",))             # bookmark the cursor to see where the locate went
        dialog("Bookmark")
        press("enter")
        time.sleep(0.6)
        focus(hwnd)
        rows = q("SELECT address FROM bookmark ORDER BY id DESC LIMIT 1")
        check(m["vfs_size"] == 768 and rows == [(m["vfs_buf"],)],
              "the archived file is located and the cursor moves to its RAM copy at %08x: %s"
              % (m["vfs_buf"], [hex(r[0]) for r in rows]))

        print("layout: workspaces and the Panels palette")
        focus(hwnd)
        key(ord("6"), ("ctrl", "alt"))       # Window > Workspaces > Minimal (the layout is saved within a second)
        time.sleep(1.6)
        cur, states = layout(toml)
        check(cur == "Minimal" and not panel_open(states.get("Minimal", ""), "dock_origin"),
              "Ctrl+Alt+6 switches to the Minimal workspace: %s" % cur)
        key(ord("P"), ("ctrl", "shift"))
        pal = dialog("Panels")
        check(pal is not None, "Ctrl+Shift+P opens the Panels palette")
        type_text("where the selected")      # words from the Data origin panel's description
        press("enter")
        time.sleep(1.6)
        focus(hwnd)
        cur, states = layout(toml)
        check(panel_open(states.get("Minimal", ""), "dock_origin"),
              "typing part of a panel's description and Enter opens that panel in the workspace")
        key(ord("1"), ("ctrl", "alt"))
        time.sleep(1.6)
        away, _ = layout(toml)
        key(ord("6"), ("ctrl", "alt"))
        time.sleep(1.6)
        back, states = layout(toml)
        check(away == "Default" and back == "Minimal" and panel_open(states.get("Minimal", ""), "dock_origin"),
              "switching away and back keeps the workspace's layout (%s, then %s)" % (away, back))
        # each workspace keeps its game screen mode: Minimal with the game in the workbench, Default without
        embedded = lambda: re.search(r'^EmbeddedScreen = true$', open(toml, encoding="utf-8").read(), re.M) is not None
        key(ord("G"), ("ctrl", "shift"))     # View > Game screen in the workbench
        time.sleep(1.6)
        on_min = embedded()
        key(ord("1"), ("ctrl", "alt"))
        time.sleep(1.6)
        on_def = embedded()
        key(ord("6"), ("ctrl", "alt"))
        time.sleep(1.6)
        on_back = embedded()
        check(on_min and not on_def and on_back,
              "a workspace keeps its game screen mode (Minimal %s, Default %s, Minimal again %s)" % (on_min, on_def, on_back))
        key(ord("G"), ("ctrl", "shift"))
        time.sleep(0.5)
        key(ord("1"), ("ctrl", "alt"))
        time.sleep(0.5)

        authors = q("SELECT a.kind, a.name, count(*) FROM change_log c JOIN author a ON a.id = c.author_id GROUP BY a.id ORDER BY a.kind DESC")
        check(len(authors) == 2 and authors[0][0] == "person" and authors[1][:2] == ("agent", "ui-agent") and authors[1][2] == 1,
              "every change is the person's except the agent's one label: %s" % authors)
        if a.shot:
            time.sleep(1.0)
            subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", os.path.join(HERE, "shot.ps1"),
                            "-Title", "Decomp Workbench", "-Out", a.shot])
        c.close()
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        # after melonDS exits: it saves its configuration on the way out
        if os.path.exists(toml):
            open(toml, "w", encoding="utf-8").write(toml_orig)
    print("\n%d failed" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
