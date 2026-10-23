"""Phase 2 and phase 4 exit check, driven entirely through the API the way an agent would.

    python tools/workbench/tests/agent_scenario.py --rom "TrackMania DS (USA) (En,Fr,Es).nds"

1. starts melonDS with only the API enabled and boots the game with game.boot
2. turns recording on and plays from a fresh save to a StadiumA1 race with button input
3. checks the race runs at full speed while recording and the recording indicator reports it
4. pauses, finds the race timer by diffing RAM between two pauses, and answers
   "which code writes this address" with a write breakpoint, write history and call data

The ROM is copied to a temporary folder so no existing save file is used.
"""
import argparse
import base64
import io
import os
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
from dswb import Client
from dswb._generated import AddrRange, OriginSourceSpec  # noqa: E402

FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def screen_brightness(png_b64):
    """Mean luminance of a PNG (8-bit RGB/RGBA as written by Qt), decoded with the standard library."""
    import zlib
    data = base64.b64decode(png_b64)
    pos, idat, w, h, ctype = 8, b"", 0, 0, 2
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    total = 0
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            ul = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + left) & 0xFF
            elif f == 2: line[i] = (line[i] + up) & 0xFF
            elif f == 3: line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif f == 4:
                pp = left + up - ul
                pa, pb, pc = abs(pp - left), abs(pp - up), abs(pp - ul)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else ul)) & 0xFF
        for x in range(w):
            total += (line[x * bpp] + line[x * bpp + 1] + line[x * bpp + 2]) / 3
        prev = line
    return total / (w * h)


def read_ram(c, base, size):
    out = b""
    for off in range(0, size, 0x100000):
        out += bytes.fromhex(c.memory_read(base + off, size=min(0x100000, size - off)).hex)
    return out


# menu path from a fresh save to a StadiumA1 practice race (inputs and frame waits)
MENU = [("wait", 750), ("a", 5), ("wait", 60), ("a", 5), ("wait", 90),       # logos, select profile -> enter name
        ("start", 5), ("wait", 90), ("a", 5), ("wait", 90), ("a", 5), ("wait", 90), ("a", 5), ("wait", 90),  # name, save, play
        ("a", 5), ("wait", 60), ("a", 5), ("wait", 60), ("a", 5), ("wait", 60),  # solo, race, practice
        ("a", 5), ("wait", 120), ("a", 5), ("wait", 200),                         # stadium, StadiumA1
        ("wait", 300), ("a", 5), ("wait", 200)]                                   # opponent: all -> GO


VEHICLES = "Scripts/config/vehicles.luac"
DOCUMENTED_LOADER = 0x020a87f4   # load_vehicle_physics_parameters per tm-nds-documentation (020a8800 is inside it)


def find_loader(c, source):
    """The code that turned a file into a struct, found from data origin alone: outside copies of
    archived files, the densest block of the file's labelled data words (not pointers) that was written
    once while loading rather than every frame. Returns (function, block address, words, writer PCs)."""
    now = c.status_get().frame
    rr = c.origin_ranges(source=source, limit=50000)
    labelled = set()
    for q in rr.ranges:
        labelled.update(range(q.start & ~3, q.end, 4))
    # nearby ranges in one query each
    spans = []
    for w in sorted(labelled):
        if spans and w - spans[-1][1] < 64:
            spans[-1][1] = w + 4
        else:
            spans.append([w, w + 4])
    func_of, words = {}, {}
    for lo, hi in spans:
        md = c.metadata_get(lo, size=hi - lo, group="block", block_size=4)
        raw = bytes.fromhex(c.memory_read(lo, size=hi - lo).hex)
        for e in md.entries:
            if e.addr not in labelled or not e.last_writer_pc or e.last_write_frame + 120 >= now:
                continue
            if (struct.unpack_from("<I", raw, e.addr - lo)[0] >> 24) == 0x02:
                continue                     # a pointer: computed from the data, not the data
            if e.last_writer_pc not in func_of:
                func_of[e.last_writer_pc] = c.calls_get(e.last_writer_pc).function
            words[e.addr] = func_of[e.last_writer_pc]

    def densest(addrs):
        addrs.sort()
        best, at, j = 0, 0, 0
        for i in range(len(addrs)):
            while addrs[i] - addrs[j] >= 512:
                j += 1
            if i - j + 1 > best:
                best, at = i - j + 1, addrs[j]
        return best, at

    cands = []
    for f in set(words.values()):
        n, at = densest([w for w, g in words.items() if g == f])
        cands.append((n, f, at))
    ranked = []
    for n, f, at in sorted(cands, reverse=True)[:10]:
        # bytes equal to an archived file are a copy of it, not a struct made from it
        copy = [(x.addr, x.addr + x.size) for x in c.origin_get(at, size=512).archive]
        n = sum(1 for w, g in words.items() if g == f and at <= w < at + 512 and not any(s <= w < e for s, e in copy))
        ranked.append((n, f, at))
    ranked.sort(reverse=True)
    if not ranked:
        return 0, 0, 0, [], []
    n, f, at = ranked[0]
    pcs = sorted(pc for pc, g in func_of.items() if g == f)
    return f, at, n, pcs, ranked[:4]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--port", type=int, default=47960)
    ap.add_argument("--shots", help="save both screens after every menu step into this folder")
    ap.add_argument("--menu-only", action="store_true")
    ap.add_argument("--keep", action="store_true", help="leave melonDS running at the end")
    ap.add_argument("--assets", action="store_true",
                    help="phase 7 check: record data origin (following table lookups) from boot and trace the race's 3D data to its ROM file")
    ap.add_argument("--struct", action="store_true",
                    help="phase 8 check: track Scripts/config/vehicles.luac with data origin from boot and trace "
                         "the car physics parameters back to it")
    ap.add_argument("--follow", action="store_true", help="with --struct: also follow table lookups")
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    tmp = tempfile.mkdtemp(prefix="dswb_agent_")
    rom = os.path.join(tmp, os.path.basename(a.rom))
    shutil.copyfile(a.rom, rom)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    kb = os.path.join(tmp, "knowledge.db")
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--kb", kb], cwd=exedir, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        c = Client.connect(info, timeout=120)
        c.subscribe()
        check(c.status_get().state == "no_game", "melonDS started with no game")

        print("boot and play")
        c.game_boot(path=rom)
        st = c.status_get()
        check(st.rom_gamecode == "CTRE", "game.boot booted %s (%s)" % (st.rom_title, st.rom_gamecode))
        trace = a.keep or a.assets
        if a.struct:
            # narrow data origin to the vehicle configuration so its label can't drown in unions. Its unpacked
            # bytes are labelled by content, so table lookups (zlib's Huffman decoding) need not be followed
            for _ in range(100):
                if c.archive_list(limit=1).status != "indexing":
                    break
                time.sleep(0.05)
            # write history for main RAM only (the game's objects live there), with the default filters
            r = c.recording_configure(enabled=True, write_history=True, history_file_mb=512, history_include=[AddrRange(start=0x02000000, end=0x02400000)],
                                      data_origin=True, origin_follow_addresses=a.follow, origin_all_rom_files=False,
                                      sources_add=[OriginSourceSpec(kind="archive_file", file=VEHICLES)])
        else:
            r = c.recording_configure(enabled=True, data_origin=True if trace else None, origin_follow_addresses=True if trace else None)
        check(r.recording.enabled, "recording switched on through the API")
        if a.shots:
            os.makedirs(a.shots, exist_ok=True)
        for i, (kind, n) in enumerate(MENU):
            if kind == "wait":
                c.execution_control("run_frames", count=n)
            else:
                c.input_send(buttons=[kind], frames=n, capture=False)
            if a.shots:
                sc = c.screen_capture()
                for nm, d in (("top", sc.top_png), ("bottom", sc.bottom_png)):
                    open(os.path.join(a.shots, "step%02d_%s.png" % (i, nm)), "wb").write(base64.b64decode(d))
        if a.menu_only:
            if a.keep:
                os._exit(0)
            return 0
        r = c.input_send(buttons=["a"], frames=150)
        top = screen_brightness(r.top_png)
        check(top < 200, "reached the race (top screen is the 3D track, brightness %.0f)" % top)

        if a.struct:
            print("a struct field -> its archived file, RAM location and loader")
            c.execution_control("pause")
            t0 = time.time()
            loader, block, n, pcs, ranked = find_loader(c, VEHICLES)
            print("    candidates (labelled data words in 512 bytes, written once): %s"
                  % ", ".join("%08x: %d at %08x" % (f, k, at) for k, f, at in ranked))
            calls = c.calls_get(loader)
            check(loader and n >= 40 and calls.callers,
                  "found the vehicle parameter loader from data origin alone in %.0f s: %08x, %d words of a struct at %08x, called from %s"
                  % (time.time() - t0, loader, n, block, [hex(e.call_site) for e in calls.callers[:4]]))
            doc = c.calls_get(DOCUMENTED_LOADER)
            print("    the documented load_vehicle_physics_parameters %08x calls %s" % (DOCUMENTED_LOADER, [hex(e.callee) for e in doc.callees[:4]]))
            # what the loader wrote last: per-byte last writers, no write history needed
            loader_from, end = loader, (pcs[-1] + 4 if pcs else loader + 4)
            cov = c.coverage_get(region="main_ram", kind="write", writer_from=loader_from, writer_to=end, limit=200)
            words = [w for q in cov.ranges for w in range(q.start & ~3, q.end, 4)]
            # write history, on since boot, still holds the loader's writes: bulk unpacking is left out
            t0 = time.time()
            hist = c.writes_list(0, size=0, writer_from=loader_from, writer_to=end, limit=5000)
            t1 = time.time()
            page = c.writes_list(0, size=0, limit=100000)
            t2 = time.time()
            print("    queries over all of history: the loader's writes in %.1f s; a 100000-write page of %d in %.1f s"
                  % (t1 - t0, page.total, t2 - t1))
            check(t1 - t0 < 10 and t2 - t1 < 20 and len(page.entries) == 100000 and page.truncated,
                  "full-history queries return well within the client timeout")
            rs = c.recording_get().recording
            print("    write history: %d entries kept, %d bulk writes skipped; %d by the loader"
                  % (rs.history_entries, rs.history_skipped, len(hist.entries)))
            # every word the loader wrote is in history (one store-multiple writes several words), except
            # zero defaults written over zeroed memory: history leaves out writes that change nothing
            kept = set()
            for e in hist.entries:
                kept.update(range(e.addr & ~3, e.addr + e.size, 4))
            unchanged = [w for w in words if w not in kept]
            check(rs.history_skip_unchanged and all(c.memory_read(w, size=4).hex == "00000000" for w in unchanged)
                  and rs.history_entries < rs.history_cap,
                  "write history from boot still has the loader's writes: %d of %d words, the rest zeros written over zeros (not full: %d of %d)"
                  % (len(words) - len(unchanged), len(words), rs.history_entries, rs.history_cap))
            biggest = max(cov.ranges, key=lambda q: q.end - q.start) if cov.ranges else None
            print("    the loader wrote %d bytes in %d runs; largest %08x..%08x" % (cov.total_bytes, len(cov.ranges),
                  biggest.start if biggest else 0, biggest.end if biggest else 0))
            tracked, other = [], []
            for w in words:
                o = c.origin_get(w, size=4)
                names = {src for t in o.taint for src in t.sources}
                (tracked if any(n.endswith(VEHICLES) for n in names) else other).append(w)
            check(len(tracked) >= 0.85 * len(words), "%d of %d words the loader wrote carry the %s label" % (len(tracked), len(words), VEHICLES))
            # the rest must be explainable: flags, counts and defaults, code pointers, or words the loader only
            # partly wrote (a byte store at the end of a run)
            unexplained = []
            for w in other:
                val = struct.unpack("<I", bytes.fromhex(c.memory_read(w, size=4).hex))[0]
                partial = not any(q.start <= w and w + 4 <= q.end for q in cov.ranges)
                code = (val >> 24) == 0x02 and c.metadata_get(val & ~1, size=2, group="byte").entries[0].exec > 0
                if not (partial or val <= 16 or code):
                    unexplained.append("%08x=%08x" % (w, val))
            check(not unexplained, "every unlabelled word is a constant, count, code pointer or partial write: %s"
                  % (unexplained or "%d words" % len(other)))
            o = c.origin_get(tracked[0], size=4) if tracked else None
            reads = [f for f in (o.file_reads if o else []) if f.archive_member.startswith(VEHICLES)]
            for f in reads[:2]:
                print("    %s read from the card by %08x (function %08x), frame %d: %s"
                      % (VEHICLES, f.pc, c.calls_get(f.pc).function, f.frame, f.archive_member))
            check(reads and reads[0].pc, "the labelled fields name the card reads of %s and their loader" % VEHICLES)
            loc = c.archive_locate(path=VEHICLES)
            check(loc.ram and loc.loads, "%s is unpacked at %s" % (VEHICLES, [hex(x.addr) for x in loc.ram[:2]]))
            # control: bytes equal to other archived files (the race's 3D data) cannot derive from the vehicle
            # configuration, so the label there is a false positive
            dma = c.coverage_get(region="main_ram", kind="dma", limit=5000)
            sample = []
            for big in sorted(dma.ranges, key=lambda q: -(q.end - q.start))[:3]:
                o = c.origin_get(big.start, size=big.end - big.start)
                for x in [x for x in o.archive if not x.path.endswith(VEHICLES)][:30]:
                    sample += list(range((x.addr + 3) & ~3, x.addr + x.size - 4, max(4, (x.size // 8) & ~3)))
            leaked = [w for w in sample if any(src.endswith(VEHICLES) for t in c.origin_get(w, size=4).taint for src in t.sources)]
            hit = len(tracked) / max(len(words), 1)
            noise = len(leaked) / max(len(sample), 1)
            print("    labelled: %.0f%% of the struct words, %.0f%% of %d words that equal other archived files"
                  % (100 * hit, 100 * noise, len(sample)))
            seeded = c.status_get().recording.origin_seeded_files
            print("    archived file chunks labelled by content: %d" % seeded)
            check(sample and hit >= 0.7 and noise <= 0.02 and seeded > 0,
                  "the label singles out the vehicle parameters (%.0f%% against %.0f%% false positives)" % (100 * hit, 100 * noise))
            if a.keep:
                os._exit(1 if FAILS else 0)
            c.close()
            return 1 if FAILS else 0

        if a.assets:
            print("where does the race's 3D data come from?")
            c.execution_control("pause")
            lst = c.archive_list(filter=".nsbmd", limit=1)
            check(lst.status == "ready" and lst.member_count > 3000,
                  "the game archive was detected and indexed: %s" % lst.summary)
            cov = c.coverage_get(region="main_ram", kind="dma", limit=5000)
            ranges = sorted(cov.ranges, key=lambda r: -(r.end - r.start))[:3]
            check(len(ranges) > 0, "found main RAM ranges the game DMAs to the 3D engine and VRAM")
            traced = 0
            models = []
            all_segs = []
            for rg in ranges:
                found, at = {}, rg.start
                while at < rg.end:     # page through long ranges
                    o = c.origin_get(at, size=rg.end - at)
                    for x in o.archive:
                        found[x.addr] = x
                    if not o.archive_truncated:
                        break
                    at = max(x.addr + x.size for x in o.archive)
                segs = sorted(found.values(), key=lambda x: -x.size)
                all_segs += segs
                spans = sorted((max(x.addr, rg.start), min(x.addr + x.size, rg.end)) for x in segs)
                covered, edge = 0, rg.start
                for s0, s1 in spans:
                    covered += max(0, s1 - max(s0, edge))
                    edge = max(edge, s1)
                print("    %08x..%08x: %d archived files, %d%% of the bytes" % (rg.start, rg.end, len({x.path for x in segs}),
                                                                         100 * covered // max(rg.end - rg.start, 1)))
                for x in segs[:3]:
                    print("      %08x+%-6x = %s +0x%x" % (x.addr, x.size, x.path, x.member_offset))
                    if x.loads:
                        l = x.loads[0]
                        print("        read from the card by %08x (function %08x), frame %d: %s"
                              % (l.pc, c.calls_get(l.pc).function, l.frame, l.archive_member))
                    if x.writer_pc:
                        print("        unpacked into RAM by %08x (function %08x)" % (x.writer_pc, c.calls_get(x.writer_pc).function))
                    if x.path.lower().endswith(".nsbmd") and x.loads and x.writer_pc:
                        models.append(x)
                traced += 1 if any(x.loads and x.writer_pc for x in segs) else 0
            unpackers = {c.calls_get(x.writer_pc).function for x in all_segs if x.writer_pc}
            check(all(f and c.calls_get(f).callers for f in unpackers) or not unpackers,
                  "the unpacking code resolves to functions with observed callers: %s" % sorted(hex(f) for f in unpackers)[:6])
            check(traced >= 2, "%d of the 3 largest DMA sources resolve to archived files with their loader and unpacker" % traced)

            print("a 3D model file -> its ROM file, RAM location and loader function")
            if models:
                model = max(models, key=lambda x: x.size).path
                r = c.archive_locate(path=model)
                m = r.member
                print("    %s: %d bytes, %s %d bytes in %s at ROM 0x%x (vfs.bin entry %d)"
                      % (m.path, m.size, m.method, m.packed_size, lst.archive_file, m.rom_offset, m.vfs_entry))
                for x in r.ram[:3]:
                    print("    in RAM at %08x+%x (bytes +0x%x), written by %08x" % (x.addr, x.size, x.member_offset, x.writer_pc))
                loaders = sorted({c.calls_get(l.pc).function for l in r.loads})
                for l in r.loads[:2]:
                    print("    read from the card by %08x, frame %d, to %08x: %s" % (l.pc, l.frame, l.dst_addr, l.archive_member))
                check(r.ram and r.loads and m.rom_offset,
                      "%s located: ROM 0x%x, RAM %08x, loader function(s) %s"
                      % (m.path, m.rom_offset, r.ram[0].addr if r.ram else 0, ", ".join("%08x" % f for f in loaders)))
                c.recording_configure(enabled=False)
                rows = []
                for _ in range(50):
                    try:
                        db = sqlite3.connect(kb, timeout=5)
                        rows = db.execute("SELECT l.kind, count(*) FROM asset a JOIN asset_link l ON l.asset_id = a.id "
                                          "WHERE a.member = ? GROUP BY l.kind", (model,)).fetchall()
                        db.close()
                    except sqlite3.Error:
                        rows = []
                    if len(rows) >= 2:
                        break
                    time.sleep(0.2)
                check(dict(rows).get("archive_read") and dict(rows).get("content"),
                      "the session import linked %s to its card reads and RAM copies: %s" % (model, rows))
            else:
                check(False, "no 3D model file found among the DMA sources")
            if a.keep:
                os._exit(1 if FAILS else 0)
            c.close()
            return 1 if FAILS else 0

        print("full speed while recording")
        c.execution_control("resume")
        import threading
        def hold():
            try:
                c.input_send(buttons=["a"], frames=600, capture=False)
            except Exception:
                pass   # melonDS may be closed before the 600 frames end
        threading.Thread(target=hold, daemon=True).start()
        time.sleep(1.0)
        f0 = c.status_get().frame
        t0 = time.time()
        time.sleep(5.0)
        st = c.status_get()
        fps = (st.frame - f0) / (time.time() - t0)
        check(st.recording.enabled and st.state == "running", "status reports recording on while racing")
        check(fps >= 58.0, "race runs at full speed with recording on (%.1f fps)" % fps)
        check(st.recording.frames_recorded > 2000, "frames recorded so far: %d" % st.recording.frames_recorded)

        print("which code writes the race timer?")
        c.execution_control("pause")
        time.sleep(0.3)
        regions = [(0x02000000, 0x400000), (0x027E0000, 0x4000)]
        snap0 = [read_ram(c, b, n) for b, n in regions]
        f0 = c.status_get().frame
        c.execution_control("run_frames", count=60)
        snap1 = [read_ram(c, b, n) for b, n in regions]
        f1 = c.status_get().frame
        frames = f1 - f0
        # the race clock: milliseconds, centiseconds or frames since the start
        units = [("ms", frames * 1000.0 / 59.83, 3), ("cs", frames * 100.0 / 59.83, 2), ("frames", frames, 0)]
        cands = []
        for unit, want, tol in units:
            for (base, n), r0, r1 in zip(regions, snap0, snap1):
                for off in range(0, n, 4):
                    v0, v1 = struct.unpack_from("<I", r0, off)[0], struct.unpack_from("<I", r1, off)[0]
                    if v1 > v0 and abs((v1 - v0) - want) <= tol and v1 < want * 100:
                        cands.append((base + off, unit, v1))
            if cands:
                break
        print("    candidates:", [(hex(x), u, v) for x, u, v in cands[:8]])
        cands = [x for x, _, _ in cands]
        check(len(cands) > 0, "found the race clock in RAM (%d candidates)" % len(cands))
        if cands:
            addr = cands[0]
            c.recording_configure(write_history=True)
            c.breakpoint_set("write", addr, size=4)
            c.execution_control("resume")
            ev = c.wait_event("breakpoint_hit", 10)
            pc = ev["hit"]["pc"]
            check(ev["hit"]["access_addr"] == addr, "write breakpoint on %08x hit, written by code at %08x" % (addr, pc))
            c.breakpoint_clear(-1)
            c.execution_control("run_frames", count=10)
            w = c.writes_list(addr, size=4, limit=20)
            writers = sorted(set(e.pc for e in w.entries))
            check(pc in writers, "write history agrees: writers %s" % [hex(x) for x in writers])
            d = c.disasm_get(pc, count=1).lines[0]
            stack = c.callstack_get()
            print("    %08x  %s" % (d.addr, d.text))
            print("    called through:", " <- ".join("%08x" % f.callee for f in reversed(stack.frames[-6:])))
            meta = c.metadata_get(pc, size=4, group="instruction").entries[0]
            check(meta.exec > 0 and meta.mode in ("arm", "thumb"), "the writer is recorded %s code executed %d times" % (meta.mode, meta.exec))
            calls = c.calls_get(pc)
            check(len(calls.callers) > 0, "the writer's function %08x has observed callers %s" % (calls.function, [hex(e.call_site) for e in calls.callers[:3]]))

        print("overlays after loading a savestate")
        # a savestate brings overlay code that was never read from the card in this run: it must still be
        # attributed to its overlay ("written, then executed" detection)
        state = os.path.join(tmp, "race.mln")
        c.execution_control("pause")
        c.state_save(path=state)
        c.game_boot(path=rom)                      # fresh ownership: nothing is known to be loaded
        c.recording_configure(enabled=True)
        c.state_load(path=state)
        c.execution_control("run_frames", count=30)
        span_lo, span_hi = 0x020ffe00, 0x0212e240           # TrackMania's overlays 2 and 3
        cov = c.coverage_get(addr=span_lo, size=span_hi - span_lo, kind="exec", limit=40)
        regions = sorted({c.metadata_get(r.start, size=4, group="byte").entries[0].region.split("+")[0] for r in cov.ranges})
        check(cov.ranges and all(r.startswith("ov9_") for r in regions),
              "overlay code run after the savestate is recorded under its overlay: %s" % regions)
        c.close()
    finally:
        proc.terminate()
        try:
            proc.wait(15)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d failed" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
