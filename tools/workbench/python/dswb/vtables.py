"""Finds C++ vtables in a game from its dsd configuration, and ties them to what saved sessions saw.

    python -m dswb.vtables --dsd path/to/dsd-config --extract path/to/extract --sessions path/to/sessions -o report.json
    python -m dswb.vtables --dsd ... --extract ... -o report.json                 (static part only)
    python -m dswb.vtables ... --labels labels.json     (vt_<address> labels, for File > Import static analysis)

Static part, per module: a table is a run of words in .data or .rodata that dsd marks as pointers to
function starts. A vtable has two zero words in front of it (the offset and the empty type-info slot).
The functions whose literal pool holds a table's address are its constructors and destructors.

Runtime part, from the sessions: which slots were read and by what code, which code stored a table's
address into an object, and for each indirect call (blx/bx to a register) which table and slot it went
through. A call site is tied to a slot in two ways:

  decoded   the instructions before the call load the register from [reg, #offset]; the offset is the
            slot, and the tables with the header that hold the observed target at that slot are the
            site's tables
  readers   the session keeps the last two code addresses that read each word; a slot holding the
            target whose reader lies just before the call was the one used
"""
import argparse
import bisect
import glob
import json
import os
import struct
import sys
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Set, Tuple

from . import dsd
from . import session as sessionmod
from ._generated import Region

READER_WINDOW = 24      # a slot's reader this many bytes or fewer before the call is the call's load
SCAN_BACK = 16          # instructions looked at before a call for the load of its register


# ---------------------------------------------------------------- images and modules

@dataclass
class Image:
    base: int
    data: bytes

    def has(self, addr: int, size: int = 4) -> bool:
        return self.base <= addr and addr + size <= self.base + len(self.data)

    def u32(self, addr: int) -> int:
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def u16(self, addr: int) -> int:
        return struct.unpack_from("<H", self.data, addr - self.base)[0]


def image_path(extract: str, mod: dsd.Module) -> str:
    if mod.overlay >= 0:
        return os.path.join(extract, "arm9_overlays", "ov%03d.bin" % mod.overlay)
    return os.path.join(extract, "arm9", "arm9.bin" if mod.name == "main" else mod.name + ".bin")


def load_images(extract: str, mods: List[dsd.Module]) -> Dict[str, Image]:
    out = {}
    for m in mods:
        p = image_path(extract, m)
        if not m.sections or not os.path.exists(p):
            continue
        with open(p, "rb") as f:
            out[m.name] = Image(min(s.start for s in m.sections), f.read())
    return out


class Functions:
    """A module's functions by address."""

    def __init__(self, mod: dsd.Module):
        self.list = sorted(mod.functions(), key=lambda s: s.addr)
        self.starts = [s.addr for s in self.list]
        self.by_addr = {s.addr: s for s in self.list}

    def containing(self, addr: int) -> Optional[dsd.Symbol]:
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        s = self.list[i]
        return s if addr < s.addr + (s.size or 0) else None


def reloc_module(mods: Dict[str, dsd.Module], r: dsd.Reloc) -> Optional[str]:
    """Name of the module a reference points into, None when dsd could not decide."""
    if r.to_module == "overlay" and len(r.to_overlays) == 1:
        name = "ov%03d" % r.to_overlays[0]
        return name if name in mods else None
    return r.to_module if r.to_module in mods else None


# ---------------------------------------------------------------- static part

@dataclass
class Table:
    module: str
    overlay: int
    addr: int                       # the first function pointer: the address objects hold
    slots: List[int]                # function address per slot
    slot_modules: List[str]         # the module each function is in
    header: bool                    # two zero words in front
    constructors: List[Tuple[str, int, int]] = field(default_factory=list)      # module, function, the literal's address
    reads: List[int] = field(default_factory=list)
    readers: List[Set[int]] = field(default_factory=list)
    stores: Dict[int, int] = field(default_factory=dict)                          # storing pc -> count
    sites: Set[Tuple[str, int]] = field(default_factory=set)                      # call sites tied by readers
    sites_decoded: Set[Tuple[str, int]] = field(default_factory=set)

    @property
    def candidate(self) -> bool:
        return self.header and len(self.slots) >= 2

    @property
    def end(self) -> int:
        return self.addr + 4 * len(self.slots)


def find_tables(mods: List[dsd.Module], images: Dict[str, Image]) -> List[Table]:
    byname = dsd.by_name(mods)
    funcs = {m.name: Functions(m) for m in mods}
    tables: List[Table] = []
    for m in mods:
        img = images.get(m.name)
        secs = [s for s in m.sections if s.name in (".data", ".rodata")]
        if not secs:
            continue
        # every word dsd marks as a pointer to a function start
        ptr: Dict[int, Tuple[int, str]] = {}
        loads: Set[int] = set()
        for r in m.relocs:
            if r.kind != "load" or not any(s.start <= r.frm < s.end for s in secs):
                continue
            loads.add(r.frm)
            tm = reloc_module(byname, r)
            if tm and (r.to & ~1) in funcs[tm].by_addr:
                ptr[r.frm] = (r.to & ~1, tm)
        run: List[int] = []
        for a in sorted(ptr) + [None]:
            if run and a is not None and a == run[-1] + 4:
                run.append(a)
                continue
            if run:
                start = run[0]
                sec = next(s for s in secs if s.start <= start < s.end)
                header = bool(img and start - 8 >= sec.start and img.has(start - 8, 8) and start - 8 not in loads and start - 4 not in loads
                              and img.u32(start - 8) == 0 and img.u32(start - 4) == 0)
                t = Table(m.name, m.overlay, start, [ptr[x][0] for x in run], [ptr[x][1] for x in run], header)
                t.reads = [0] * len(run)
                t.readers = [set() for _ in run]
                tables.append(t)
            run = [a] if a is not None else []
    # constructors and destructors: code whose literal pool holds a table's address
    starts = {(t.module, t.addr): t for t in tables}
    for m in mods:
        code = [s for s in m.sections if s.kind == "code"]
        for r in m.relocs:
            if r.kind != "load" or not any(s.start <= r.frm < s.end for s in code):
                continue
            tm = reloc_module(byname, r)
            t = starts.get((tm, r.to)) if tm else None
            if not t:
                continue
            f = funcs[m.name].containing(r.frm)
            t.constructors.append((m.name, f.addr if f else 0, r.frm))
    return tables


# ---------------------------------------------------------------- instruction forms

def is_indirect_call(instr: int, thumb: bool) -> bool:
    """blx reg or bx reg."""
    if thumb:
        return (instr & 0xFF80) == 0x4780 or (instr & 0xFF80) == 0x4700
    return (instr & 0x0FFFFFF0) == 0x012FFF30 or (instr & 0x0FFFFFF0) == 0x012FFF10


def _arm_writes(instr: int, reg: int) -> Optional[Tuple[str, int, int]]:
    """None if this ARM instruction does not write reg; else (shape, base register, offset)."""
    if (instr & 0x0FFFFFD0) == 0x012FFF10:                      # bx / blx reg
        return None
    cls = (instr >> 25) & 7
    if cls in (2, 3):                                           # ldr / str / ldrb / strb
        if not instr & 0x00100000:
            return None
        rd, rn = (instr >> 12) & 15, (instr >> 16) & 15
        writeback = rn == reg and (not instr & 0x01000000 or instr & 0x00200000)
        if rd != reg:
            return ("other", rn, 0) if writeback else None
        if instr & 0x00400000:
            return ("other", rn, 0)                             # ldrb
        if rn == 15:
            return ("literal", rn, 0)
        if cls == 3:
            return ("indexed", rn, 0)
        if not instr & 0x01000000 or instr & 0x00200000:
            return ("other", rn, 0)                             # post-indexed or writeback
        off = instr & 0xFFF
        return ("slot", rn, off if instr & 0x00800000 else -off)
    if cls == 4:                                                # ldm / stm
        return ("other", 0, 0) if instr & 0x00100000 and instr & (1 << reg) else None
    if cls in (0, 1):
        if (instr & 0x0FC000F0) == 0x00000090:                  # mul / mla
            return ("other", 0, 0) if (instr >> 16) & 15 == reg else None
        if (instr & 0x0E000090) == 0x00000090 and cls == 0:     # ldrh / ldrsb / ldrsh / strh
            if instr & 0x00100000 and (instr >> 12) & 15 == reg:
                return ("other", 0, 0)
            return None
        op = (instr >> 21) & 15
        if 8 <= op <= 11:                                       # tst, teq, cmp, cmn (and mrs/msr)
            return None
        return ("other", 0, 0) if (instr >> 12) & 15 == reg else None
    return None


def _thumb_writes(instr: int, reg: int) -> Optional[Tuple[str, int, int]]:
    lo, top5 = instr & 7, instr >> 11
    if top5 == 0x0D:                                            # ldr rd, [rn, #imm5 * 4]
        return ("slot", (instr >> 3) & 7, ((instr >> 6) & 0x1F) * 4) if lo == reg else None
    if top5 == 0x09:                                            # ldr rd, [pc, #imm8 * 4]
        return ("literal", 15, 0) if (instr >> 8) & 7 == reg else None
    if top5 == 0x13:                                            # ldr rd, [sp, #imm8 * 4]
        return ("stack", 13, (instr & 0xFF) * 4) if (instr >> 8) & 7 == reg else None
    if (instr & 0xFE00) == 0x5800:                              # ldr rd, [rn, rm]
        return ("indexed", (instr >> 3) & 7, 0) if lo == reg else None
    if (instr & 0xF000) == 0x5000:                              # other register-offset loads and stores
        return ("other", 0, 0) if instr & 0x0800 or (instr & 0x0E00) == 0x0600 else None if lo != reg else ("other", 0, 0)
    if top5 in (0x0F, 0x11):                                    # ldrb / ldrh with immediate
        return ("other", 0, 0) if lo == reg else None
    if top5 <= 0x03:                                            # shifts, add/sub 3-register
        return ("other", 0, 0) if lo == reg else None
    if top5 in (0x04, 0x06, 0x07):                              # mov / add / sub rd, #imm8
        return ("other", 0, 0) if (instr >> 8) & 7 == reg else None
    if (instr & 0xFC00) == 0x4000:                              # ALU
        op = (instr >> 6) & 15
        return ("other", 0, 0) if lo == reg and op not in (8, 10, 11) else None
    if (instr & 0xFC00) == 0x4400:                              # high-register add / cmp / mov
        op = (instr >> 8) & 3
        if op in (0, 2) and (((instr >> 4) & 8) | lo) == reg:
            return ("mov", (instr >> 3) & 15, 0) if op == 2 else ("other", 0, 0)
        return None
    if top5 in (0x14, 0x15):                                    # add rd, pc / sp
        return ("other", 0, 0) if (instr >> 8) & 7 == reg else None
    if (instr & 0xFE00) == 0xBC00:                              # pop
        return ("stack", 13, 0) if reg < 8 and instr & (1 << reg) else None
    return None


def decode_site(img: Image, site: int, thumb: bool, func_start: int) -> Dict[str, Any]:
    """How the register of the indirect call at `site` was loaded.

    shape: slot (from [reg, #offset]: a table slot, the virtual-call shape), literal (a constant
    function pointer), indexed (a table indexed by a register), stack, other, unknown (no writer in
    the instructions looked at). For slot, vptr_offset is the offset the table pointer itself was
    loaded from, when the instruction before shows it."""
    step = 2 if thumb else 4
    read = img.u16 if thumb else img.u32
    writes = _thumb_writes if thumb else _arm_writes
    instr = read(site)
    reg = (instr >> 3) & 15 if thumb else instr & 15
    out: Dict[str, Any] = {"shape": "unknown", "register": reg}

    def last_writer(start: int, r: int):
        a = start
        for _ in range(SCAN_BACK):
            a -= step
            if a < func_start or not img.has(a, step):
                return None
            w = writes(read(a), r)
            if w and w[0] == "mov" and w[1] != r:              # mov r, rs: follow rs from here
                r = w[1]
                continue
            if w:
                return a, w
        return None

    hit = last_writer(site, reg)
    if not hit:
        return out
    a, (shape, base, off) = hit
    out["shape"] = shape
    if shape != "slot":
        return out
    out["slot_offset"] = off
    out["load_addr"] = a
    up = last_writer(a, base)
    if up and up[1][0] == "slot":
        out["vptr_offset"] = up[1][2]
    return out


# ---------------------------------------------------------------- runtime part

@dataclass
class Site:
    module: str
    addr: int
    thumb: bool
    func: int
    targets: Dict[int, int] = field(default_factory=dict)       # callee -> count
    decode: Dict[str, Any] = field(default_factory=dict)
    by_readers: Dict[int, List[Tuple[int, int]]] = field(default_factory=dict)   # callee -> [(table index, slot)]
    by_decode: Dict[int, List[Tuple[int, int]]] = field(default_factory=dict)


class Runtime:
    def __init__(self, mods: List[dsd.Module], images: Dict[str, Image], tables: List[Table]):
        self.mods = mods
        self.images = images
        self.tables = tables
        self.funcs = {m.name: Functions(m) for m in mods}
        self.code = {m.name: [(s.start, s.end) for s in m.sections if s.kind == "code"] for m in mods}
        self.overlays = [m for m in mods if m.overlay >= 0 and m.sections]
        self.edges: Dict[Tuple[str, int, int], int] = {}        # (module, site, callee) -> count
        self.unplaced = 0                                       # edges from an address two overlays share, not attributable
        self.sessions = 0
        self.history_entries = 0
        # table words by (region, page)
        self.words: Dict[Tuple[int, int], List[Tuple[int, int, int]]] = {}       # -> (index in page, table index, slot)
        for ti, t in enumerate(tables):
            region = Region.MainRAM if t.overlay < 0 else Region.OverlayARM9Base + t.overlay
            for k in range(len(t.slots)):
                off = (t.addr + 4 * k) & 0x00FFFFFF
                self.words.setdefault((int(region), off >> 12), []).append(((off & 0xFFF) >> 2, ti, k))
        self.starts: Dict[int, List[int]] = {}
        for ti, t in enumerate(tables):
            self.starts.setdefault(t.addr, []).append(ti)

    def in_code(self, name: str, addr: int) -> bool:
        return any(a <= addr < b for a, b in self.code[name])

    def add_session(self, path: str):
        s = sessionmod.load(path)
        self.sessions += 1
        # byte record pages by (region, page)
        pages: Dict[Tuple[int, int], memoryview] = {(r, p): raw for r, p, raw in s.pages()}

        def execd(region: int, addr: int) -> bool:
            raw = pages.get((region, (addr & 0x00FFFFFF) >> 12))
            return bool(raw) and struct.unpack_from("<I", raw, (addr & 0xFFF) * 32)[0] > 0

        for e in s.call_edges():
            if e.cpu != 0 or e.kind != 0:
                continue
            site = e.call_site
            if self.in_code("main", site):
                name = "main"
            elif e.site_overlay and not (e.site_overlay - 1) & 0x8000 and "ov%03d" % (e.site_overlay - 1) in self.code:
                name = "ov%03d" % (e.site_overlay - 1)          # the edge says which overlay (schema 2 sessions)
            else:
                # code two overlays share: the overlay that ran this address in this session, when only one did
                ran = [m.name for m in self.overlays if self.in_code(m.name, site) and execd(int(Region.OverlayARM9Base) + m.overlay, site)]
                if len(ran) != 1:
                    if any(self.in_code(m.name, site) for m in self.overlays):
                        self.unplaced += 1
                    continue
                name = ran[0]
            key = (name, site, e.callee & ~1)
            self.edges[key] = self.edges.get(key, 0) + e.count

        for key, words in self.words.items():
            raw = pages.get(key)
            if raw:
                for idx, ti, k in words:
                    self.tables[ti].reads[k] += struct.unpack_from("<I", raw, idx * 4 * 32 + 4)[0]
        for region, page, rd in s.readers():
            for idx, ti, k in self.words.get((region, page), ()):
                for pc in (rd[2 * idx], rd[2 * idx + 1]):
                    if pc and not pc & 1:
                        self.tables[ti].readers[k].add(pc)

        # stores of a table's address: 4-byte ARM9 writes
        hist = os.path.splitext(path)[0] + ".dswbh"
        if os.path.exists(hist):
            data = sessionmod.load_history(hist).data
        else:
            sec = s.section(sessionmod.SessionSection.WriteHistory)
            data = sec[1][:sec[0].count * 32] if sec else memoryview(b"")
        w = data.cast("I")
        n = len(w) // 8
        self.history_entries += n
        starts = self.starts
        for i, v in enumerate(w[6::8]):
            if v in starts and (w[i * 8 + 7] & 0xFFFF) == 0x0400:
                pc = w[i * 8 + 3]
                for ti in starts[v]:
                    st = self.tables[ti].stores
                    st[pc] = st.get(pc, 0) + 1

    def sites(self) -> List[Site]:
        """The indirect call sites among the merged edges, tied to tables."""
        slot_of: Dict[int, List[Tuple[int, int]]] = {}
        for ti, t in enumerate(self.tables):
            for k, f in enumerate(t.slots):
                slot_of.setdefault(f, []).append((ti, k))
        out: Dict[Tuple[str, int], Site] = {}
        for (name, addr, callee), count in self.edges.items():
            st = out.get((name, addr))
            if not st:
                img = self.images.get(name)
                f = self.funcs[name].containing(addr)
                if not img or not f or not f.mode:
                    continue
                thumb = f.mode == "thumb"
                if not img.has(addr, 2 if thumb else 4) or not is_indirect_call(img.u16(addr) if thumb else img.u32(addr), thumb):
                    continue
                st = out[(name, addr)] = Site(name, addr, thumb, f.addr)
                st.decode = decode_site(img, addr, thumb, f.addr)
            if callee >= 0xFFFF0000:        # an interrupt taken here, recorded as a call by older workbench builds
                continue
            st.targets[callee] = st.targets.get(callee, 0) + count
        res = []
        for st in out.values():
            if not st.targets:
                continue
            slot = st.decode.get("slot_offset")
            for callee in st.targets:
                cands = [(ti, k) for ti, k in slot_of.get(callee, ())
                         if st.module == "main" or self.tables[ti].overlay < 0 or self.tables[ti].module == st.module]
                rd = [(ti, k) for ti, k in cands if any(0 < st.addr - pc <= READER_WINDOW for pc in self.tables[ti].readers[k])]
                if rd:
                    st.by_readers[callee] = rd
                    for ti, _ in rd:
                        self.tables[ti].sites.add((st.module, st.addr))
                if slot is not None and slot >= 0 and slot % 4 == 0:
                    dc = [(ti, k) for ti, k in cands if k == slot // 4 and self.tables[ti].header]     # a vtable, not a callback table
                    if dc:
                        st.by_decode[callee] = dc
                        for ti, _ in dc:
                            self.tables[ti].sites_decoded.add((st.module, st.addr))
            res.append(st)
        res.sort(key=lambda s: (s.module, s.addr))
        return res


# ---------------------------------------------------------------- report

def build_report(mods: List[dsd.Module], images: Dict[str, Image], tables: List[Table], rt: Optional[Runtime]) -> Dict[str, Any]:
    funcs = {m.name: Functions(m) for m in mods}

    def fname(module: str, addr: int) -> str:
        s = funcs[module].by_addr.get(addr) if module in funcs else None
        return s.name if s else ""

    def anyname(addr: int, prefer: str) -> str:
        for name in [prefer] + [m.name for m in mods]:
            n = fname(name, addr)
            if n:
                return n
        return ""

    sites = rt.sites() if rt else []

    # functions that sit in many tables: base-class methods and the pure-virtual placeholder
    where: Dict[Tuple[str, int], Set[int]] = {}
    for ti, t in enumerate(tables):
        if t.candidate:
            for f, fm in zip(t.slots, t.slot_modules):
                where.setdefault((fm, f), set()).add(ti)
    shared = sorted(((len(v), k) for k, v in where.items() if len(v) >= 4), reverse=True)

    def related(t: Table) -> List[Dict[str, Any]]:
        score: Dict[int, int] = {}
        for k, (f, fm) in enumerate(zip(t.slots, t.slot_modules)):
            if len(where.get((fm, f), ())) > 40:        # in almost every table: says nothing
                continue
            for ti in where.get((fm, f), ()):
                o = tables[ti]
                if o is not t and k < len(o.slots) and o.slots[k] == f:
                    score[ti] = score.get(ti, 0) + 1
        best = sorted(((n, ti) for ti, n in score.items() if n >= 2), reverse=True)[:6]
        return [{"module": tables[ti].module, "addr": tables[ti].addr, "same_slots": n, "slots": len(tables[ti].slots)} for n, ti in best]

    def ref(ti: int, k: int) -> Dict[str, Any]:
        return {"module": tables[ti].module, "table": tables[ti].addr, "slot": k}

    out_tables = []
    for t in tables:
        d: Dict[str, Any] = {"module": t.module, "overlay": t.overlay, "addr": t.addr, "header": t.header, "candidate": t.candidate,
                             "slots": [{"index": k, "addr": t.addr + 4 * k, "func": f, "func_module": fm, "name": fname(fm, f)}
                                       for k, (f, fm) in enumerate(zip(t.slots, t.slot_modules))],
                             "constructors": [{"module": m, "func": f, "name": fname(m, f), "literal": lit} for m, f, lit in t.constructors]}
        if t.candidate:
            d["related"] = related(t)
        if rt:
            for k, sl in enumerate(d["slots"]):
                sl["reads"] = t.reads[k]
                sl["readers"] = sorted(t.readers[k])
            d["stores"] = [{"pc": pc, "count": n, "func": anyname_containing(funcs, mods, pc)} for pc, n in sorted(t.stores.items())]
            d["sites_by_readers"] = [{"module": m, "addr": a} for m, a in sorted(t.sites)]
            d["sites_by_decode"] = [{"module": m, "addr": a} for m, a in sorted(t.sites_decoded)]
            d["confirmed"] = bool(t.sites)
        out_tables.append(d)

    out_sites = []
    for st in sites:
        d = {"module": st.module, "addr": st.addr, "mode": "thumb" if st.thumb else "arm", "func": st.func, "func_name": fname(st.module, st.func)}
        d.update(st.decode)
        d["targets"] = [{"callee": c, "name": anyname(c, st.module), "count": n,
                         "in_slot": any(c in t.slots for t in tables),
                         "by_readers": [ref(ti, k) for ti, k in st.by_readers.get(c, ())],
                         "by_decode": [ref(ti, k) for ti, k in st.by_decode.get(c, ())]}
                        for c, n in sorted(st.targets.items())]
        out_sites.append(d)

    summary: Dict[str, Any] = {}
    for m in mods:
        mt = [t for t in tables if t.module == m.name]
        if not mt:
            continue
        s: Dict[str, Any] = {"runs": len(mt), "with_header": sum(t.header for t in mt), "candidates": sum(t.candidate for t in mt),
                             "header_and_3_slots": sum(t.header and len(t.slots) >= 3 for t in mt),
                             "with_constructor": sum(t.candidate and bool(t.constructors) for t in mt)}
        if rt:
            ms = [st for st in sites if st.module == m.name]
            shapes: Dict[str, int] = {}
            for st in ms:
                shapes[st.decode.get("shape", "unknown")] = shapes.get(st.decode.get("shape", "unknown"), 0) + 1
            in_slot = [st for st in ms if any(any(c in t.slots for t in tables) for c in st.targets)]
            s.update({"indirect_sites": len(ms), "sites_with_several_targets": sum(len(st.targets) > 1 for st in ms),
                      "most_targets_at_one_site": max((len(st.targets) for st in ms), default=0),
                      "sites_with_target_in_a_slot": len(in_slot),
                      "sites_tied_by_readers": sum(bool(st.by_readers) for st in ms),
                      "sites_tied_by_decode": sum(bool(st.by_decode) for st in ms),
                      "sites_tied_either_way": sum(bool(st.by_readers or st.by_decode) for st in ms),
                      "sites_with_every_target_tied_by_decode": sum(bool(st.targets) and all(c in st.by_decode for c in st.targets) for st in ms),
                      "site_shapes": shapes,
                      "runs_read": sum(any(t.reads) for t in mt), "runs_with_header_read": sum(t.header and any(t.reads) for t in mt),
                      "tables_confirmed_by_readers": sum(bool(t.sites) for t in mt),
                      "tables_confirmed_by_decode": sum(bool(t.sites_decoded) for t in mt if t.candidate),
                      "tables_stored": sum(bool(t.stores) for t in mt),
                      "tables_stored_with_header": sum(bool(t.stores) and t.header for t in mt),
                      "storing_code_addresses": len({pc for t in mt for pc in t.stores})})
        summary[m.name] = s
    rep: Dict[str, Any] = {"summary": summary, "tables": out_tables, "sites": out_sites,
                           "shared_functions": [{"module": fm, "func": f, "name": fname(fm, f), "tables": n} for n, (fm, f) in shared]}
    if rt:
        rep["runtime"] = {"sessions": rt.sessions, "edges": len(rt.edges), "history_entries": rt.history_entries,
                          "edges_in_shared_overlay_code_not_attributed": rt.unplaced}
    return rep


def anyname_containing(funcs: Dict[str, Functions], mods: List[dsd.Module], pc: int) -> str:
    """Name of the function containing pc. An address two overlays share gives every candidate."""
    names = []
    for m in mods:
        f = funcs[m.name].containing(pc & ~1)
        if f:
            names.append(f.name)
    return " | ".join(names)


def labels_doc(mods: List[dsd.Module], tables: List[Table]) -> Dict[str, Any]:
    """vt_<address> for each candidate table dsd has not named, in the static analysis import format."""
    named = {(m.name, s.addr) for m in mods for s in m.symbols if s.kind != "function" and not s.placeholder}
    labels = [{"addr": "0x%08x" % t.addr, "name": "vt_%08x" % t.addr, "cpu": "arm9", "overlay": t.overlay}
              for t in tables if t.candidate and (t.module, t.addr) not in named]
    return {"tool": "dswb.vtables", "labels": labels}


def print_summary(rep: Dict[str, Any]):
    for name, s in rep["summary"].items():
        print(f"{name}: {s['runs']} runs, {s['with_header']} with the header, {s['candidates']} candidates (header and 2+ slots), "
              f"{s['header_and_3_slots']} with 3+ slots, {s['with_constructor']} candidates with a constructor")
        if "indirect_sites" in s:
            print(f"  indirect call sites {s['indirect_sites']}: target in a slot {s['sites_with_target_in_a_slot']}, "
                  f"tied by readers {s['sites_tied_by_readers']}, by decode {s['sites_tied_by_decode']}, either {s['sites_tied_either_way']}")
            print(f"  shapes {s['site_shapes']}; {s['sites_with_several_targets']} sites reach several targets (most: {s['most_targets_at_one_site']})")
            print(f"  tables confirmed by readers {s['tables_confirmed_by_readers']}, by decode {s['tables_confirmed_by_decode']}; "
                  f"runs read {s['runs_read']} ({s['runs_with_header_read']} with the header); "
                  f"stored {s['tables_stored']} ({s['tables_stored_with_header']} with the header) by {s['storing_code_addresses']} code addresses")
    if "runtime" in rep:
        r = rep["runtime"]
        print(f"sessions {r['sessions']}, merged ARM9 call edges {r['edges']}, write history entries {r['history_entries']}, "
              f"edges in shared overlay code not attributed {r['edges_in_shared_overlay_code_not_attributed']}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m dswb.vtables", description=(__doc__ or "").split("\n")[0])
    ap.add_argument("--dsd", required=True, help="the dsd-config folder")
    ap.add_argument("--extract", required=True, help="dsd's extract folder (arm9/arm9.bin, arm9_overlays/ovNNN.bin)")
    ap.add_argument("--sessions", help="a folder of saved sessions (*.dswbs), or one session file")
    ap.add_argument("-o", "--out", required=True, help="the report (JSON)")
    ap.add_argument("--labels", help="also write vt_<address> labels for the candidate tables, as an import file")
    a = ap.parse_args(argv)

    mods = dsd.load(a.dsd)
    images = load_images(a.extract, mods)
    if "main" not in images:
        print("no arm9/arm9.bin under", a.extract, file=sys.stderr)
        return 1
    tables = find_tables(mods, images)
    rt = None
    if a.sessions:
        files = [a.sessions] if os.path.isfile(a.sessions) else sorted(glob.glob(os.path.join(a.sessions, "*.dswbs")))
        if not files:
            print("no *.dswbs under", a.sessions, file=sys.stderr)
            return 1
        rt = Runtime(mods, images, tables)
        for i, p in enumerate(files):
            print(f"[{i + 1}/{len(files)}] {os.path.basename(p)}", file=sys.stderr)
            rt.add_session(p)
    rep = build_report(mods, images, tables, rt)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=1)
    print_summary(rep)
    print("wrote", os.path.abspath(a.out))
    if a.labels:
        doc = labels_doc(mods, tables)
        with open(a.labels, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        print(f"wrote {len(doc['labels'])} labels to", os.path.abspath(a.labels))
    return 0


if __name__ == "__main__":
    sys.exit(main())
