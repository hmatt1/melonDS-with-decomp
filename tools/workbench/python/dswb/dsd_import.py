"""Turns a dsd configuration into the workbench's static analysis import (README: "Static analysis import").

    python -m dswb.dsd_import path/to/dsd-config -o dsd.json            write the JSON
    python -m dswb.dsd_import path/to/dsd-config -o dsd.json --apply    review the store, then import into a running workbench
    python -m dswb.dsd_import path/to/dsd-config -o dsd.json --apply --report review.json

dsd owns the ARM9 code's names, sizes and modes:
* a function dsd names gets dsd's name, size and mode, replacing a name given by hand;
* a function dsd has not named (func_02001234) gets dsd's size and mode and keeps the store's name;
* in the ranges dsd covers, the import's list is the whole truth: functions the workbench detected
  on its own, and functions, labels and references an earlier dsd import made, that dsd no longer
  lists are deleted. The import writes as the author "dsd" so the next one can tell its own rows
  apart. A function a person or an agent made or changed is kept and reported as a conflict.
With --apply the script first lists, from the running store, the names and functions of people and
agents this changes or contradicts, and the names that exist only in the workbench (to add to
symbols.txt).
"""
import argparse
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional

from . import dsd

# names the workbench or dsd make up from an address: not names to keep or report
PLACEHOLDER = re.compile(r"^(?:sub|func)_(?:ov\d+_)?[0-9a-fA-F]{8}$")

XREF_KIND = {"arm_call": "call", "thumb_call": "call", "arm_call_thumb": "call", "thumb_call_arm": "call",
             "arm_branch": "jump", "load": "data_ref"}


def convert(mods: List[dsd.Module]) -> Dict[str, Any]:
    """Returns the import document, with what was left out under "skipped" (the importer ignores that key)."""
    functions, labels, xrefs, covers = [], [], [], []
    skipped: Dict[str, List[str]] = {"ambiguous_overlay": [], "duplicate_address": [], "unknown_kind": []}
    counts = {"link_time_const": 0, "placeholder_labels": 0}
    for m in mods:
        if m.sections:
            covers.append({"cpu": "arm9", "overlay": m.overlay, "start": "0x%08x" % min(s.start for s in m.sections),
                           "end": "0x%08x" % max(s.end for s in m.sections), "module": m.name})
        seen = set()
        for s in m.symbols:
            if s.kind == "function":
                if s.mode not in ("arm", "thumb") or s.size is None:
                    skipped["unknown_kind"].append(f"{m.name}: {s.name} has no mode or size")
                    continue
                f = {"addr": "0x%08x" % s.addr, "size": s.size, "mode": s.mode, "cpu": "arm9", "overlay": m.overlay}
                if not s.placeholder:       # no name: the store keeps its own
                    f["name"] = s.name
                functions.append(f)
            elif s.kind in ("data", "bss", "label"):
                if s.placeholder:
                    counts["placeholder_labels"] += 1
                    continue
                if s.addr in seen:          # the store keeps one label per address: the first in the file
                    skipped["duplicate_address"].append(f"{m.name}: {s.name} at 0x{s.addr:08x}")
                    continue
                seen.add(s.addr)
                labels.append({"addr": "0x%08x" % s.addr, "name": s.name, "cpu": "arm9", "overlay": m.overlay})
            else:
                skipped["unknown_kind"].append(f"{m.name}: {s.name} kind {s.kind}")
        for r in m.relocs:
            if r.kind == "link_time_const":
                counts["link_time_const"] += 1
                continue
            kind = XREF_KIND.get(r.kind)
            if not kind:
                skipped["unknown_kind"].append(f"{m.name}: reference kind {r.kind} at 0x{r.frm:08x}")
                continue
            if r.to_module == "overlays" or (r.to_module == "overlay" and len(r.to_overlays) != 1):
                skipped["ambiguous_overlay"].append(f"{m.name}: 0x{r.frm:08x} -> 0x{r.to:08x} overlays{r.to_overlays}")
                continue
            xrefs.append({"from": "0x%08x" % r.frm, "to": "0x%08x" % r.to, "kind": kind, "cpu": "arm9",
                          "from_overlay": m.overlay, "to_overlay": r.to_overlays[0] if r.to_module == "overlay" else -1})
    return {"tool": "dsd", "functions": functions, "labels": labels, "xrefs": xrefs, "covers": covers,
            "skipped": skipped, "counts": counts}


def review(doc: Dict[str, Any], store: List[Dict[str, Any]]) -> Dict[str, List[Dict[str, Any]]]:
    """What importing doc does to a store's ARM9 functions (each a dict with addr, overlay, size, mode,
    name, author and author_kind, as kb.list gives them).

    replaced    a person's or agent's function whose name, size or mode the import changes
    workbench_names  names a person or agent gave to functions dsd has not named: kept, and worth
                adding to symbols.txt
    conflicts   a person's or agent's function in a covered range that dsd does not list: kept
    removed     functions in a covered range that dsd does not list and the import deletes: ones the
                workbench detected on its own, and ones an earlier dsd import made
    Rows by the import's own author (the tool, "dsd") are dsd's, not a person's or agent's.
    """
    tool = doc.get("tool", "")
    by = {(f["overlay"], int(f["addr"], 16)): f for f in doc["functions"]}
    covers = [(c["overlay"], int(c["start"], 16), int(c["end"], 16)) for c in doc.get("covers", [])]
    out: Dict[str, List[Dict[str, Any]]] = {"replaced": [], "workbench_names": [], "conflicts": [], "removed": []}
    for f in store:
        key = (f["overlay"], f["addr"])
        mine = f["author_kind"] in ("person", "agent") and f["author"] != tool
        here = {"addr": "0x%08x" % f["addr"], "overlay": f["overlay"], "name": f["name"], "size": f["size"],
                "mode": f["mode"], "author": f["author"]}
        d = by.get(key)
        if d:
            if not mine:
                continue
            new_name = d.get("name", f["name"])
            if (new_name, d["size"], d["mode"]) != (f["name"], f["size"], f["mode"]):
                out["replaced"].append(dict(here, dsd_name=d.get("name", ""), dsd_size=d["size"], dsd_mode=d["mode"]))
            if "name" not in d and f["name"] and not PLACEHOLDER.match(f["name"]):
                out["workbench_names"].append(here)
            continue
        if not any(o == f["overlay"] and s <= f["addr"] < e for o, s, e in covers):
            continue
        if mine:
            out["conflicts"].append(here)
        elif f["author_kind"] == "system" or f["author"] == tool:
            out["removed"].append(here)
    return out


def _fn(e: Dict[str, Any]) -> str:
    ov = "" if e["overlay"] < 0 else f" (overlay {e['overlay']})"
    return f"{e['addr']}{ov} {e['name'] or '(no name)'} size 0x{e['size']:x} {e['mode']}, by {e['author']}"


def review_text(rv: Dict[str, List[Dict[str, Any]]], limit: int = 10) -> str:
    lines = []
    if rv["replaced"]:
        lines.append(f"{len(rv['replaced'])} functions given by a person or agent change:")
        for e in rv["replaced"][:limit]:
            lines.append(f"    {_fn(e)}  ->  {e['dsd_name'] or '(name kept)'} size 0x{e['dsd_size']:x} {e['dsd_mode']}")
    if rv["workbench_names"]:
        lines.append(f"{len(rv['workbench_names'])} names exist only in the workbench (kept; add them to symbols.txt):")
        lines += ["    " + _fn(e) for e in rv["workbench_names"][:limit]]
    if rv["conflicts"]:
        lines.append(f"{len(rv['conflicts'])} functions of a person or agent are not in dsd's list (kept; fix them or add them to dsd):")
        lines += ["    " + _fn(e) for e in rv["conflicts"][:limit]]
    lines.append(f"{len(rv['removed'])} functions are not in dsd's list and will be removed (detected by the workbench, or left from an earlier import)"
                 + (":" if rv["removed"] else ""))
    lines += ["    " + _fn(e) for e in rv["removed"][:limit]]
    if len(rv["removed"]) > limit:
        lines.append(f"    ... and {len(rv['removed']) - limit} more (all in --report)")
    if rv["removed"]:
        lines.append("    python -m dswb.dsd_check shows what the recordings say about these addresses")
    return "\n".join(lines)


def summary(doc: Dict[str, Any]) -> str:
    named = sum(1 for f in doc["functions"] if f.get("name"))
    lines = [f"functions {len(doc['functions'])} ({named} named)  labels {len(doc['labels'])}  references {len(doc['xrefs'])}  "
             f"covered ranges {len(doc.get('covers', []))}",
             f"left out: {doc['counts']['placeholder_labels']} data labels dsd has not named, "
             f"{doc['counts']['link_time_const']} link-time constants"]
    for key, what in (("ambiguous_overlay", "references dsd could not place in one overlay"),
                      ("duplicate_address", "second labels at an address"), ("unknown_kind", "lines not understood")):
        if doc["skipped"][key]:
            lines.append(f"left out: {len(doc['skipped'][key])} {what}")
            lines += ["    " + s for s in doc["skipped"][key][:10]]
    return "\n".join(lines)


def store_functions(c) -> List[Dict[str, Any]]:
    """The running store's ARM9 functions, as review() takes them."""
    out, offset = [], 0
    while True:
        r = c.kb_list("functions", cpu="arm9", offset=offset, limit=100000)
        out += [{"addr": f.addr, "overlay": f.overlay, "size": f.size, "mode": f.mode, "name": f.name,
                 "author": f.author, "author_kind": f.author_kind} for f in r.functions]
        if not r.truncated:
            return out
        offset = r.next_offset


def apply(c, config: str, out: str, report: Optional[str] = None) -> bool:
    """Converts the dsd configuration, writes the import file, reviews the store and imports it
    through client c. Prints what it does; returns False when entries failed."""
    doc = convert(dsd.load(config))
    out = os.path.abspath(out)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    print(summary(doc))
    rv = review(doc, store_functions(c))
    print(review_text(rv))
    if report:
        with open(report, "w", encoding="utf-8") as f:
            json.dump(rv, f, indent=1)
        print("wrote", os.path.abspath(report))
    r = c.kb_import_static_analysis(out)
    print(f"imported {r.count} entries, removed {r.removed} functions and {r.removed_labels} labels, {r.failed} failed")
    if r.failed:
        print(r.error)
    return not r.failed


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m dswb.dsd_import", description=(__doc__ or "").split("\n")[0])
    ap.add_argument("config", help="the dsd-config folder (or its arm9 folder)")
    ap.add_argument("-o", "--out", required=True, help="the import JSON to write")
    ap.add_argument("--apply", action="store_true", help="import it into the running workbench (needs --api)")
    ap.add_argument("--report", help="with --apply: write the full review of the store (JSON)")
    ap.add_argument("--agent", default="dsd-import", help="author name for the knowledge-store changes")
    ap.add_argument("--info-file", default=None, help="api.json of the running melonDS, when not in the default place")
    a = ap.parse_args(argv)

    if not a.apply:
        doc = convert(dsd.load(a.config))
        out = os.path.abspath(a.out)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        print(summary(doc))
        print("wrote", out)
        return 0

    from .client import Client
    with Client.connect(a.info_file, timeout=600.0, agent=a.agent) as c:
        return 0 if apply(c, a.config, a.out, a.report) else 1


if __name__ == "__main__":
    sys.exit(main())
