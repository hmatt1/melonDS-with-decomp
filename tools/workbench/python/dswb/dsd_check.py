"""Where recordings disagree with dsd's function list (ARM9).

    python -m dswb.dsd_check --dsd path/to/dsd-config --sessions path/to/sessions [-o check.json]

dsd finds functions by reading the code; the recorder sees what runs. This lists what the
recordings saw that dsd's function list does not explain:

  came back        a call into the middle of a dsd function that returned to its call site: a
                   function probably starts there that dsd merged into the one before it
  jumped between   a bl that never came back (Thumb code uses bl for long jumps) from one dsd
                   function into the middle of another: the two are probably one function
  tail between     a plain branch the recorder took for a tail call, from one dsd function into the
                   middle of another: the same, weaker
  unknown          calls into the middle from older recordings, which do not say whether they returned
  called outside   a call target in a dsd module but in no dsd function, or in no dsd module
  ran outside      code that ran in a dsd module but in no dsd function

A jump or branch that stays inside one dsd function is normal and only counted.

Each needs a look before symbols.txt changes; the report gives the call sites and counts.
"""
import argparse
import glob
import json
import os
import sys
from typing import Any, Dict, List, Optional, Tuple

from . import dsd
from . import session as sessionmod
from ._generated import Region

try:
    import numpy as np
except ImportError:          # the executed-code check needs numpy; the rest does not
    np = None


class Modules:
    def __init__(self, mods: List[dsd.Module]):
        self.mods = [m for m in mods if m.sections]
        self.by = {m.name: m for m in self.mods}
        self.funcs = {}
        for m in self.mods:
            fs = sorted(m.functions(), key=lambda s: s.addr)
            self.funcs[m.name] = ([s.addr for s in fs], fs)

    def containing_modules(self, addr: int) -> List[dsd.Module]:
        return [m for m in self.mods if m.contains(addr)]

    def resolve(self, addr: int, overlay: Optional[int]) -> Tuple[Optional[str], str]:
        """(module name, how): how is "named" (the recording said which overlay), "only" (one module
        has this address), "start" (several overlays share it and one has a function starting
        there), "ambiguous", or "none" (no dsd module)."""
        if overlay is not None:
            n = "ov%03d" % overlay
            return (n, "named") if n in self.by and self.by[n].contains(addr) else (None, "none")
        ms = self.containing_modules(addr)
        if not ms:
            return None, "none"
        if len(ms) == 1:
            return ms[0].name, "only"
        starting = [m.name for m in ms if addr in self.funcs[m.name][0]]
        if len(starting) == 1:
            return starting[0], "start"
        return None, "ambiguous"

    def function(self, module: str, addr: int) -> Optional[dsd.Symbol]:
        starts, fs = self.funcs[module]
        import bisect
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return None
        f = fs[i]
        return f if addr < f.addr + (f.size or 0) else None


def _ov(v: int) -> Optional[int]:
    """CallEdge site_overlay / callee_overlay to an ARM9 overlay id (None: not recorded or static)."""
    if not v or (v - 1) & 0x8000:
        return None
    return (v - 1) & 0x7FFF


def _where(mm: Modules, module: Optional[str], addr: int) -> str:
    if not module:
        return "%08x" % addr
    f = mm.function(module, addr)
    if not f:
        return "%08x (%s, in no function)" % (addr, module)
    return "%08x (%s+0x%x)" % (addr, f.name, addr - f.addr) if addr != f.addr else "%08x (%s)" % (addr, f.name)


def check(mods: List[dsd.Module], session_files: List[str], executed: bool = True) -> Dict[str, Any]:
    mm = Modules(mods)
    targets: Dict[Tuple[int, Optional[int], int], Dict[str, Any]] = {}
    ran: Dict[str, set] = {}
    for p in session_files:
        s = sessionmod.load(p)
        for e in s.call_edges():
            if e.cpu != 0 or e.kind not in (0, 3) or e.callee >= 0xFFFF0000:
                continue
            key = (e.callee & ~1, _ov(e.callee_overlay), e.kind)
            t = targets.setdefault(key, {"count": 0, "returns": 0, "sites": {}})
            t["count"] += e.count
            t["returns"] |= getattr(e, "returns", 0)
            sk = (e.call_site, _ov(e.site_overlay))
            t["sites"][sk] = t["sites"].get(sk, 0) + e.count
        if executed and np is not None:
            for region, page, raw in s.pages():
                if region == Region.MainRAM:
                    base, names = 0x02000000, None
                elif Region.OverlayARM9Base <= region < Region.OverlayARM7Base:
                    base, names = 0x02000000, ["ov%03d" % (region - Region.OverlayARM9Base)]
                elif region == Region.ITCM:
                    base, names = 0x01FF8000, ["itcm"]
                else:
                    continue
                ex = np.frombuffer(raw, dtype=np.uint32).reshape(4096, 8)[:, 0]
                idx = np.nonzero(ex)[0]
                if not len(idx):
                    continue
                start = base + (page << 12)
                for i in idx[(idx & 1) == 0]:      # even addresses are enough: instructions are 2 or 4 bytes
                    a = start + int(i)
                    cands = names or [m.name for m in mm.containing_modules(a) if m.overlay < 0]
                    for n in cands:
                        if n in mm.by and mm.by[n].contains(a) and any(sec.kind == "code" and sec.start <= a < sec.end for sec in mm.by[n].sections):
                            ran.setdefault(n, set()).add(a & ~1)

    out: Dict[str, Any] = {"sessions": len(session_files), "targets_at_a_start": 0, "ambiguous": 0,
                           "came_back": [], "jumped_between": [], "unknown": [], "tail_between": [], "called_outside": [],
                           "ran_outside": [], "within_one_function": 0}
    for (callee, cov, kind), t in sorted(targets.items(), key=lambda kv: (kv[0][0], -1 if kv[0][1] is None else kv[0][1], kv[0][2])):
        module, how = mm.resolve(callee, cov)
        if how == "ambiguous":
            out["ambiguous"] += 1
            continue
        if module and callee in mm.funcs[module][0]:
            out["targets_at_a_start"] += 1
            continue
        returned = "yes" if t["returns"] & 1 else ("no" if t["returns"] & 2 else "unknown")
        f = mm.function(module, callee) if module else None
        sites, other = [], False
        for (site, sov), n in sorted(t["sites"].items(), key=lambda kv: -kv[1]):
            sm, _ = mm.resolve(site, sov)
            sf = mm.function(sm, site) if sm else None
            same = f is not None and sf is not None and sm == module and sf.addr == f.addr
            other = other or not same
            sites.append({"site": "0x%08x" % site, "in": _where(mm, sm, site), "count": n, "same_function": same})
        e = {"target": "0x%08x" % callee, "module": module, "where": _where(mm, module, callee), "count": t["count"],
             "returned": returned, "kind": "tail call" if kind == 3 else "call", "sites": sites[:8]}
        if f is None:
            out["called_outside"].append(e)
        elif kind == 0 and returned == "yes":
            out["came_back"].append(e)                  # a real call: a function probably starts here
        elif not other:
            out["within_one_function"] += 1             # a jump or branch inside one function: fine
        elif kind == 3:
            out["tail_between"].append(e)
        elif returned == "no":
            out["jumped_between"].append(e)             # bl as a jump from another function: probably one function
        else:
            out["unknown"].append(e)
    for n, addrs in sorted(ran.items()):
        runs, cur = [], None
        for a in sorted(addrs):
            if mm.function(n, a):
                continue
            if cur and a <= cur[1] + 4:
                cur[1] = a + 2
            else:
                cur = [a, a + 2]
                runs.append(cur)
        out["ran_outside"] += [{"module": n, "start": "0x%08x" % s, "end": "0x%08x" % e} for s, e in runs]
    return out


def summary(r: Dict[str, Any], limit: int = 12) -> str:
    lines = [f"{r['sessions']} sessions: {r['targets_at_a_start']} call targets start a dsd function; "
             f"{r['within_one_function']} jumps or branches stay inside one dsd function (fine)"
             + (f"; {r['ambiguous']} in code two overlays share could not be placed" if r["ambiguous"] else "")]

    def block(title, items):
        if not items:
            return
        lines.append(f"{len(items)} {title}")
        for e in items[:limit]:
            lines.append(f"    {e['where']}  x{e['count']}  {e['kind']}, returned {e['returned']}, from "
                         + ", ".join(x["in"] for x in e["sites"][:3]))
        if len(items) > limit:
            lines.append(f"    ... and {len(items) - limit} more")

    block("calls into the middle of a dsd function that came back: a function probably starts there", r["came_back"])
    block("bl jumps from one dsd function into the middle of another: the two are probably one function", r["jumped_between"])
    block("branches the recorder took for tail calls, from one dsd function into the middle of another", r["tail_between"])
    block("calls into the middle of a dsd function from older recordings (whether they came back is not known)", r["unknown"])
    block("call targets in no dsd function", r["called_outside"])
    if r["ran_outside"]:
        lines.append(f"{len(r['ran_outside'])} ranges of code ran in no dsd function")
        for x in r["ran_outside"][:limit]:
            lines.append(f"    {x['module']} {x['start']}-{x['end']}")
    if not any(r[k] for k in ("came_back", "jumped_between", "tail_between", "unknown", "called_outside", "ran_outside")):
        lines.append("nothing the recordings saw contradicts dsd's function list")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m dswb.dsd_check", description=(__doc__ or "").split("\n")[0])
    ap.add_argument("--dsd", required=True, help="the dsd-config folder")
    ap.add_argument("--sessions", required=True, help="a folder of saved sessions (*.dswbs), or one session file")
    ap.add_argument("-o", "--out", help="the full report (JSON)")
    ap.add_argument("--no-executed", action="store_true", help="skip the check of code that ran outside every function")
    a = ap.parse_args(argv)
    files = [a.sessions] if os.path.isfile(a.sessions) else sorted(glob.glob(os.path.join(a.sessions, "*.dswbs")))
    if not files:
        print("no *.dswbs under", a.sessions, file=sys.stderr)
        return 1
    if np is None and not a.no_executed:
        print("numpy is not installed: skipping the check of code that ran outside every function", file=sys.stderr)
    r = check(dsd.load(a.dsd), files, executed=not a.no_executed)
    print(summary(r))
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(r, f, indent=1)
        print("wrote", os.path.abspath(a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
