"""Tests for dswb.dsd_check that need no emulator: a small dsd configuration and session files built
here, with one call target of each kind the check sorts.

  python tools/workbench/tests/test_dsd_check.py
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY = os.path.abspath(os.path.join(HERE, "..", "python"))
sys.path.insert(0, PY)

from dswb import dsd, dsd_check  # noqa: E402
from dswb._generated import CallEdge, ByteRecord, PageHeader, Region, SessionHeader, SessionSection, SessionSectionHeader, SCHEMA_VERSION  # noqa: E402

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + ("" if cond or not detail else ": " + str(detail)))


SYMBOLS = """\
Main kind:function(arm,size=0x100) addr:0x02000000
Loop kind:function(thumb,size=0x100) addr:0x02000100
Piece kind:function(thumb,size=0x40) addr:0x02000200
Merged kind:function(thumb,size=0x100) addr:0x02000300
Leaf kind:function(arm,size=0x10) addr:0x02000500
"""
DELINKS = """\
    .text       start:0x02000000 end:0x02000600 kind:code align:32
    .data       start:0x02000600 end:0x02000700 kind:data align:32
"""
OV_SYMBOLS = "OvA kind:function(arm,size=0x20) addr:0x02100000\n"
OV_DELINKS = "    .text       start:0x02100000 end:0x02100100 kind:code align:32\n"


def edge(site, callee, kind=0, returns=0, count=1, site_ov=0, callee_ov=0):
    return struct.pack(CallEdge.FORMAT, 0, kind, 0, returns, site, callee, 0, count, 0, 0, site_ov, callee_ov)


def write_session(path, edges, executed=()):
    """A session file with these call edges and, in main RAM, these executed addresses."""
    sections = [(SessionSection.CallEdges, len(edges), b"".join(edges))]
    if executed:
        page = bytearray(PageHeader.SIZE + ByteRecord.SIZE * 4096)
        struct.pack_into(PageHeader.FORMAT, page, 0, int(Region.MainRAM), 0, 0)
        for a in executed:
            struct.pack_into("<I", page, PageHeader.SIZE + (a - 0x02000000) * ByteRecord.SIZE, 5)
        sections.append((SessionSection.ByteRecords, 1, bytes(page)))
    hdr = struct.pack(SessionHeader.FORMAT, b"DSWBSESS", SCHEMA_VERSION, SessionHeader.SIZE, b"\0" * 20, len(sections),
                      0, 100, 0, 0, 0, b"\0" * 20, b"\0" * 8)
    body = b"".join(struct.pack(SessionSectionHeader.FORMAT, int(sid), n, len(data)) + data for sid, n, data in sections)
    with open(path, "wb") as f:
        f.write(hdr + body)


def main():
    tmp = tempfile.mkdtemp(prefix="wb_check_")
    try:
        cfg = os.path.join(tmp, "dsd-config", "arm9")
        os.makedirs(os.path.join(cfg, "overlays", "ov002"))
        for rel, text in (("symbols.txt", SYMBOLS), ("relocs.txt", ""), ("delinks.txt", DELINKS),
                          ("overlays/ov002/symbols.txt", OV_SYMBOLS), ("overlays/ov002/relocs.txt", ""), ("overlays/ov002/delinks.txt", OV_DELINKS)):
            with open(os.path.join(cfg, rel), "w", encoding="utf-8") as f:
                f.write(text)
        sessions = os.path.join(tmp, "sessions")
        os.makedirs(sessions)
        write_session(os.path.join(sessions, "a.dswbs"), [
            edge(0x02000010, 0x02000500, returns=1, count=9),          # an ordinary call to a function start
            edge(0x02000180, 0x0200011a, returns=2, count=7),          # bl as a jump inside Loop: fine
            edge(0x02000010, 0x02000040, returns=1, count=3),          # a call into the middle of Main that came back
            edge(0x02000210, 0x02000330, returns=2, count=4),          # a bl jump from Piece into the middle of Merged
            edge(0x02000150, 0x02000160, kind=3, count=2),             # a "tail call" inside Loop: fine
            edge(0x02000220, 0x02000340, kind=3, count=2),             # a "tail call" from Piece into Merged
            edge(0x02000010, 0x02000420, returns=1),                   # a target in no dsd function
            edge(0x02000010, 0x02100000, returns=1, callee_ov=3),      # overlay 2's function start
            edge(0x02000010, 0xFFFF0108, kind=0),                      # into the BIOS (an old recording's interrupt): ignored
        ], executed=[0x02000000, 0x02000400, 0x02000402, 0x02000404, 0x02000600])
        write_session(os.path.join(sessions, "b.dswbs"), [
            edge(0x02000010, 0x02000500, returns=1, count=1),
            edge(0x02000180, 0x0200011a, returns=0, count=1),          # an older recording: adds nothing to "fine"
            edge(0x02000228, 0x02000308, returns=0, count=5),          # an older recording: from Piece into Merged, not known
        ])
        files = sorted(os.path.join(sessions, n) for n in os.listdir(sessions))
        r = dsd_check.check(dsd.load(os.path.join(tmp, "dsd-config")), files)
        where = lambda k: [e["target"] for e in r[k]]
        check("targets at a function start are only counted (main and an overlay)", r["targets_at_a_start"] == 2, r["targets_at_a_start"])
        check("a call into the middle that came back is reported", where("came_back") == ["0x02000040"], r["came_back"])
        check("a bl jump from one function into another is reported", where("jumped_between") == ["0x02000330"], r["jumped_between"])
        check("a tail call from one function into another is reported", where("tail_between") == ["0x02000340"], r["tail_between"])
        check("jumps and branches inside one function are only counted", r["within_one_function"] == 2, r["within_one_function"])
        check("an older recording's call into the middle is 'unknown'", where("unknown") == ["0x02000308"]
              and r["unknown"][0]["returned"] == "unknown", r["unknown"])
        check("a target in no function", where("called_outside") == ["0x02000420"], r["called_outside"])
        check("returned marks add up across sessions", r["jumped_between"][0]["returned"] == "no"
              and r["came_back"][0]["sites"][0]["in"] == "02000010 (Main+0x10)", r["jumped_between"])
        ran = [(x["module"], x["start"], x["end"]) for x in r["ran_outside"]]
        check("code that ran outside every function, not data", ran == [("main", "0x02000400", "0x02000406")], ran)
        text = dsd_check.summary(r)
        check("the summary names each finding", "came back" in text and "probably one function" in text and "Merged+0x30" in text, text)

        p = subprocess.run([sys.executable, "-m", "dswb.dsd_check", "--dsd", os.path.join(tmp, "dsd-config"), "--sessions", sessions,
                            "-o", os.path.join(tmp, "r.json")], capture_output=True, text=True, env=dict(os.environ, PYTHONPATH=PY))
        check("the command runs and writes the report", p.returncode == 0 and os.path.exists(os.path.join(tmp, "r.json")), p.stdout + p.stderr)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed" % (len(passed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
