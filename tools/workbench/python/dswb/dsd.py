"""Reader for a dsd (ds-decomp) configuration folder: symbols.txt, relocs.txt and delinks.txt.

    mods = dsd.load("path/to/dsd-config")        # or its arm9 folder
    for m in mods: print(m.name, m.overlay, len(m.symbols), len(m.relocs))

One Module per folder: the static module ("main"), the autoloads ("itcm", "dtcm") and each overlay
("ov003"). Only the ARM9 side is read; dsd has no ARM7 configuration.
"""
import os
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

_SYM = re.compile(r"^(\S+)\s+kind:(\w+)(?:\(([^)]*)\))?\s+addr:0x([0-9a-fA-F]+)")
_REL = re.compile(r"^from:0x([0-9a-fA-F]+)\s+kind:(\w+)\s+to:0x([0-9a-fA-F]+)\s+module:(\w+)(?:\(([^)]*)\))?")
_SEC = re.compile(r"^\s+(\.\w+)\s+start:0x([0-9a-fA-F]+)\s+end:0x([0-9a-fA-F]+)(?:\s+kind:(\w+))?")
# dsd's default names: func_/data_/bss_, an optional ov003_, autoload_1_, itcm_ or dtcm_, the address,
# an optional _unk; and its local labels (.L_...) and gap names (_dsd...)
_PLACEHOLDER = re.compile(r"^(func|data|bss)_(?:ov\d+_|autoload_\d+_|itcm_|dtcm_)?([0-9a-fA-F]{8})(?:_unk)?$")
_LOCAL = re.compile(r"^\.L_|^_dsd")


@dataclass
class Symbol:
    name: str
    kind: str                       # function, data, bss, label
    addr: int
    mode: Optional[str] = None      # arm or thumb (functions and labels)
    size: Optional[int] = None      # functions
    attrs: str = ""                 # the text in the kind's brackets

    @property
    def placeholder(self) -> bool:
        """True for a name dsd made up from the address (func_02001234, data_ov003_02121c68)."""
        if _LOCAL.match(self.name):
            return True
        m = _PLACEHOLDER.match(self.name)
        return bool(m) and int(m.group(2), 16) == self.addr


@dataclass
class Reloc:
    frm: int
    kind: str                       # arm_call, thumb_call, arm_call_thumb, thumb_call_arm, arm_branch, load, link_time_const
    to: int
    to_module: str                  # main, itcm, dtcm, overlay, overlays
    to_overlays: Tuple[int, ...] = ()   # one id for "overlay", several for "overlays" (dsd could not decide)


@dataclass
class Section:
    name: str
    start: int
    end: int
    kind: str                       # code, rodata, data, bss


@dataclass
class Module:
    name: str                       # main, itcm, dtcm, ovNNN
    overlay: int                    # overlay id, -1 for the static module and the autoloads
    path: str
    symbols: List[Symbol] = field(default_factory=list)
    relocs: List[Reloc] = field(default_factory=list)
    sections: List[Section] = field(default_factory=list)

    def functions(self) -> List[Symbol]:
        return [s for s in self.symbols if s.kind == "function"]

    def section(self, name: str) -> Optional[Section]:
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def contains(self, addr: int) -> bool:
        return any(s.start <= addr < s.end for s in self.sections)


def _symbols(path: str) -> List[Symbol]:
    out = []
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = _SYM.match(line)
            if not m:
                continue
            name, kind, attrs, addr = m.group(1), m.group(2), m.group(3) or "", int(m.group(4), 16)
            s = Symbol(name, kind, addr, attrs=attrs)
            parts = [p.strip() for p in attrs.split(",") if p.strip()]
            if kind in ("function", "label"):
                for p in parts:
                    if p in ("arm", "thumb"):
                        s.mode = p
                    elif p.startswith("size="):
                        s.size = int(p[5:], 0)
            out.append(s)
    return out


def _relocs(path: str) -> List[Reloc]:
    out = []
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = _REL.match(line)
            if not m:
                continue
            ids = tuple(int(x) for x in m.group(5).split(",")) if m.group(5) else ()
            out.append(Reloc(int(m.group(1), 16), m.group(2), int(m.group(3), 16), m.group(4), ids))
    return out


def _sections(path: str) -> List[Section]:
    """The module's own section list: the indented lines before the first object file."""
    out = []
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if line.strip().endswith(":"):
                break
            m = _SEC.match(line)
            if m:
                out.append(Section(m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4) or ""))
    return out


def _module(name: str, overlay: int, folder: str) -> Module:
    return Module(name, overlay, folder, _symbols(os.path.join(folder, "symbols.txt")),
                  _relocs(os.path.join(folder, "relocs.txt")), _sections(os.path.join(folder, "delinks.txt")))


def arm9_dir(config_dir: str) -> str:
    """Accepts the dsd-config folder or its arm9 folder."""
    sub = os.path.join(config_dir, "arm9")
    if os.path.exists(os.path.join(sub, "symbols.txt")):
        return sub
    if os.path.exists(os.path.join(config_dir, "symbols.txt")):
        return config_dir
    raise FileNotFoundError(f"no arm9/symbols.txt under {config_dir}")


def load(config_dir: str) -> List[Module]:
    root = arm9_dir(config_dir)
    mods = [_module("main", -1, root)]
    for auto in ("itcm", "dtcm"):
        d = os.path.join(root, auto)
        if os.path.isdir(d):
            mods.append(_module(auto, -1, d))
    ovroot = os.path.join(root, "overlays")
    if os.path.isdir(ovroot):
        for name in sorted(os.listdir(ovroot)):
            m = re.match(r"^ov(\d+)$", name)
            if m and os.path.isdir(os.path.join(ovroot, name)):
                mods.append(_module(name, int(m.group(1)), os.path.join(ovroot, name)))
    return mods


def by_name(mods: List[Module]) -> Dict[str, Module]:
    return {m.name: m for m in mods}
