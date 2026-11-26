"""Writes names given in the workbench into dsd-config, checks the build, and imports dsd-config again.

    python -m dswb.dsd_sync path/to/dsd-config              write, build, import (the workbench running with --api)
    python -m dswb.dsd_sync path/to/dsd-config --dry-run    only show what it would do

dsd-config owns the names. This is how a name given in the workbench gets there:

* Sources: functions, labels and named data that a person or an agent named in the knowledge
  store, and the names file (below). Rows of the dsd import itself, automatic sub_ names, vt_
  labels and other generated names are not names to write.
* Only a dsd default name (func_02001234, data_ov003_02121c68, ...) is replaced, or a name this
  command wrote before (it is in the names file), so a name can be changed again from the
  workbench. A name dsd has from elsewhere (the matcher, old_names.py, by hand) stays, and a
  different workbench name for that address is listed as a conflict.
* A name must be a valid symbol (letters, digits, _ $ .) and unused in every module of dsd-config.
* The names file (workbench_names.tsv next to dsd-config) records every name written, so a
  dsd-config made again from scratch gets them back (this command, or a project script that
  applies the file).
* Then the build runs (ninja, ninja check, when the project has a build.ninja). If it fails,
  every file is put back and nothing is imported.
* Last, dsd-config is imported into the workbench (as python -m dswb.dsd_import --apply).

Review the change with git diff, then commit dsd-config and the names file.
"""
import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from . import dsd

# dsd's default names (as dsd.Symbol.placeholder, without the address check: any of these is free)
DEFAULT = re.compile(r"^(?:func|data|bss)_(?:ov\d+_|autoload_\d+_|itcm_|dtcm_)?[0-9a-fA-F]{8}(?:_unk)?$|^\.L_|^_dsd")
# names tools make up from an address: never names to write
GENERATED = re.compile(r"^(?:sub|func|data|bss|vt|lbl|FUN|DAT)_(?:ov\d+_)?[0-9a-fA-F]{7,8}$")
VALID = re.compile(r"^[A-Za-z_$.][A-Za-z0-9_$.]*$")
TOOL_AUTHORS = {"dsd", "vtables"}
NAMES_HEADER = ("# Names given in the melonDS workbench and written into dsd-config by `python -m dswb.dsd_sync`.\n"
                "# Applied again after dsd-config is made from scratch. module, address, kind (function or data), name.\n")
_LINE = re.compile(r"^(\S+)\s+kind:(\w+)(?:\([^)]*\))?\s+addr:0x([0-9a-fA-F]+)")


@dataclass
class Entry:
    module: str             # main, itcm, dtcm, ov003
    addr: int
    kind: str               # function or data
    name: str
    source: str = ""        # who gave it: "names file", or the store's author


class SymFile:
    """One module's symbols.txt, kept as lines so a rename changes only the name."""

    def __init__(self, module: str, overlay: int, path: str):
        self.module, self.overlay, self.path = module, overlay, path
        raw = open(path, "rb").read()
        self.original = raw
        text = raw.decode("latin-1")
        self.nl = "\r\n" if "\r\n" in text else "\n"
        self.lines = text.split(self.nl)
        self.by_addr: Dict[int, List[int]] = {}
        for i, ln in enumerate(self.lines):
            m = _LINE.match(ln)
            if m:
                self.by_addr.setdefault(int(m.group(3), 16), []).append(i)
        self.changed = False

    def symbols_at(self, addr: int, kind: str) -> List[Tuple[int, str]]:
        """(line, name) of the symbols at addr of this kind: function, or data (data and bss)."""
        out = []
        for i in self.by_addr.get(addr, []):
            m = _LINE.match(self.lines[i])
            k = m.group(2)
            if (kind == "function" and k == "function") or (kind == "data" and k in ("data", "bss")):
                out.append((i, m.group(1)))
        return out

    def names(self) -> List[str]:
        return [m.group(1) for m in (_LINE.match(ln) for ln in self.lines) if m]

    def rename(self, line: int, old: str, new: str):
        assert self.lines[line].startswith(old)
        self.lines[line] = new + self.lines[line][len(old):]
        self.changed = True

    def save(self):
        with open(self.path, "wb") as f:
            f.write(self.nl.join(self.lines).encode("latin-1"))

    def restore(self):
        with open(self.path, "wb") as f:
            f.write(self.original)


def load_symbols(config: str) -> Dict[str, SymFile]:
    return {m.name: SymFile(m.name, m.overlay, os.path.join(m.path, "symbols.txt"))
            for m in dsd.load(config) if os.path.exists(os.path.join(m.path, "symbols.txt"))}


def names_path(config: str) -> str:
    """workbench_names.tsv in the folder that holds dsd-config."""
    root = os.path.dirname(os.path.abspath(dsd.arm9_dir(config)))
    if os.path.basename(root) == "dsd-config":
        root = os.path.dirname(root)
    return os.path.join(root, "workbench_names.tsv")


def read_names(path: str) -> List[Entry]:
    out = []
    if not os.path.exists(path):
        return out
    for ln in open(path, encoding="utf-8").read().splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        p = ln.split("\t")
        if len(p) >= 4:
            out.append(Entry(p[0], int(p[1], 16), p[2], p[3], "names file"))
    return out


def write_names(path: str, entries: List[Entry]):
    rows = {}
    for e in entries:
        rows[(e.module, e.addr, e.kind)] = e.name
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(NAMES_HEADER)
        for (m, a, k), n in sorted(rows.items(), key=lambda kv: (kv[0][0] != "main", kv[0][0], kv[0][1], kv[0][2])):
            f.write(f"{m}\t0x{a:08x}\t{k}\t{n}\n")


def module_for(files: Dict[str, SymFile], overlay: int, addr: int) -> str:
    if overlay >= 0:
        return "ov%03d" % overlay
    for m in ("main", "itcm", "dtcm"):
        if m in files and addr in files[m].by_addr:
            return m
    return "main"


def store_names(c, files: Dict[str, SymFile]) -> List[Entry]:
    """Names a person or an agent gave in the running workbench's store (ARM9)."""
    out = []

    def rows(kind):
        got, offset = [], 0
        while True:
            r = c.kb_list(kind, cpu="arm9", offset=offset, limit=100000)
            got += getattr(r, kind) or []
            if not r.truncated:
                return got
            offset = r.next_offset

    for kind, rk in (("functions", "function"), ("labels", "data"), ("data_objects", "data")):
        for x in rows(kind):
            if x.author_kind not in ("person", "agent") or x.author in TOOL_AUTHORS or not x.name or GENERATED.match(x.name):
                continue
            out.append(Entry(module_for(files, x.overlay, x.addr), x.addr, rk, x.name, x.author))
    return out


@dataclass
class Plan:
    write: List[Tuple[Entry, str]] = field(default_factory=list)       # (entry, old name)
    already: List[Entry] = field(default_factory=list)
    conflicts: List[Tuple[Entry, str]] = field(default_factory=list)   # dsd has another name it owns
    no_symbol: List[Entry] = field(default_factory=list)
    invalid: List[Tuple[Entry, str]] = field(default_factory=list)


def plan(files: Dict[str, SymFile], recorded: List[Entry], wanted: List[Entry]) -> Plan:
    """What to rename. recorded: the names file (names this command wrote before, which it may
    replace). wanted: the names file again first (to put its names back after dsd-config was
    made from scratch), then the store's names, which win over the file's for the same symbol."""
    p = Plan()
    ours = {(e.module, e.addr, e.kind): e.name for e in recorded}
    used = {n for f in files.values() for n in f.names()}
    # the last wanted name per symbol counts; the store's come after the file's
    final: Dict[Tuple[str, int, str], Entry] = {}
    for e in wanted:
        final[(e.module, e.addr, e.kind)] = e
    for key, e in sorted(final.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        if not VALID.match(e.name) or DEFAULT.match(e.name) or GENERATED.match(e.name):
            p.invalid.append((e, "not a name to write (not a valid symbol, or one made up from an address)"))
            continue
        f = files.get(e.module)
        syms = f.symbols_at(e.addr, e.kind) if f else []
        if any(n == e.name for _, n in syms):
            p.already.append(e)
            continue
        free = [(i, n) for i, n in syms if DEFAULT.match(n) or ours.get(key) == n]
        if not syms:
            p.no_symbol.append(e)
            continue
        if len(free) != 1:
            p.conflicts.append((e, ", ".join(n for _, n in syms)))
            continue
        line, old = free[0]
        if e.name in used:
            p.invalid.append((e, "the name is already used in dsd-config"))
            continue
        f.rename(line, old, e.name)
        used.add(e.name)
        p.write.append((e, old))
    return p


def plan_text(p: Plan, limit: int = 20) -> str:
    def where(e):
        return f"{e.module} 0x{e.addr:08x} {e.kind}"
    lines = [f"{len(p.write)} names to write into dsd-config, {len(p.already)} already there"]
    for e, old in p.write[:limit]:
        lines.append(f"    {where(e)}: {old} -> {e.name}  (from {e.source})")
    for title, items in (("conflicts: dsd has its own name there (dsd's stays; rename it in symbols.txt if the workbench name is right)", p.conflicts),
                         ("not written", p.invalid)):
        if items:
            lines.append(f"{len(items)} {title}:")
            lines += [f"    {where(e)}: {e.name} (from {e.source}): {why}" for e, why in items[:limit]]
    if p.no_symbol:
        lines.append(f"{len(p.no_symbol)} names at an address where dsd-config has no symbol of that kind (add one to symbols.txt by hand):")
        lines += [f"    {where(e)}: {e.name} (from {e.source})" for e in p.no_symbol[:limit]]
    return "\n".join(lines)


def build(project: str, command: Optional[str]) -> Tuple[bool, str]:
    """Runs the build check in the project folder. command None: ninja, then ninja check, when the
    project has a build.ninja; otherwise nothing is run."""
    if command is None:
        if not os.path.exists(os.path.join(project, "build.ninja")):
            return True, "no build.ninja in %s: the build was not checked" % project
        steps = [["ninja"], ["ninja", "check"]]
    else:
        steps = [command]
    log = []
    for s in steps:
        p = subprocess.run(s, cwd=project, capture_output=True, text=True, shell=isinstance(s, str))
        log.append(("$ " + (s if isinstance(s, str) else " ".join(s))) + "\n" + (p.stdout + p.stderr).strip()[-2000:])
        if p.returncode != 0:
            return False, "\n".join(log)
    return True, "\n".join(log)


def write(files: Dict[str, SymFile], p: Plan, recorded: List[Entry], npath: str, project: str,
          build_cmd: Optional[str] = None, check_build: bool = True) -> bool:
    """Saves the renamed symbols.txt files and the names file, then checks the build. When the
    check fails every file is put back as it was. Returns whether the names are in place."""
    if not p.write:
        return True
    names_before = open(npath, "rb").read() if os.path.exists(npath) else None
    for f in files.values():
        if f.changed:
            f.save()
    write_names(npath, recorded + [e for e, _ in p.write])
    print("wrote", ", ".join(os.path.relpath(f.path, project) for f in files.values() if f.changed), "and", os.path.relpath(npath, project))
    if not check_build:
        return True
    ok, log = build(project, build_cmd)
    print(log)
    if ok:
        return True
    for f in files.values():
        if f.changed:
            f.restore()
    if names_before is None:
        os.remove(npath)
    else:
        with open(npath, "wb") as fh:
            fh.write(names_before)
    print("the build check failed: every file is back as it was")
    return False


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m dswb.dsd_sync", description=(__doc__ or "").split("\n")[0])
    ap.add_argument("config", help="the dsd-config folder")
    ap.add_argument("--dry-run", action="store_true", help="show what it would write; change nothing")
    ap.add_argument("--names-file", help="default: workbench_names.tsv in the folder that holds dsd-config")
    ap.add_argument("--build", help="build check command run in the project folder (default: ninja, then ninja check)")
    ap.add_argument("--no-build", action="store_true", help="do not check the build")
    ap.add_argument("--no-import", action="store_true", help="do not import dsd-config into the workbench afterwards")
    ap.add_argument("--report", help="the import's full review (JSON)")
    ap.add_argument("--info-file", default=None, help="api.json of the running melonDS, when not in the default place")
    a = ap.parse_args(argv)

    from .client import Client
    from . import dsd_import
    config = os.path.abspath(a.config)
    project = os.path.dirname(names_path(config))
    npath = os.path.abspath(a.names_file) if a.names_file else names_path(config)
    files = load_symbols(config)
    recorded = read_names(npath)
    with Client.connect(a.info_file, timeout=600.0, agent="dsd-sync") as c:
        p = plan(files, recorded, recorded + store_names(c, files))
        print(plan_text(p))
        if a.dry_run:
            print("dry run: nothing written")
            return 0
        if not write(files, p, recorded, npath, project, a.build, not a.no_build):
            print("nothing was imported")
            return 1
        if a.no_import:
            return 0
        ok = dsd_import.apply(c, config, os.path.join(project, "build", "dsd_import.json") if os.path.isdir(os.path.join(project, "build"))
                              else os.path.join(config, "..", "dsd_import.json"), a.report)
    if p.write:
        print("review with git diff, then commit dsd-config and %s" % os.path.basename(npath))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
