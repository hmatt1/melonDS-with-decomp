"""Reference compare script for the DS Decomp Workbench.

Compiles one function's C file with the module's compiler, checks the function is in the compiled
object, diffs it against the target object the workbench wrote (the original bytes, with the
relocations it could infer) using objdiff-cli, and prints one JSON object:

  {"score", "matched", "compiler", "flags", "diff": [{"orig", "compiled", "status"}], "recorded",
   "stage", "log", "engine", "source_file"}

stage is where it stopped: compile, extract, diff or done. Any failure still prints valid JSON,
{"matched": false, "stage": ..., "error": ...}, and exits 0, so the workbench can explain it.

The workbench copies this file into the project folder (compare/compare.py) the first time Compare
is set up. Edit that copy freely; Build > Compare setup shows the changes from this template.

Run by hand:
  python compare.py --file src/func.c --symbol func --target target.o --context context.h \
      --compiler C:/mwcc/2.0/sp1p5/mwccarm.exe --flags "-O4,p -proc arm946e" --objdiff objdiff-cli.exe
  python compare.py --self-test --compiler ... --flags ... --objdiff ...
Standard library only. Needs objdiff-cli 3.8 or newer.
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

LOG_LIMIT = 4000
OBJDIFF_MIN = (3, 8)

SELF_TEST_C = """\
int dswb_selftest(int count, int step)
{
    int sum = 0;
    while (count > 0) {
        sum += step;
        count--;
    }
    return sum;
}
"""


class Stop(Exception):
    """Ends the run at a stage with a message; becomes {"matched": false, "stage", "error"}."""

    def __init__(self, stage, error, log=""):
        super().__init__(error)
        self.stage = stage
        self.error = error
        self.log = log


class Deadline:
    def __init__(self, seconds):
        self.end = time.monotonic() + seconds

    def left(self, stage):
        t = self.end - time.monotonic()
        if t <= 0:
            raise Stop(stage, "timed out")
        return t


def tail(text):
    text = text or ""
    return text if len(text) <= LOG_LIMIT else "...\n" + text[-LOG_LIMIT:]


def run(cmd, stage, deadline, cwd=None):
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           stdin=subprocess.DEVNULL, timeout=deadline.left(stage))
    except FileNotFoundError:
        raise Stop(stage, "not found: %s" % cmd[0])
    except PermissionError as e:
        raise Stop(stage, "cannot run %s: %s" % (cmd[0], e))
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"") + (e.stderr or b"")
        raise Stop(stage, "timed out running %s" % os.path.basename(cmd[0]), tail(out.decode("utf-8", "replace")))
    out = p.stdout.decode("utf-8", "replace")
    err = p.stderr.decode("utf-8", "replace")
    return p.returncode, out, err


# ---------------------------------------------------------------- compile

def compiler_family(path):
    name = os.path.basename(path).lower()
    if "mwcc" in name:
        return "mwcc"
    if "gcc" in name or "clang" in name or name in ("cc", "cc.exe"):
        return "gcc"
    return "mwcc"


def split_flags(text):
    """Splits flags as a shell would: on spaces outside double quotes, the quotes removed.
    Backslashes stay as they are (Windows paths in -I flags)."""
    out, cur, quoted, have = [], [], False, False
    for c in text:
        if c == '"':
            quoted, have = not quoted, True
        elif c.isspace() and not quoted:
            if have:
                out.append("".join(cur))
            cur, have = [], False
        else:
            cur.append(c)
            have = True
    if have:
        out.append("".join(cur))
    return out


def compile_c(args, src, obj, deadline):
    flags = split_flags(args.flags or "")
    cmd = [args.compiler, "-c"]
    if args.context:
        cmd += ["-include", args.context]
    cmd += flags + ["-o", obj, src]
    code, out, err = run(cmd, "compile", deadline, cwd=os.path.dirname(os.path.abspath(src)))
    log = tail((out + err).strip())
    if code != 0 or not os.path.exists(obj):
        raise Stop("compile", "the compiler failed (exit %d)" % code, log)
    return log


# ---------------------------------------------------------------- extract (ELF symbol table)

def elf_symbols(path):
    """Names of the defined symbols in a 32-bit little-endian ELF object."""
    data = open(path, "rb").read()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF object")
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    if shoff == 0 or shentsize < 40 or shoff + shnum * shentsize > len(data):
        raise ValueError("bad section table")
    secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    names = {}
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        off, size, link, entsize = s[4], s[5], s[6], s[9] or 16
        if link >= len(secs) or off + size > len(data):
            continue
        stroff, strsize = secs[link][4], secs[link][5]
        for i in range(size // entsize):
            nm, value, sz, info, other, shndx = struct.unpack_from("<IIIBBH", data, off + i * entsize)
            if shndx == 0 or nm >= strsize:
                continue
            end = data.find(b"\0", stroff + nm)
            name = data[stroff + nm:end].decode("utf-8", "replace")
            if name and not name.startswith("$"):
                names[name] = (info & 0xF, sz)
    return names


def check_symbol(obj, symbol):
    """The compiled object's name for the function: the name itself, or its C++ name (CodeWarrior
    mangles a function f as f__F followed by its parameter types, a method as f__5Class...)."""
    try:
        syms = elf_symbols(obj)
    except (OSError, ValueError) as e:
        raise Stop("extract", "cannot read the compiled object: %s" % e)
    if symbol in syms:
        return symbol
    funcs = sorted(n for n, (t, _) in syms.items() if t == 2)
    parts = symbol.split("::")
    nested = "_ZN" + "".join("%d%s" % (len(p), p) for p in parts) + "E"
    mangled = [n for n in funcs
               if (n.startswith(symbol + "__") and len(n) > len(symbol) + 2)        # CodeWarrior 1.x: f__Fi
               or (len(parts) == 1 and n.startswith("_Z%d%s" % (len(symbol), symbol)))   # Itanium (2.0): _Z1fi
               or n.startswith(nested)]                                             # Itanium methods: _ZN5Class1fEv
    if len(mangled) == 1:
        return mangled[0]
    if len(mangled) > 1:
        raise Stop("extract", "'%s' is overloaded in the compiled object: %s" % (symbol, ", ".join(mangled[:8])),
                   "name the function by its C++ name in the knowledge store")
    near = sorted(funcs, key=lambda n: (n.lower() != symbol.lower(), symbol.lower() not in n.lower(), n))[:8]
    raise Stop("extract", "symbol '%s' is not in the compiled object" % symbol,
               "functions it defines: " + (", ".join(near) if near else "(none)"))


def rename_symbol(src, dst, old, new):
    """Copies an ELF object, renaming symbol old to new (the target object carries the knowledge
    store's name; a C++ compile defines the mangled one). The new name goes into a copy of the string
    table at the end of the file."""
    data = bytearray(open(src, "rb").read())
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    secs = [list(struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)) for i in range(shnum)]
    for s in secs:
        if s[1] != 2:
            continue
        strtab = secs[s[6]]
        table = bytes(data[strtab[4]:strtab[4] + strtab[5]])
        at = len(table)
        table += new.encode() + b"\0"
        renamed = False
        for i in range(s[5] // (s[9] or 16)):
            o = s[4] + i * (s[9] or 16)
            nm, = struct.unpack_from("<I", data, o)
            end = table.find(b"\0", nm)
            if table[nm:end].decode("utf-8", "replace") == old:
                struct.pack_into("<I", data, o, at)
                renamed = True
        if not renamed:
            raise Stop("diff", "the target object has no symbol '%s'" % old)
        data += b"\0" * (-len(data) % 4)
        strtab[4], strtab[5] = len(data), len(table)
        data += table
        struct.pack_into("<IIIIIIIIII", data, shoff + s[6] * shentsize, *strtab)
    open(dst, "wb").write(data)


# ---------------------------------------------------------------- diff (objdiff-cli)

def objdiff_version(path, deadline):
    code, out, err = run([path, "--version"], "diff", deadline)
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", out + err)
    if code != 0 or not m:
        raise Stop("diff", "objdiff-cli did not report a version", tail(out + err))
    v = tuple(int(x) for x in m.groups())
    if v[:2] < OBJDIFF_MIN:
        raise Stop("diff", "objdiff-cli %d.%d.%d is too old; %d.%d or newer is needed" % (v + OBJDIFF_MIN))
    return "objdiff-cli %d.%d.%d" % v


def find_symbol(side, symbol):
    for s in side.get("symbols", []):
        if s.get("name") == symbol:
            return s
    return None


def row_status(left, right):
    kind = left.get("diff_kind") or right.get("diff_kind") or ""
    if not kind or kind == "DIFF_NONE":
        return "match"
    if kind == "DIFF_INSERT":
        return "insert"
    if kind == "DIFF_DELETE":
        return "delete"
    if kind == "DIFF_ARG_MISMATCH":
        reloc = (left.get("instruction") or {}).get("relocation") or (right.get("instruction") or {}).get("relocation")
        return "reloc" if reloc else "arg"
    return "diff"


def diff_objects(args, target, compiled, symbol, deadline):
    engine = objdiff_version(args.objdiff, deadline)
    cmd = [args.objdiff, "diff", "-1", target, "-2", compiled, "-o", "-", "--format", "json", symbol]
    code, out, err = run(cmd, "diff", deadline)
    if code != 0:
        raise Stop("diff", "objdiff-cli failed (exit %d)" % code, tail(out + err))
    try:
        doc = json.loads(out)
    except ValueError:
        raise Stop("diff", "objdiff-cli printed no JSON", tail(out + err))
    left = find_symbol(doc.get("left", {}), symbol)
    right = find_symbol(doc.get("right", {}), symbol)
    if left is None:
        raise Stop("diff", "objdiff-cli found no '%s' in the target object" % symbol)
    if right is None:
        raise Stop("diff", "objdiff-cli found no '%s' in the compiled object" % symbol)
    li = left.get("instructions", [])
    ri = right.get("instructions", [])
    rows = []
    for i in range(max(len(li), len(ri))):
        a = li[i] if i < len(li) else {}
        b = ri[i] if i < len(ri) else {}
        rows.append({"orig": (a.get("instruction") or {}).get("formatted", ""),
                     "compiled": (b.get("instruction") or {}).get("formatted", ""),
                     "status": row_status(a, b)})
    score = float(left.get("match_percent", 0.0))
    return engine, score, rows


# ---------------------------------------------------------------- main

def absolute(path):
    """Paths that exist become absolute (the compiler runs in the source's folder); bare program
    names stay as they are, for a PATH lookup."""
    return os.path.abspath(path) if path and os.path.exists(path) else path


def compare(args):
    for name in ("file", "target", "context", "compiler", "objdiff"):
        setattr(args, name, absolute(getattr(args, name)))
    deadline = Deadline(args.timeout)
    work = args.work or tempfile.mkdtemp(prefix="dswb_compare_")
    os.makedirs(work, exist_ok=True)
    try:
        if args.self_test:
            src = os.path.join(work, "dswb_selftest.c")
            with open(src, "w", encoding="utf-8") as f:
                f.write(SELF_TEST_C)
            symbol, target, args.context = "dswb_selftest", None, ""
        else:
            for name in ("file", "symbol", "target"):
                if not getattr(args, name):
                    raise Stop("compile", "--%s is required" % name)
            src, symbol, target = args.file, args.symbol, args.target
            if not os.path.isfile(src):
                raise Stop("compile", "no source file: %s" % src)
            if not os.path.isfile(target):
                raise Stop("diff", "no target object: %s" % target)
        if not args.compiler:
            raise Stop("compile", "no compiler is configured for this module")
        if not args.objdiff:
            raise Stop("diff", "no objdiff-cli is configured")
        obj = os.path.join(work, "compiled.o")
        log = compile_c(args, src, obj, deadline)
        compiled_name = check_symbol(obj, symbol)
        if compiled_name != symbol:
            # the C++ name: the target object takes it too, so the diff pairs the two (the
            # self-test's target is the compiled object itself)
            if target:
                renamed = os.path.join(work, "target_renamed.o")
                rename_symbol(target, renamed, symbol, compiled_name)
                target = renamed
            symbol = compiled_name
            log = (log + "\n" if log else "") + "compared as its C++ name %s" % compiled_name
        engine, score, rows = diff_objects(args, target or obj, obj, symbol, deadline)
        return {"score": score, "matched": score >= 100.0, "compiler": args.compiler_label or os.path.basename(args.compiler),
                "flags": args.flags, "diff": rows, "recorded": False, "stage": "done", "log": log,
                "engine": engine, "source_file": "" if args.self_test else src}
    finally:
        if not args.work and not args.keep:
            shutil.rmtree(work, ignore_errors=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Compile one function and diff it against the original with objdiff-cli.")
    ap.add_argument("--file", help="the function's C source")
    ap.add_argument("--symbol", help="the function's name")
    ap.add_argument("--target", help="target object the workbench wrote (original bytes and relocations)")
    ap.add_argument("--context", default="", help="context header to prefix (types, prototypes, globals)")
    ap.add_argument("--compiler", default="", help="compiler executable (mwccarm.exe, or arm-none-eabi-gcc for tests)")
    ap.add_argument("--compiler-label", default="", help="compiler version label recorded with the result")
    ap.add_argument("--flags", default="", help="compiler flags")
    ap.add_argument("--objdiff", default="", help="objdiff-cli executable")
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds for the whole run")
    ap.add_argument("--work", default="", help="folder for the compiled object (default: a temporary one)")
    ap.add_argument("--keep", action="store_true", help="keep the temporary folder")
    ap.add_argument("--self-test", action="store_true", help="compile a built-in function against itself; expects 100")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        if e.code == 0:
            raise
        print(json.dumps({"matched": False, "score": 0.0, "stage": "compile", "error": "bad arguments to compare.py", "diff": []}))
        return 0
    try:
        result = compare(args)
    except Stop as e:
        result = {"matched": False, "score": 0.0, "stage": e.stage, "error": e.error, "log": e.log, "diff": [],
                  "compiler": args.compiler_label, "flags": args.flags}
    except Exception as e:  # anything unexpected still answers with the contract
        result = {"matched": False, "score": 0.0, "stage": "compile", "error": "compare.py failed: %r" % e, "diff": [],
                  "compiler": args.compiler_label, "flags": args.flags}
    sys.stdout.write(json.dumps(result) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
