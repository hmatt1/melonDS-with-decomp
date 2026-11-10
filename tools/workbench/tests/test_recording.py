"""End-to-end test of the recording engine, debugger and API using the test ROM.

    python tools/workbench/tests/test_recording.py --melonds build/melonDS.exe --rom tools/workbench/testrom/wbtest.nds

Boots the test ROM with the API and recording on, finds the manifest the ROM
publishes in RAM, and checks that the recording matches the ROM's known layout:
ARM and Thumb code, literal pools, data, DMA, write history, call edges,
card-read and decompression provenance, data origin labels, breakpoints,
stepping and session files.
"""
import argparse
import json
import os
import re
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client, ApiError, session as sessionmod
from dswb._generated import AddrRange  # noqa: E402
from ui_point import check_ui  # noqa: E402
import compare_approval  # noqa: E402

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


def meta(c, addr, size, group="byte"):
    return c.metadata_get(addr, size=size, group=group).entries


def find_manifest(c):
    for base in range(0x02000000, 0x02400000, 0x100000):
        data = bytes.fromhex(c.memory_read(base, size=0x100000).hex)
        pos = 0
        while True:
            i = data.find(b"DSWBTEST", pos)
            if i < 0:
                break
            addr = base + i
            self_ptr, version = struct.unpack_from("<II", data, i + 8)
            if self_ptr == addr and version == 1:
                return addr
            pos = i + 1
    return None


FIELDS = ["self", "version", "arm_func", "arm_func_end", "thumb_func", "thumb_func_end", "thumb_leaf", "thumb_leaf_end",
          "data_table", "data_table_end", "dma_dst", "dma_size", "payload_buf", "payload_size", "packed_buf", "packed_size",
          "unpacked_buf", "unpacked_size", "car", "car_speed_offset", "car_grip_offset", "car_class_offset", "write_target", "writer_func",
          "main_func", "frame", "status", "ready", "vfs_buf", "vfs_size",
          "tail_target", "tail_target_end", "tail_caller", "tail_caller_end", "decoded_buf", "decoded_size", "car_ratio_offset", "car_root_offset",
          "kb_target", "kb_target_end", "kb_caller", "kb_caller_end", "cond_caller", "cond_caller_end",
          "car_addr_like_offset", "plain_buf", "plain_size", "arm7_box", "car_mix_offset", "car_total_offset",
          "narc_title_buf", "narc_title_size", "narc_model_buf", "narc_model_size", "nested_buf", "nested_size",
          "deep_buf", "deep_size", "carc_buf", "carc_size", "sseq_buf", "sseq_size",
          "gfx_nclr_buf", "gfx_nclr_size", "gfx_ncgr_buf", "gfx_ncgr_size", "gfx_nscr_buf", "gfx_nscr_size",
          "gfx_ncer_buf", "gfx_ncer_size", "gfx_btx_buf", "gfx_btx_size"]
BOX_FIELDS = ["src", "size", "copy_dst", "lut_dst", "tail_target", "tail_target_end", "tail_caller", "tail_caller_end",
              "done", "frames"]


def read_manifest(c, addr):
    raw = bytes.fromhex(c.memory_read(addr + 8, size=4 * len(FIELDS)).hex)
    return dict(zip(FIELDS, struct.unpack("<%dI" % len(FIELDS), raw)))


def wait_ready(c, addr_holder, frames=600):
    for _ in range(frames // 30):
        c.execution_control("run_frames", count=30)
        if addr_holder[0] is None:
            addr_holder[0] = find_manifest(c)
        if addr_holder[0]:
            m = read_manifest(c, addr_holder[0])
            if m["ready"]:
                return m
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--port", type=int, default=47900)
    ap.add_argument("--keep", action="store_true", help="leave melonDS running")
    a = ap.parse_args()

    exe = os.path.abspath(a.melonds)
    rom = os.path.abspath(a.rom)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    # the stand-in compare command, for kb.build.compare (restored at the end)
    toml = os.path.join(exedir, "melonDS.toml")
    toml_orig = open(toml, encoding="utf-8").read() if os.path.exists(toml) else ""
    compare = 'python "%s" --function {addr} --file {file}' % os.path.join(HERE, "fake_compare.py").replace(os.sep, "/")
    cfg = toml_orig if "[Workbench]\n" in toml_orig else toml_orig + "\n[Workbench]\n"
    srcroot = tempfile.mkdtemp(prefix="dswb_src_")   # the decomp source tree: kb.function.get reads source text only inside it
    cfg = re.sub(r"(?m)^(CompareCommand|SourceRoot|CompareApproved) = .*\n", "", cfg)
    cfg = cfg.replace("[Workbench]\n", "[Workbench]\nCompareCommand = '%s'\nSourceRoot = '%s'\nCompareApproved = '%s'\n"
                      % (compare, srcroot, compare_approval.fingerprint(compare)), 1)
    open(toml, "w", encoding="utf-8").write(cfg)
    kbdir = tempfile.mkdtemp(prefix="dswb_rec_")      # a fresh knowledge store (and sessions folder) per run
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--record", "--kb", os.path.join(kbdir, "knowledge.db"), rom],
                            cwd=exedir, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        c = Client.connect(info)
        c.subscribe()

        print("status")
        st = c.status_get()
        check(st.recording.enabled, "recording is on from boot (--record)")
        check(st.rom_gamecode != "", "ROM loaded: %s" % st.rom_title)

        # data origin and write history must be on before the ROM loads its files: enable, then reset
        r = c.recording_configure(write_history=True, data_origin=True)
        check(r.recording.write_history and r.recording.data_origin, "write history and data origin switched on")
        c.game_reset()
        time.sleep(0.3)
        holder = [None]
        m = wait_ready(c, holder)
        check(m is not None, "test ROM published its manifest")
        if not m:
            return 1
        print("manifest at %08x, status %d" % (holder[0], m["status"]))
        check(m["status"] == 0, "test ROM read its NitroFS files (status %d)" % m["status"])
        c.execution_control("run_frames", count=60)

        print("code and data")
        arm = meta(c, m["arm_func"], m["arm_func_end"] - m["arm_func"])
        code = arm[:20]
        pool = arm[20:24]
        check(all(e.exec > 0 and e.mode == "arm" for e in code), "ARM function bytes executed in ARM mode")
        check(all(("instr_start" in e.flags) == ((e.addr - m["arm_func"]) % 4 == 0) for e in code), "ARM instruction starts every 4 bytes")
        check(all(e.exec == 0 and "literal_pool" in e.flags and e.mem_class == "data" for e in pool), "ARM literal pool read as data, never executed")
        th = meta(c, m["thumb_func"], 12)
        check(all(e.exec > 0 and e.mode == "thumb" for e in th), "Thumb function bytes executed in Thumb mode")
        check(all(("instr_start" in e.flags) == ((e.addr - m["thumb_func"]) % 2 == 0) for e in th), "Thumb instruction starts every 2 bytes")
        tpool = meta(c, (m["thumb_func_end"] - 4), 4)
        check(all(e.exec == 0 and "literal_pool" in e.flags for e in tpool), "Thumb literal pool read as data")
        leaf = meta(c, m["thumb_leaf"], 4)
        check(all(e.exec > 0 and e.mode == "thumb" for e in leaf), "Thumb leaf executed in Thumb mode")
        data = meta(c, m["data_table"], 16)
        check(all(e.exec == 0 and e.read > 0 and e.mem_class == "data" for e in data[:12]), "data table read, never executed")
        check(all(e.dma_read > 0 for e in data), "data table read by DMA")
        dst = meta(c, m["dma_dst"], 16)
        check(all(e.dma_write > 0 and e.last_writer_cpu.startswith("dma9") for e in dst), "DMA destination written by DMA9")
        wt = meta(c, m["write_target"], 4)
        check(all(e.write > 0 and m["writer_func"] <= e.last_writer_pc < m["writer_func"] + 32 for e in wt), "write target's last writer is wb_writer")
        insn = c.metadata_get(m["thumb_func"], size=12, group="instruction").entries
        check([e.size for e in insn][:4] == [2, 2, 2, 2], "instruction grouping follows the recorded Thumb mode")
        dis = c.disasm_get(m["arm_func"], count=6).lines
        check(dis[0].text.startswith("ldr r1, [pc") and "=0x" in dis[0].text, "disassembly of the ARM function: %s" % dis[0].text)
        check(dis[5].text.startswith(".word"), "never-executed literal shown as a data word")
        dis = c.disasm_get(m["thumb_func"], count=2).lines
        check(dis[0].thumb and dis[0].text.startswith("push"), "disassembly picks Thumb from the recording: %s" % dis[0].text)

        print("write history")
        w = c.writes_list(m["write_target"], size=4, limit=10)
        check(w.history_enabled and len(w.entries) > 0, "write history has entries for the write target")
        check(all(m["writer_func"] <= e.pc < m["writer_func"] + 32 for e in w.entries), "every logged write comes from wb_writer")
        if len(w.entries) >= 2:
            e0, e1 = w.entries[-2], w.entries[-1]
            check(e1.old_value == e0.new_value, "old value of a write equals the previous write's new value")
        # bulk writes: Text/decoded.str is written as one sequential run, kept for its first 1 KB only
        head = c.writes_list(m["decoded_buf"] + 100, size=1, limit=4).entries
        tail = c.writes_list(m["decoded_buf"] + 1200, size=1, limit=4).entries
        rs = c.recording_get().recording
        check(head and not tail and rs.history_skipped > 0 and rs.history_bulk_threshold == 1024,
              "write history keeps the start of a bulk write and skips the rest (%d writes skipped)" % rs.history_skipped)
        # explicit filters: skip the writer function, then keep only one address range
        c.recording_configure(history_exclude_writers=[AddrRange(start=m["writer_func"], end=m["writer_func"] + 32)])
        before = len(c.writes_list(m["write_target"], size=4, limit=1000).entries)
        c.execution_control("run_frames", count=10)
        after = len(c.writes_list(m["write_target"], size=4, limit=1000).entries)
        check(after == before, "writes by an excluded writer are not recorded (%d before, %d after)" % (before, after))
        c.recording_configure(history_exclude_writers=[], history_include=[AddrRange(start=m["car"], end=m["car"] + 32)])
        before_t = len(c.writes_list(m["write_target"], size=4, limit=1000).entries)
        before_c = len(c.writes_list(m["car"] + 12, size=4, limit=1000).entries)
        c.execution_control("run_frames", count=10)
        check(len(c.writes_list(m["write_target"], size=4, limit=1000).entries) == before_t and
              len(c.writes_list(m["car"] + 12, size=4, limit=1000).entries) >= before_c + 10,
              "with an include range, only writes inside it are recorded")
        c.recording_configure(history_include=[])
        # paging: the whole history in pages, newest first
        t0 = time.time()
        first = c.writes_list(0, size=0, limit=1000)
        pages, got, skip = 1, len(first.entries), len(first.entries)
        seen = {(e.frame, e.addr, e.pc, e.timestamp) for e in first.entries}
        while got < first.total and pages < 1000:
            pg = c.writes_list(0, size=0, limit=100000, skip=skip)
            if not pg.entries:
                break
            seen.update((e.frame, e.addr, e.pc, e.timestamp) for e in pg.entries)
            got += len(pg.entries)
            skip += len(pg.entries)
            pages += 1
        dt = time.time() - t0
        check(first.truncated and got == first.total and len(seen) == got and dt < 30,
              "the whole history (%d writes) reads back in %d pages in %.1f s" % (first.total, pages, dt))
        md = c.metadata_get(0x02000000, size=0x40000, group="byte")
        check(md.truncated and md.next_addr == 0x02000000 + len(md.entries), "metadata.get says where a long range continues")
        cv = c.coverage_get(region="main_ram", kind="read", limit=3)
        check(cv.truncated and cv.next_addr > 0, "coverage.get says where to continue")
        # a history file: 1 MB holds 32K writes, so it fills and drops its oldest writes
        rs = c.recording_configure(history_file_mb=1).recording
        check(rs.history_file_mb == 1 and rs.history_cap == (1 << 20) // 32 and rs.history_entries == 0,
              "write history moves to a 1 MB file (%d entries)" % rs.history_cap)
        c.recording_configure(history_bulk_threshold=0, history_skip_unchanged=False)
        c.execution_control("run_frames", count=300)
        rs = c.recording_get().recording
        w = c.writes_list(m["write_target"], size=4, limit=5)
        check(rs.history_wrapped and rs.history_entries == rs.history_cap and rs.history_oldest_frame > 0 and
              w.history_wrapped and w.oldest_frame == rs.history_oldest_frame and w.entries,
              "a full history file drops its oldest writes and says from which frame it is complete (frame %d)" % rs.history_oldest_frame)
        rs = c.recording_configure(history_file_mb=0, history_bulk_threshold=1024, history_skip_unchanged=True).recording
        check(rs.history_file_mb == 0 and not rs.history_wrapped, "and back to memory")

        print("calls")
        calls = c.calls_get(m["thumb_leaf"])
        check(any(m["thumb_func"] <= e.call_site < m["thumb_func_end"] for e in calls.callers), "wb_thumb_leaf is called from wb_thumb_func")
        calls = c.calls_get(m["thumb_func"])
        check(any(e.callee == m["thumb_leaf"] for e in calls.callees), "wb_thumb_func's callees include wb_thumb_leaf")
        calls = c.calls_get(m["arm_func"])
        check(len(calls.callers) > 0 and calls.callers[0].count > 60, "wb_arm_func called every frame (%d)" % (calls.callers[0].count if calls.callers else 0))
        calls = c.calls_get(m["tail_target"])
        tails = [e for e in calls.callers if e.kind == "tail_call"]
        check(any(m["tail_caller"] <= e.call_site < m["tail_caller_end"] and e.caller_func == m["tail_caller"] and e.count > 60 for e in tails),
              "the plain branch from wb_tail_caller is recorded as a tail call: %s" % [(hex(e.call_site), e.kind) for e in calls.callers])
        check(any(e.kind == "call" for e in calls.callers), "the normal call to wb_tail_target is still a call")
        check(c.calls_get(m["tail_target"] + 4).function == m["tail_target"],
              "code inside the tail-called function is credited to it, not to its caller")
        calls = c.calls_get(m["tail_caller"])
        check(any(e.callee == m["tail_target"] and e.kind == "tail_call" for e in calls.callees), "wb_tail_caller lists the tail call among its callees")
        calls = c.calls_get(m["tail_target"])
        check(any(m["cond_caller"] <= e.call_site < m["cond_caller_end"] and e.kind == "tail_call" for e in calls.callers),
              "a conditional branch into a known entry is a tail call too")
        # a function reached only by a tail call: unknown until the knowledge store defines it
        before = [e for e in c.calls_get(m["kb_target"]).callers if e.kind == "tail_call"]
        kb_agent = Client.connect(info, agent="tail-test")
        check(kb_agent.kb_function_define(m["kb_target"], size=m["kb_target_end"] - m["kb_target"], name="KbOnlyTarget").ok,
              "the knowledge store defines wb_kb_target")
        kb_agent.close()
        time.sleep(0.3)
        c.execution_control("run_frames", count=5)
        after = [e for e in c.calls_get(m["kb_target"]).callers if e.kind == "tail_call"]
        check(not before and any(m["kb_caller"] <= e.call_site < m["kb_caller_end"] for e in after),
              "once it is in the knowledge store, the plain branch into it is recorded as a tail call")

        print("coverage")
        cov = c.coverage_get(addr=m["arm_func"], size=0x100, kind="exec")
        check(any(r.start == m["arm_func"] and r.end == m["arm_func"] + 20 and r.mode == "arm" for r in cov.ranges), "coverage range of the ARM function stops before its literal pool")

        print("provenance and data origin")
        o = c.origin_get(m["payload_buf"], size=4)
        check(any(x.kind == "card_read" and x.file_name.endswith("payload.bin") for x in o.chain), "payload buffer came from ROM file payload.bin via a card read")
        check(any("payload.bin" in s for t in o.taint for s in t.sources), "payload buffer carries the payload.bin label")
        o = c.origin_get(m["unpacked_buf"] + 8, size=4)
        kinds = [x.kind for x in o.chain]
        check("decompress" in kinds, "unpacked buffer came from a BIOS decompression (%s)" % kinds)
        check(any(x.kind == "card_read" and x.file_name.endswith("packed.lz") for x in o.chain), "decompression source traced back to packed.lz")
        o = c.origin_get(m["car"] + m["car_speed_offset"], size=4)
        check(any("packed.lz" in s for t in o.taint for s in t.sources), "car.speed derived from packed.lz (data origin)")
        speed = m["car"] + m["car_speed_offset"]
        rr = c.origin_ranges(source="packed.lz")
        check(rr.sources == ["/packed.lz"] and any(q.start <= speed < q.end for q in rr.ranges)
              and any(q.start <= m["unpacked_buf"] + 8 < q.end for q in rr.ranges),
              "origin.ranges finds packed.lz's data: its unpacked copy and car.speed (%d ranges)" % len(rr.ranges))
        try:
            c.origin_ranges(source="no_such_file.bin")
            check(False, "origin.ranges refuses an unknown source")
        except Exception as e:
            check("no data origin source" in str(e), "origin.ranges refuses an unknown source")
        o = c.origin_get(m["car"] + 12, size=4)
        check(not any("packed.lz" in s for t in o.taint for s in t.sources), "car.frames is not labeled with packed.lz")
        o = c.origin_get(m["car"] + m["car_class_offset"], size=4)
        check(not any("packed.lz" in s for t in o.taint for s in t.sources), "a table lookup indexed by file data is not labeled by default")
        # merged labels say how they were made
        o = c.origin_get(m["car"] + m["car_mix_offset"], size=4)
        t = o.taint[0] if len(o.taint) == 1 else None
        check(t is not None and any("packed.lz" in x for x in t.sources) and any("plain.lz" in x for x in t.sources) and
              t.how == "computed" and m["main_func"] <= t.how_pc < 0x02400000 and t.how_steps == 1,
              "car.mix's label names packed.lz and plain.lz, computed by one instruction: %s"
              % (((t.sources, t.how, hex(t.how_pc), t.how_steps) if t else [x.sources for x in o.taint]),))
        if t:
            tr = c.origin_label(t.label)
            parts = [tr.nodes[i] for i in tr.nodes[0].parts]
            check(tr.nodes[0].pc == t.how_pc and len(parts) == 2 and all(p.how == "source" for p in parts),
                  "origin.label shows the two files it joined: %s" % [(p.how, p.sources) for p in parts])
        o = c.origin_get(m["car"] + m["car_total_offset"], size=4)
        t = o.taint[0] if len(o.taint) == 1 else None
        tr = c.origin_label(t.label) if t else None
        names = [tr.nodes[i].sources[0] for i in tr.nodes[0].parts if tr.nodes[i].how == "source"] if tr else []
        check(t is not None and t.how == "computed" and t.how_steps == 2 and len(t.sources) == 3 and
              tr.nodes[0].steps == 2 and len(names) == 3 and "packed.lz" in names[0] and "plain.lz" in names[1] and "test.nsbmd" in names[2],
              "car.total is a running total: one add instruction joined three files in turn, listed oldest first: %s"
              % (((t.how, t.how_steps, hex(t.how_pc), names) if t else None),))
        try:
            c.origin_label(10 ** 7)
            check(False, "origin.label refuses an unknown label")
        except ApiError:
            check(True, "origin.label refuses an unknown label")
        for name, off in (("hardware divider", "car_ratio_offset"), ("hardware square root", "car_root_offset")):
            o = c.origin_get(m["car"] + m[off], size=4)
            check(any("packed.lz" in s for t in o.taint for s in t.sources),
                  "a value computed on the %s keeps its operand's label: %s" % (name, [t.sources for t in o.taint]))
        check(any(x.kind == "computed" for x in o.chain), "origin names the code that computed car.class_id %s" % [(x.kind, hex(x.pc), x.detail) for x in o.chain])
        o = c.origin_get(m["unpacked_buf"] + 8, size=4)
        check(any(x.kind == "card_read" and x.file_name.endswith("packed.lz") and x.pc for x in o.file_reads),
              "origin lists the card read of packed.lz with its loader code")
        o = c.origin_get(m["arm_func"], size=4)
        check(any(x.file_name == "arm9.bin" for x in o.chain) or any("arm9.bin" in s for t in o.taint for s in t.sources),
              "ARM function bytes trace back to arm9.bin %s" % [(x.kind, x.file_name, hex(x.src_addr), hex(x.dst_addr), x.dst_size) for x in o.chain])

        print("ARM, Thumb and data")
        lines = c.disasm_get(m["thumb_func"], count=2).lines
        check(lines and lines[0].classification == "thumb" and lines[0].confidence == "confirmed" and lines[0].thumb
              and "as Thumb" in lines[0].why, "executed Thumb code is classified Thumb, confirmed: %s"
              % [(l.classification, l.confidence, l.why) for l in lines[:1]])
        lines = c.disasm_get(m["data_table"], count=1).lines
        check(lines and lines[0].classification == "data" and lines[0].confidence == "confirmed" and lines[0].text.startswith(".word"),
              "a table that is read and never executed is data, confirmed: %s" % [(l.classification, l.why) for l in lines])
        pool = [l for l in c.disasm_get(m["arm_func"], count=(m["arm_func_end"] - m["arm_func"]) // 4).lines if l.classification == "data"]
        check(len(pool) == 1 and "literal pool" in pool[0].why, "the literal pool inside wb_arm_func is data")
        md = c.metadata_get(m["thumb_func"], size=2, group="byte").entries
        check(md and md[0].classification == "thumb" and md[0].confidence == "confirmed", "metadata.get gives the same classification")
        dec = Client.connect(info, agent="decode-test")
        check(dec.kb_decode_set(m["arm_func"], m["arm_func"] + 8, "data").ok, "an agent marks two ARM instructions as data")
        lines = c.disasm_get(m["arm_func"], count=3).lines
        check(lines[0].classification == "data" and lines[0].confidence == "override" and lines[0].conflict
              and lines[0].text.startswith(".word") and "decode-test" in lines[0].why and lines[2].classification == "arm",
              "the override decodes them as data and flags the conflict with the recording: %s" % lines[0].why)
        ov = c.kb_list("decode_overrides", author="decode-test", conflicts_only=True).decode_overrides
        check(len(ov) == 1 and ov[0].start == m["arm_func"] and ov[0].end == m["arm_func"] + 8 and ov[0].mode == "data"
              and ov[0].conflict.startswith("marked data, but executed"),
              "kb.list lists the override with its conflict, as the Annotations panel's decode conflicts: %s"
              % [(hex(o.start), o.conflict) for o in ov])
        check(dec.kb_decode_set(m["arm_func"], m["arm_func"] + 4, "clear").ok
              and c.disasm_get(m["arm_func"], count=2).lines[0].classification == "arm"
              and c.disasm_get(m["arm_func"] + 4, count=1).lines[0].confidence == "override",
              "clearing part of an override trims it")
        check(dec.kb_undo().ok and c.disasm_get(m["arm_func"], count=1).lines[0].confidence == "override",
              "undo brings the cleared part back")
        check(dec.kb_decode_set(m["arm_func"], m["arm_func"] + 8, "clear").ok, "the override is cleared")
        try:
            dec.kb_decode_set(m["arm_func"], m["arm_func"] + 8, "code")
            check(False, "an unknown mode is refused")
        except ApiError as e:
            check("mode must be" in str(e), "an unknown mode is refused")
        dec.close()

        print("content labelling")
        # Text/decoded.str went through a lookup table (table lookups are not followed yet): its label comes from content seeding
        od = c.origin_get(m["decoded_buf"] + 100, size=16)
        dl = [src for t in od.taint for src in t.sources]
        check(m["decoded_size"] > 1000 and dl == ["/vfs.zip:Text/decoded.str"] and od.taint[0].size == 16,
              "a file unpacked through a lookup table is labelled by its content: %s" % dl)
        check(c.status_get().recording.origin_seeded_files > 0, "the recording reports the seeded chunks")
        # plain.lz is a plain NitroFS file the ROM unpacks in software through the same table
        op = c.origin_get(m["plain_buf"] + 200, size=16)
        pl = [src for t in op.taint for src in t.sources]
        check(m["plain_size"] > 700 and pl == ["/plain.lz"] and op.taint[0].size == 16,
              "a plain ROM file unpacked through a lookup table is labelled by its unpacked content: %s" % pl)

        print("arm7")
        box = {}
        for _ in range(20):
            raw = bytes.fromhex(c.memory_read(m["arm7_box"], size=4 * len(BOX_FIELDS)).hex)
            box = dict(zip(BOX_FIELDS, struct.unpack("<%dI" % len(BOX_FIELDS), raw)))
            if box["done"] and box["frames"] > 60:
                break
            c.execution_control("run_frames", count=30)
        check(box.get("done") == 1 and box["size"] == 256 and box["frames"] > 60,
              "the test ROM's own ARM7 code ran (%s)" % {k: hex(v) for k, v in box.items()})
        if box.get("done"):
            # an archived file's bytes, copied on the ARM7: as they are, and through a lookup table
            o = c.origin_get(box["copy_dst"] + 64, size=16)
            srcs = [src for t in o.taint for src in t.sources]
            check(srcs == ["/vfs.zip:Models/test.nsbmd"], "an archived file copied by the ARM7 keeps its label: %s" % srcs)
            o = c.origin_get(box["lut_dst"] + 64, size=16)
            srcs = [src for t in o.taint for src in t.sources]
            check(srcs == ["/vfs.zip:Models/test.nsbmd"] and o.taint[0].size == 16,
                  "an archived file copied by the ARM7 through a lookup table is labelled by its content: %s" % srcs)
            calls = c.calls_get(box["tail_target"], cpu="arm7")
            check(any(box["tail_caller"] <= e.call_site < box["tail_caller_end"] and e.kind == "tail_call" for e in calls.callers),
                  "the ARM7's plain branch into a known function is recorded as a tail call: %s"
                  % [(hex(e.call_site), e.kind) for e in calls.callers])
            check(c.calls_get(box["tail_target"] + 4, cpu="arm7").function == box["tail_target"],
                  "ARM7 code inside the tail-called function is credited to it, not to its caller")

        print("table lookups")
        r = c.recording_configure(origin_follow_addresses=True)
        check(r.recording.origin_follow_addresses, "follow table lookups switched on")
        c.game_reset()
        time.sleep(0.3)
        m2 = wait_ready(c, [holder[0]])
        check(m2 is not None, "test ROM ready again after reset")
        o = c.origin_get(m["car"] + m["car_class_offset"], size=4)
        check(any("packed.lz" in s for t in o.taint for s in t.sources), "with table lookups followed, car.class_id is labeled packed.lz")
        c.execution_control("run_frames", count=5)
        o = c.origin_get(m["car"] + m["car_addr_like_offset"], size=4)
        check(any("packed.lz" in s for t in o.taint for s in t.sources),
              "file data that looks like an address keeps its label in arithmetic: %s" % [t.sources for t in o.taint])
        c.recording_configure(origin_follow_addresses=False)

        print("breakpoints and stepping")
        bp = c.breakpoint_set("exec", m["writer_func"])
        c.execution_control("resume")
        ev = c.wait_event("breakpoint_hit", 10)
        check(ev["break_info"]["pc"] == m["writer_func"], "execute breakpoint stops at wb_writer")
        st = c.status_get()
        check(st.state == "break" and st.break_info is not None, "status reports the break state")
        frame_arg = st.registers[0].r[0]
        s1 = c.execution_control("step", count=1)
        check(s1.state == "break" and s1.pc == m["writer_func"] + 4, "step advances one ARM instruction (%08x)" % s1.pc)
        c.breakpoint_clear(bp.id)
        c.execution_control("resume")
        wp = c.breakpoint_set("write", m["write_target"], size=4, condition="value == %d" % ((frame_arg + 5) * 3 + 1))
        ev = c.wait_event("breakpoint_hit", 10)
        check(ev["hit"]["write"] and ev["hit"]["access_addr"] == m["write_target"], "write breakpoint with a condition hits on the write target %s" % ev)
        check(m["writer_func"] <= ev["hit"]["pc"] < m["writer_func"] + 32, "write breakpoint reports the writing instruction")
        c.breakpoint_clear(-1)
        c.execution_control("resume")
        bl = c.breakpoints_list()
        check(len(bl.breakpoints) == 0 and len(bl.hits) >= 2, "breakpoints cleared, hit list kept")

        # breakpoint.update: enable, disable and change the condition, keeping the hit count
        bp = c.breakpoint_set("exec", m["writer_func"])
        c.wait_event("breakpoint_hit", 10)
        u = c.breakpoint_update(bp.id, enabled=False)
        check(not u.enabled and u.hit_count == 1, "breakpoint.update disables a breakpoint and keeps its hit count")
        c.execution_control("resume")
        r = c.execution_control("run_frames", count=5)
        hc = [x.hit_count for x in c.breakpoints_list().breakpoints if x.id == bp.id]
        check(not r.interrupted and hc == [1], "a disabled breakpoint does not stop (%s)" % hc)
        u = c.breakpoint_update(bp.id, enabled=True, condition="r0 == 0xFFFFFFFF")
        r = c.execution_control("run_frames", count=5)
        check(u.enabled and u.condition == "r0 == 0xFFFFFFFF" and u.hit_count == 1 and not r.interrupted,
              "breakpoint.update enables it with a condition that never holds: no stop")
        try:
            c.breakpoint_update(bp.id, condition="r0 ==")
            check(False, "a bad condition is refused")
        except ApiError:
            cond = [x.condition for x in c.breakpoints_list().breakpoints if x.id == bp.id]
            check(cond == ["r0 == 0xFFFFFFFF"], "a bad condition is refused and changes nothing (%s)" % cond)
        u = c.breakpoint_update(bp.id, condition="")
        c.execution_control("resume")
        c.wait_event("breakpoint_hit", 10)
        hc = [x.hit_count for x in c.breakpoints_list().breakpoints if x.id == bp.id]
        check(u.condition == "" and hc == [2], "removing the condition makes it stop again, counting on from 1 (%s)" % hc)
        try:
            c.breakpoint_update(9999, enabled=False)
            check(False, "breakpoint.update refuses an unknown id")
        except ApiError:
            check(True, "breakpoint.update refuses an unknown id")
        c.breakpoint_clear(-1)
        c.execution_control("resume")

        print("input, screens, savestates")
        r = c.input_send(buttons=["a"], frames=3)
        check(r.frame > 0 and len(r.top_png) > 100, "input.send returns the frame and both screens")
        c.execution_control("pause")
        r = c.state_save(slot=7)
        check(r.ok, "savestate saved")
        r = c.state_load(slot=7)
        check(r.ok, "savestate loaded")
        # data origin was rebuilt from content: what still equals the ROM is labelled, computed data is not
        def srcs(addr, size=4):
            o = c.origin_get(addr, size=size)
            return o, [x for t in o.taint for x in t.sources]
        # the test ROM's code runs from a copy of arm9.bin that boot code relocated: found by content
        o, l = srcs(m["arm_func"])
        check(o.state_loaded_frame > 0 and any(x.endswith("arm9.bin") for x in l),
              "after a savestate load, relocated code that equals part of arm9.bin is labelled again (state at frame %d): %s" % (o.state_loaded_frame, l))
        o, l = srcs(m["vfs_buf"] + 0x40, 16)
        check(l == ["/vfs.zip:Models/test.nsbmd"], "an archived file found in RAM is labelled again by content: %s" % l)
        o, l = srcs(m["unpacked_buf"] + 0x40, 16)
        check(l == ["/packed.lz"], "a packed ROM file's unpacked copy is labelled again by content: %s" % l)
        o, l = srcs(m["car"] + m["car_speed_offset"])
        check(not l, "a field computed before the state was saved has no origin after the load: %s" % l)
        mw = None
        try:
            c.memory_write(m["write_target"], hex="00000000")
        except ApiError as e:
            mw = e.code
        check(mw == -32003, "memory.write is refused unless enabled")
        try:
            c.read.call("game.reset", {})
            check(False, "control method refused on the read endpoint")
        except ApiError:
            check(True, "control method refused on the read endpoint")

        print("agent edits through the API")
        ag = Client.connect(info, agent="test-agent")
        r1 = ag.kb_function_define(m["arm_func"], size=m["arm_func_end"] - m["arm_func"], name="Agent_ArmSum")
        r2 = ag.kb_annotation_save(comment="running sum", target_kind="register", addr=m["arm_func"], reg="r0")
        r3 = ag.kb_data_define(m["data_table"], type="u32[4]", name="gAgentTable")
        r4 = ag.kb_label_set(m["data_table"] + 16, name="agent_after_table")
        check(r1.ok and r1.id and r2.ok and r2.id and r3.ok and r4.ok, "an agent defines a function, annotates a register, defines data and sets a label")
        lines = c.disasm_get(m["data_table"], count=2).lines
        check(len(lines) == 2 and lines[0].text.startswith(".word") and "gAgentTable[0]" in lines[0].text and "gAgentTable[1]" in lines[1].text,
              "disasm.get shows defined data by its type's elements: %s" % [l.text for l in lines])
        check(ag.kb_function_define(m["thumb_func"]).ok, "kb.function.define picks the recorded Thumb mode on its own")
        ch = ag.kb_changes_list(limit=10).changes
        mine = [x for x in ch if x.author == "test-agent"]
        check(len(mine) >= 5 and all(x.author_kind == "agent" and x.agent_session.startswith("test-agent#") for x in mine),
              "the change log attributes the edits to the agent and its session: %s" % sorted({x.table for x in mine}))
        newest = mine[0].id
        u = ag.kb_undo()
        ch = ag.kb_changes_list(limit=10).changes
        check(u.ok and any(x.id == newest and x.undone for x in ch), "kb.undo reverts the agent's newest change")
        check(ag.kb_redo().ok and not any(x.id == newest and x.undone for x in ag.kb_changes_list(limit=10).changes),
              "kb.redo puts it back")
        try:
            ag.read.call("kb.rename", {"addr": m["arm_func"], "name": "x"})
            check(False, "the read endpoint refuses knowledge-store writes")
        except ApiError:
            check(True, "the read endpoint refuses knowledge-store writes")
        try:
            ag.kb_annotation_save(comment="", target_kind="address", addr=m["arm_func"])
            check(False, "an empty annotation is refused")
        except ApiError:
            check(True, "an empty annotation is refused")
        print("agent edits: deletes, source files, builds, imports")
        bm = ag.kb_bookmark_add(m["data_table"], name="agent mark")
        check(bm.ok and bm.id and ag.kb_bookmark_delete(bm.id).ok, "an agent adds and deletes a bookmark (id %d)" % bm.id)
        check(ag.kb_type_define("struct AgentPair { int a; int b; };").ok and ag.kb_type_delete("AgentPair").ok,
              "an agent defines and deletes a type")
        check(ag.kb_label_delete(m["data_table"] + 16).ok, "an agent deletes its label")
        check(ag.kb_data_delete(m["data_table"]).ok, "an agent deletes a data object")
        ch = ag.kb_changes_list(limit=1).changes[0]
        check(ag.kb_undo().ok and ag.kb_changes_list(limit=1).changes[0].undone and ch.action == "delete" and ch.table == "data_object",
              "the delete is in the change log and kb.undo brings the data object back")
        src = os.path.join(srcroot, "arm_sum.c")
        with open(src, "w") as f:
            f.write("int Agent_ArmSum(void) { return 0; } /* MATCH */\n")
        check(ag.kb_function_update(m["arm_func"] + 4, source_file=src, signature="int Agent_ArmSum(void)").ok,
              "an agent sets a function's source file and signature (by any address inside it)")
        cr = ag.kb_build_compare(m["arm_func"])
        check(cr.ok and cr.matched and cr.score == 100.0 and not cr.recorded_by_tool,
              "kb.build.compare runs the compare command and reports the result (%.1f%%, matched %s)" % (cr.score, cr.matched))
        check(ag.kb_build_record(m["thumb_func"], compiler="mwccarm (agent)", score=61.5, matched=False).ok,
              "an agent records a build result")
        imp = os.path.join(os.path.dirname(src), "analysis.json")
        with open(imp, "w") as f:
            json.dump({"tool": "agent-tool", "labels": [{"addr": "0x%08x" % (m["data_table"] + 32), "name": "agent_import_label"}],
                       "decompiled": [{"addr": "0x%08x" % m["arm_func"],
                                       "text": "int FUN_%08x(void) { return DAT_%08x[0]; }" % (m["arm_func"], m["data_table"])}]}, f)
        ir = ag.kb_import_static_analysis(imp)
        check(ir.ok and ir.count == 2, "an agent imports a static analysis export (%d entries)" % ir.count)
        check(ag.kb_function_delete(m["thumb_func"]).ok and ag.kb_undo().ok, "an agent deletes a function, and undo restores it")
        try:
            ag.kb_function_delete(0x02000000 + 0x3FFFF0)
            check(False, "deleting a function where there is none is refused")
        except ApiError:
            check(True, "deleting a function where there is none is refused")

        print("knowledge-store reads: the panels' lists and one function (read endpoint)")
        fl = c.kb_list("functions", filter="agent_armsum")
        f0 = fl.functions[0] if fl.functions else None
        check(fl.total == 1 and f0 and f0.addr == m["arm_func"] and f0.author == "test-agent" and f0.author_kind == "agent"
              and f0.signature == "int Agent_ArmSum(void)" and f0.status == "matched" and f0.exec_count > 0,
              "kb.list finds the agent's function by name, with its status and how often it ran: %s"
              % ((f0.status, f0.exec_count) if f0 else None,))
        allf = c.kb_list("functions", limit=0)
        check(allf.total >= 3 and allf.functions == [] and allf.truncated, "kb.list with limit 0 only counts (%d functions)" % allf.total)
        ann = c.kb_list("annotations", author="test-agent", target_kind="register").annotations
        check(len(ann) == 1 and ann[0].reg == "r0" and ann[0].function == m["arm_func"] and ann[0].comment == "running sum",
              "kb.list lists the agent's register annotation, on its function")
        lbl = c.kb_list("labels", filter="agent_import").labels
        check(len(lbl) == 1 and lbl[0].addr == m["data_table"] + 32 and lbl[0].module == "arm9", "kb.list lists the imported label")
        dat = c.kb_list("data_objects", addr=m["data_table"] + 8).data_objects
        check(len(dat) == 1 and dat[0].name == "gAgentTable" and dat[0].size == 16 and dat[0].type == "u32[4]",
              "kb.list finds the data object containing an address (the one undo brought back)")
        mods = c.kb_list("modules", cpu="arm9").modules
        check(any(x.name == "arm9" and x.kind == "static" for x in mods) and all(x.cpu == "arm9" for x in mods),
              "kb.list lists the ROM's modules: %s" % [x.name for x in mods])
        try:
            c.kb_list("functions", tag="todo")
            check(False, "kb.list refuses a filter the kind does not have")
        except ApiError as e:
            check("does not apply" in str(e), "kb.list refuses a filter the kind does not have")
        fd = c.kb_function_get(m["arm_func"] + 4)
        check(fd.function.addr == m["arm_func"] and fd.display_name == "Agent_ArmSum" and fd.source_status == "ok"
              and "MATCH" in fd.source_text and fd.attempts and fd.attempts[0].matched and fd.attempts[0].score == 100.0,
              "kb.function.get gives the source text (inside the source root) and the Compare attempts, newest first: %s"
              % ((fd.source_status, len(fd.attempts)),))
        check(fd.decompiler_tool == "agent-tool" and fd.decompiled_named == "int Agent_ArmSum(void) { return gAgentTable[0]; }",
              "kb.function.get gives the decompiler output with the knowledge store's names: %s" % fd.decompiled_named)
        check(any(a.reg == "r0" for a in fd.annotations), "kb.function.get gives the function's annotations")
        tid = c.kb_list("functions", addr=m["thumb_func"]).functions[0].id
        td = c.kb_function_get(id=tid)
        check(td.function.mode == "thumb" and td.attempts and td.attempts[0].compiler == "mwccarm (agent)"
              and td.attempts[0].score == 61.5 and td.source_status == "none",
              "kb.function.get by id gives the build result an agent recorded")
        outside = os.path.join(tempfile.mkdtemp(prefix="dswb_out_"), "secret.c")
        with open(outside, "w") as f:
            f.write("outside the source tree\n")
        ag.kb_function_update(m["thumb_func"], source_file=outside)
        td = c.kb_function_get(m["thumb_func"])
        check(td.source_status == "outside_source_root" and td.source_text == "" and td.source_path == outside,
              "kb.function.get gives no text for a source file outside the source root (%s)" % td.source_status)
        ag.close()

        print("panel tools: cross-references, overlays, search, value scans")
        xr = c.xrefs_get(m["data_table"])
        lit = [x for x in xr.refs if x.kind == "literal" and m["arm_func"] <= x.from_addr < m["arm_func_end"]]
        check(lit and lit[0].function == m["arm_func"] and lit[0].function_name == "Agent_ArmSum" and xr.name == "gAgentTable",
              "xrefs.get finds wb_arm_func's literal-pool load of wb_data_table, named: %s" % [(x.kind, hex(x.from_addr)) for x in xr.refs])
        xw = c.xrefs_get(m["write_target"])
        wr = [x for x in xw.refs if x.kind == "write" and m["writer_func"] <= x.from_addr < m["writer_func"] + 32]
        check(wr and wr[0].count > 0 and wr[0].source == "runtime", "xrefs.get lists wb_writer's writes with their count: %s"
              % [(x.kind, hex(x.from_addr), x.count) for x in xw.refs])
        xc = c.xrefs_get(m["arm_func"])
        check(any(x.kind == "call" and x.count > 0 for x in xc.refs), "xrefs.get lists the observed calls of a function")
        check(len(c.xrefs_get(m["write_target"], limit=0).refs) == 0 and c.xrefs_get(m["write_target"], limit=0).truncated,
              "xrefs.get honours limit")
        ol = c.overlays_list()
        check(ol.overlays == [] and ol.events == [] and ol.events_total == 0, "overlays.list: the test ROM has no overlays")
        try:
            c.disasm_get(m["arm_func"], count=2, overlay=0)
            check(False, "disasm.get refuses an overlay the ROM doesn't have")
        except ApiError as e:
            check("no arm9 overlay 0" in str(e), "disasm.get refuses an overlay the ROM doesn't have")
        c.execution_control("pause")
        sr = c.search_string("DSWBTEST")
        check(any(x.addr == m["self"] for x in sr.matches) and sr.size == 8, "search.string finds the manifest's magic (%d matches)" % len(sr.matches))
        sb = c.search_bytes("44 53 57 42 ?? 45 53 54", region="main_ram")
        check([x.addr for x in sb.matches] == [x.addr for x in sr.matches] and sb.matches[0].preview.startswith("DSWB"),
              "search.bytes with a wildcard finds the same places")
        check(c.search_string("DSWBTEST", encoding="sjis").matches[0].addr == sr.matches[0].addr, "search.string in Shift-JIS (ASCII range)")
        check(c.search_string("DSWBTEST", limit=1).truncated == (len(sr.matches) > 1), "search results are limited")
        check(c.search_string("D\u00e9j\u00e0 vu nowhere", encoding="utf16le").matches == [], "search.string in UTF-16 finds nothing absent")
        for bad in [("search_bytes", ("12 345",)), ("search_bytes", ("1234567",)), ("search_bytes", ("?? ??",)), ("search_bytes", ("zz",)), ("search_string", ("x",), {"encoding": "ebcdic"}),
                    ("search_string", ("日本",))]:
            try:
                getattr(c, bad[0])(*bad[1], **(bad[2] if len(bad) > 2 else {}))
                check(False, "%s refuses %s" % (bad[0], bad[1:]))
            except ApiError:
                check(True, "%s refuses %s" % (bad[0], bad[1:]))
        # value scan for the manifest's frame counter, across pauses
        frame_addr = m["self"] + 8 + 4 * FIELDS.index("frame")
        s0 = c.search_scan(type="u32", condition="unknown")
        c.execution_control("run_frames", count=3)
        s1 = c.search_scan(scan_id=s0.scan_id, condition="increased")
        c.execution_control("run_frames", count=3)
        s2 = c.search_scan(scan_id=s0.scan_id, condition="increased")
        s3 = c.search_scan(scan_id=s0.scan_id, condition="unchanged", limit=100000)
        now = struct.unpack("<I", bytes.fromhex(c.memory_read(frame_addr, size=4).hex))[0]
        s4 = c.search_scan(scan_id=s0.scan_id, condition="equal", value=now, limit=100000)
        check(s0.candidates >= 0x100000 and s0.candidates > s1.candidates >= s2.candidates > 0 and s3.candidates == s2.candidates,
              "search.scan narrows down: %d, %d, %d, %d" % (s0.candidates, s1.candidates, s2.candidates, s3.candidates))
        check(any(x.addr == frame_addr and x.raw == now and x.value == now for x in s4.matches) and s4.frame > s0.frame,
              "the frame counter survives every step of the scan (%d left)" % s4.candidates)
        f1 = c.search_scan(type="fx32", condition="greater", value="0.5")
        f2 = c.search_scan(type="fx32", condition="greater", value=0.5)
        check(f1.candidates == f2.candidates and f1.scan_id != f2.scan_id and f1.type == "fx32"
              and all(x.value > 0.5 for x in f1.matches[:50]), "an fx32 scan takes a decimal value, as text or a number")
        for kw, what in [({"condition": "changed"}, "a first scan can't compare with a previous one"),
                         ({"condition": "equal"}, "equal needs a value"),
                         ({"scan_id": 9999, "condition": "changed"}, "an unknown scan id is refused"),
                         ({"type": "u24", "condition": "unknown"}, "an unknown type is refused"),
                         ({"scan_id": s0.scan_id, "condition": "unknown"}, "unknown is only for a first scan"),
                         ({"type": "u32", "condition": "equal", "value": -1}, "a u32 scan refuses a negative value (it would never match)"),
                         ({"type": "u8", "condition": "greater", "value": 300}, "a u8 scan refuses a value above 255")]:
            try:
                c.search_scan(**kw)
                check(False, what)
            except ApiError:
                check(True, what)
        ids = [c.search_scan(type="u8", condition="equal", value=0x5A).scan_id for _ in range(8)]
        try:
            c.search_scan(scan_id=s0.scan_id, condition="unchanged")
            check(False, "a connection keeps only its newest 8 scans")
        except ApiError:
            check(c.search_scan(scan_id=ids[-1], condition="unchanged").scan_id == ids[-1], "a connection keeps only its newest 8 scans")
        c.execution_control("resume")

        print("game archive (Octane VFS: vfs.zip, vfs.bin, vfs.dat)")
        lst = c.archive_list()
        for _ in range(50):
            if lst.status != "indexing":
                break
            time.sleep(0.1)
            lst = c.archive_list()
        check(lst.status == "ready" and lst.kind == "octane_vfs" and lst.member_count == 4,
              "the VFS was detected and indexed: %s" % lst.summary)
        paths = [x.path for x in lst.members]
        check(paths == ["Models/test.nsbmd", "Models/Other.bin", "Text/readme.str", "Text/decoded.str"], "archive.list lists the files in index order")
        check([x.vfs_entry for x in lst.members] == [1, 2, 4, 5] and lst.summary.endswith("agrees for 4"),
              "vfs.bin entries map to the ZIP members: %s" % [x.vfs_entry for x in lst.members])
        check([x.method for x in lst.members] == ["stored", "deflate", "deflate", "stored"] and all(x.readable for x in lst.members),
              "stored and deflated members unpack and pass their CRC")
        check(c.archive_list(filter="MODELS/").matched == 2, "archive.list filters by path")
        check(m["vfs_size"] == 768, "test ROM read Models/test.nsbmd out of vfs.zip (%d bytes)" % m["vfs_size"])
        o = c.origin_get(m["vfs_buf"] + 0x40, size=16)
        seg = o.archive[0] if o.archive else None
        check(seg is not None and seg.path == "Models/test.nsbmd" and seg.addr == m["vfs_buf"] and seg.size == 768
              and seg.member_offset == 0, "origin.get names the archived file the bytes came from (%s)"
              % (((seg.path, hex(seg.addr), seg.size) if seg else None),))
        check(seg is not None and seg.loads and seg.loads[0].file_name == "/vfs.zip"
              and seg.loads[0].archive_member.startswith("Models/test.nsbmd"),
              "the card reads of its bytes are listed with the loading code: %s"
              % ([(hex(l.pc), l.archive_member) for l in seg.loads[:2]] if seg else None))
        labels = [src for t in o.taint for src in t.sources]
        check(labels[:1] == ["/vfs.zip:Models/test.nsbmd"], "data origin labels name the file inside the archive: %s" % labels[:3])
        check(any(f.archive_member.startswith("Models/test.nsbmd") and f.pc for f in o.file_reads),
              "and list the card reads of that file's bytes")
        reads = [x for x in o.chain if x.kind == "card_read"]
        check(any(x.archive_member.startswith("Models/test.nsbmd, stored data") for x in reads) or
              any(x.archive_member.startswith("Models/test.nsbmd") for x in o.file_reads) or bool(seg and seg.loads),
              "card read provenance of vfs.zip names the archived file")
        loc = c.archive_locate(path="models/TEST.NSBMD")
        check(loc.member.index == 0 and len(loc.ram) >= 1 and loc.ram[0].addr == m["vfs_buf"] and loc.ram[0].size == 768,
              "archive.locate finds the file in RAM (%s)" % [(hex(x.addr), x.size) for x in loc.ram])
        check(len(loc.loads) > 0 and all(l.pc for l in loc.loads), "archive.locate lists the card reads of its bytes")
        try:
            c.archive_locate(path="missing.bin")
            check(False, "archive.locate refuses an unknown file")
        except ApiError:
            check(True, "archive.locate refuses an unknown file")

        print("NARC and SDAT containers")
        cl = c.container_list(limit=1000)
        for _ in range(50):
            if cl.status != "indexing":
                break
            time.sleep(0.1)
            cl = c.container_list(limit=1000)
        conts = {k.path: k for k in cl.containers}
        check(cl.status == "ready" and sorted(conts) == ["/gfx.carc", "/gfx.narc", "/sound.sdat"],
              "the NARC, the packed NARC and the SDAT were found: %s" % cl.summary)
        want = ["/gfx.narc:model.nsbmd", "/gfx.narc:sub.narc", "/gfx.narc:sub.narc/0000.bin", "/gfx.narc:sub.narc/0001.narc",
                "/gfx.narc:sub.narc/0001.narc/0000.ncer", "/gfx.narc:bg/title.ncgr", "/gfx.narc:bg/title.nclr",
                "/gfx.carc:ui/icon.ncgr", "/gfx.carc:ui/font.bin",
                "/sound.sdat:SEQ_TITLE.sseq", "/sound.sdat:SEQ_RACE.sseq", "/sound.sdat:WAVE_SE.swar"]
        got = sorted(x.path for x in cl.members)
        check(got == sorted(want), "container.list names every file, nested ones and SDAT symbols included: %s" % got)
        byname = {x.path: x for x in cl.members}
        if got == sorted(want):
            mm, deepf, icon = byname["/gfx.narc:model.nsbmd"], byname["/gfx.narc:sub.narc/0001.narc/0000.ncer"], byname["/gfx.carc:ui/icon.ncgr"]
            check(mm.form == "lz10" and mm.kind == "BMD0" and mm.size == 600 and mm.rom_offset != 0xFFFFFFFF,
                  "a file packed on its own is listed unpacked, with its kind and ROM offset")
            check(deepf.depth == 2 and deepf.kind == "NCER" and deepf.rom_offset == 0xFFFFFFFF and deepf.nested_in == "sub.narc/0001.narc",
                  "a file two levels down, inside a packed NARC, has no ROM offset of its own")
            check(conts["/gfx.carc"].packed == "lz10" and icon.rom_offset == 0xFFFFFFFF and conts["/gfx.narc"].nested_count == 3,
                  "a NARC packed whole is read through its unpacked form")
            check(byname["/sound.sdat:SEQ_RACE.sseq"].kind == "SSEQ" and byname["/sound.sdat:WAVE_SE.swar"].kind == "SWAR",
                  "SDAT files carry their symbol names and kinds")
        check(c.container_list(filter=".NCGR").matched == 2 and c.container_list(container="/sound.sdat").member_count == 3,
              "container.list filters by name and by container")
        check(not (m["status"] & 0x1C0) and (m["narc_title_size"], m["narc_model_size"], m["sseq_size"]) == (512, 600, 240)
              and m["deep_size"] > 256 and m["carc_size"] > 700,
              "test ROM read the container files: status %#x, sizes %s"
              % (m["status"], [m[k] for k in ("narc_title_size", "narc_model_size", "nested_size", "deep_size", "carc_size", "sseq_size")]))

        def labelled(addr, size=16):
            o = c.origin_get(addr, size=size)
            return o, [s for t in o.taint for s in t.sources]
        o, l = labelled(m["narc_title_buf"] + 0x40)
        check(l[:1] == ["/gfx.narc:bg/title.ncgr"], "a file read out of a NARC through the card is labelled as itself: %s" % l[:3])
        seg = o.containers[0] if o.containers else None
        check(seg is not None and seg.path == "/gfx.narc:bg/title.ncgr" and seg.addr == m["narc_title_buf"] and seg.size == 512
              and seg.loads and seg.loads[0].archive_member.startswith("bg/title.ncgr, stored data"),
              "origin.get finds it by content, with the card read of its bytes: %s"
              % (((seg.path, hex(seg.addr), seg.size, [x.archive_member for x in seg.loads[:1]]) if seg else None),))
        check(any(f.archive_member.startswith("bg/title.ncgr") and f.pc for f in o.file_reads), "and lists the loader of that file's bytes")
        check(not o.archive, "the bytes are not taken for the game archive's")
        o, l = labelled(m["narc_model_buf"] + 0x80)
        check(l[:1] == ["/gfx.narc:model.nsbmd"], "a file packed inside a NARC keeps its name once the BIOS unpacks it: %s" % l[:3])
        o, l = labelled(m["nested_buf"] + 72 + 0x20)
        check(l[:1] == ["/gfx.narc:sub.narc/0000.bin"], "a nested NARC read whole labels its files as the files inside: %s" % l[:3])
        o, l = labelled(m["deep_buf"] + 64 + 0x20)
        check(l[:1] == ["/gfx.narc:sub.narc/0001.narc/0000.ncer"],
              "a packed NARC inside it, unpacked by the BIOS, labels the file two levels down: %s" % l[:3])
        check(o.containers and o.containers[0].path == "/gfx.narc:sub.narc/0001.narc/0000.ncer",
              "and origin.get names that file by content: %s" % [x.path for x in o.containers[:2]])
        o, l = labelled(m["carc_buf"] + 0x100)
        names = [x.path for x in o.containers]
        check(l[:1] in (["/gfx.carc:ui/icon.ncgr"], ["/gfx.carc:ui/font.bin"]) and l[:1] == names[:1],
              "a NARC packed whole and unpacked by the BIOS labels its files as themselves: %s, %s" % (l[:2], names[:1]))
        o, l = labelled(m["sseq_buf"] + 0x20)
        check(l[:1] == ["/sound.sdat:SEQ_RACE.sseq"], "a file read out of an SDAT is labelled with its symbol name: %s" % l[:3])
        loc = c.container_locate(path="GFX.NARC:BG/title.ncgr")
        check(loc.member.path == "/gfx.narc:bg/title.ncgr" and loc.ram and loc.ram[0].addr == m["narc_title_buf"]
              and loc.ram[0].size == 512 and loc.loads and all(x.pc for x in loc.loads),
              "container.locate finds the file in RAM and the card reads of its bytes: %s" % [(hex(x.addr), x.size) for x in loc.ram])
        loc = c.container_locate(path="/gfx.narc:sub.narc/0001.narc/0000.ncer")
        check(loc.ram and loc.ram[0].addr == m["deep_buf"] + 64 and loc.ram[0].size == 256,
              "container.locate finds a file two levels down: %s" % [(hex(x.addr), x.size) for x in loc.ram])
        try:
            c.container_locate(path="/gfx.narc:missing.bin")
            check(False, "container.locate refuses an unknown file")
        except ApiError:
            check(True, "container.locate refuses an unknown file")

        print("session file")
        saved_exec = c.metadata_get(m["arm_func"], size=4, group="byte").entries[0].exec
        r = c.recording_configure(enabled=False)
        plain = os.path.basename(r.session_file)       # with write history, no history file (it fits)
        check(not r.recording.enabled and r.session_file.endswith(".dswbs"), "recording off saves a session file")
        if r.session_file and os.path.exists(r.session_file):
            with open(r.session_file, "rb") as f:
                head = f.read(8)
            check(head == sessionmod.ZMAGIC, "session file is compressed (%d KB)" % (os.path.getsize(r.session_file) // 1024))
            s = sessionmod.load(r.session_file)
            check(len(list(s.pages())) > 0 and len(s.call_edges()) > 0, "session file has byte records and call edges")
            check(len(s.write_history()) > 0 and len(s.provenance()) > 0, "session file has write history and provenance")
            fr = {(e.cpu, e.pc): e.func for e in s.first_run()}
            check(fr.get((0, m["arm_func"])) == m["arm_func"] and fr.get((0, m["arm_func"] + 4)) == m["arm_func"],
                  "session file records the function each instruction first ran in (%d entries)" % len(fr))
            check(any(src["name"].endswith("packed.lz") for src in s.sources()), "session file lists the data origin sources")
            lm = s.label_merges()
            check(len(lm) > 0 and all(e.label > max(e.a, e.b) for e in lm),
                  "session file records how its merged labels were made (%d merges)" % len(lm))
            kb = os.path.join(os.path.dirname(os.path.dirname(r.session_file)), "knowledge.db")
            rows = []
            for _ in range(50):
                try:
                    db = sqlite3.connect(kb, timeout=5)
                    rows = db.execute("SELECT l.kind, a.path, a.format FROM asset a JOIN asset_link l ON l.asset_id = a.id "
                                      "WHERE a.member = 'Models/test.nsbmd' AND l.session_id = (SELECT max(id) FROM session)").fetchall()
                    members = db.execute("SELECT count(*) FROM asset WHERE member <> '' AND path LIKE '/vfs.zip/%'").fetchone()[0]
                    contained = db.execute("SELECT a.path, l.kind FROM asset a JOIN asset_link l ON l.asset_id = a.id "
                                           "WHERE a.path LIKE '/gfx.%' AND l.session_id = (SELECT max(id) FROM session)").fetchall()
                    contained_rows = db.execute("SELECT count(*) FROM asset WHERE member <> '' AND path NOT LIKE '/vfs.zip/%'").fetchone()[0]
                    builds = db.execute("SELECT b.score, b.matched, a.kind FROM build_result b JOIN author a ON a.id = b.author_id "
                                        "ORDER BY b.id").fetchall()
                    srcs = db.execute("SELECT source_file, signature FROM function WHERE name = 'Agent_ArmSum'").fetchall()
                    modes = db.execute("SELECT f.mode, a.kind FROM function f JOIN module mo ON mo.id = f.module_id "
                                       "JOIN author a ON a.id = f.author_id WHERE mo.load_address + f.offset = ?",
                                       (m["thumb_func"],)).fetchall()
                    db.close()
                except sqlite3.Error:
                    rows, members, modes, builds, srcs, contained, contained_rows = [], 0, [], [], [], [], 0
                if {x[0] for x in rows} >= {"archive_read", "content"}:
                    break
                time.sleep(0.2)
            check(builds == [(100.0, 1, "agent"), (61.5, 0, "agent")], "the agent's build results are stored with their author: %s" % builds)
            check(srcs == [(src, "int Agent_ArmSum(void)")], "the function keeps the agent's source file and signature")
            check(modes == [("thumb", "agent")], "the agent's auto-mode function at the Thumb entry is stored as thumb: %s" % modes)
            check(members == 4 and {x[0] for x in rows} >= {"archive_read", "content"} and rows[0][1] == "/vfs.zip/Models/test.nsbmd"
                  and rows[0][2] == "nsbmd", "the knowledge store has the archived files as assets, linked to card reads and RAM: %s"
                  % sorted({x[0] for x in rows}))
            check(contained_rows == 12 and ("/gfx.narc/bg/title.ncgr", "archive_read") in contained
                  and ("/gfx.narc/bg/title.ncgr", "content") in contained and ("/gfx.carc/ui/icon.ncgr", "content") in contained,
                  "files inside containers are assets too, linked to card reads and RAM: %d rows, %s"
                  % (contained_rows, sorted(set(contained))[:6]))
        st = c.status_get()
        check(not st.recording.enabled, "status reports recording off")

        print("all-time scope")
        c.recording_configure(enabled=True)
        c.execution_control("run_frames", count=10)
        now = c.metadata_get(m["arm_func"], size=4, group="byte").entries[0].exec
        merged = 0
        for _ in range(50):     # saved sessions merge in the background
            merged = c.metadata_get(m["arm_func"], size=4, group="byte", scope="all").entries[0].exec
            if merged >= now + saved_exec:
                break
            time.sleep(0.2)
        check(0 < now < saved_exec and merged >= now + saved_exec,
              "all-time scope adds saved sessions to this one (exec %d now, %d saved, %d all time)" % (now, saved_exec, merged))
        c.recording_configure(enabled=False)

        print("saved sessions' write history")
        # a 1 MB history file fills and wraps; ending the session keeps it next to the session file
        c.recording_configure(history_file_mb=1, history_keep_mb=2048, history_bulk_threshold=0, history_skip_unchanged=False)
        c.recording_configure(enabled=True, write_history=True)
        c.execution_control("run_frames", count=300)
        rs = c.recording_get().recording
        live = c.writes_list(m["write_target"], size=4, limit=100000)
        st = c.status_get()
        md = c.metadata_get(m["write_target"], size=1, group="byte").entries[0]
        mall = c.metadata_get(m["write_target"], size=1, group="byte", scope="all").entries[0]
        fx = c.metadata_get(m["arm_func"], size=1, group="byte").entries[0]
        check(md.first_seen and "write" in md.first_kinds and rs.session_frame_start <= md.first_write <= st.frame and
              md.first_frame == min(getattr(md, "first_" + k) for k in md.first_kinds) and not mall.first_seen and
              "exec" in fx.first_kinds and rs.session_frame_start <= fx.first_exec <= st.frame,
              "metadata.get gives each byte's first frame per kind this session (written first in %d, arm_func run first in %d)"
              % (md.first_write, fx.first_exec))
        # saving without stopping copies the history file's used part
        sv = c.recording_configure(save_session=True)
        snap = sessionmod.load_history(sv.session_file) if sv.session_file else None
        check(sv.recording.enabled and snap is not None and len(snap) == rs.history_entries and snap.wrapped,
              "saving without stopping keeps a copy of the history so far (%s writes)" % (len(snap) if snap else None))
        r = c.recording_configure(enabled=False)
        hist = r.session_file[:-6] + ".dswbh"
        ff = sessionmod.load(r.session_file).first_frames(sessionmod.Region.MainRAM, m["write_target"] - 0x02000000)
        check(ff is not None and ff[2] == md.first_write, "the session file keeps each byte's first frames (%s)" % (ff,))
        check(r.session_file != sv.session_file and r.recording.history_file_mb == 1,
              "the ended session gets its own name, and a fresh 1 MB history file is ready for the next one")
        sl = c.sessions_list()
        top = sl.sessions[0] if sl.sessions else None
        check(top is not None and top.file == os.path.basename(r.session_file) and os.path.exists(hist) and top.history_file and
              top.history_entries == rs.history_entries and top.history_wrapped == rs.history_wrapped and rs.history_wrapped,
              "the ended session keeps all of its write history in a history file (%s entries, wrapped)" % (top.history_entries if top else None))
        w = c.writes_list(m["write_target"], size=4, limit=100000, session=top.file if top else "")
        key = lambda x: [(e.frame, e.pc, e.addr, e.new_value) for e in x.entries]
        check(w.history_source == "history_file" and w.total == live.total and key(w) == key(live) and w.total > 0 and
              w.history_wrapped and w.oldest_frame == live.oldest_frame,
              "writes.list searches the saved session's history file and finds what the live search found (%d writes)" % w.total)
        h = sessionmod.load_history(r.session_file)
        frames = [e.frame for e in h]
        check(len(h) == rs.history_entries and h.wrapped and frames == sorted(frames) and frames[0] == rs.history_oldest_frame,
              "dswb.session.load_history reads it offline, oldest first")
        old = c.writes_list(0, size=0, limit=5, session=plain)
        check(old.history_source == "session_file" and old.total > 0,
              "a session saved without a history file is searched in its session file (%d writes)" % old.total)
        try:
            c.writes_list(0, size=0, session="no-such-session.dswbs")
            check(False, "writes.list refuses an unknown session")
        except ApiError:
            check(True, "writes.list refuses an unknown session")
        # the disk budget: the oldest history files go, never the newest
        c.recording_configure(history_keep_mb=1)
        c.recording_configure(enabled=True, write_history=True)
        c.execution_control("run_frames", count=60)
        r = c.recording_configure(enabled=False)
        check(r.recording.history_keep_mb == 1 and os.path.exists(r.session_file[:-6] + ".dswbh") and not os.path.exists(hist),
              "over the 1 MB budget the older history file is deleted and the new one kept")
        c.recording_configure(history_file_mb=0, history_keep_mb=2048, history_bulk_threshold=1024, history_skip_unchanged=True)

        print("a person pausing an agent's input; long source lists")
        c.execution_control("resume")
        time.sleep(0.3)
        sent = {}

        def send_input():
            try:
                sent["r"] = c.input_send(buttons=["a"], frames=600, capture=False)
            except Exception as e:  # noqa: BLE001
                sent["e"] = e
        t = threading.Thread(target=send_input, daemon=True)
        t.start()
        time.sleep(1.0)
        t0 = time.time()
        c.execution_control("pause")                # what the person's pause does
        t.join(10)
        check(not t.is_alive() and "r" in sent and getattr(sent["r"], "interrupted", False),
              "input.send for 600 frames answers when the game is paused, as interrupted (%.1f s after the pause)" % (time.time() - t0))
        if t.is_alive():
            c.execution_control("resume")
            t.join(30)
        c.execution_control("resume")
        time.sleep(0.3)
        t0 = time.time()
        r = c.recording_configure(sources_remove=list(range(1000000, 1002000)))
        took = time.time() - t0
        check(took < 3.0, "recording.configure removing 2000 sources answers in %.1f s" % took)

        print("pointing the window")
        check_ui(c, check, m["arm_func"], m["data_table"])
        c.close()
    finally:
        if not a.keep:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        # after melonDS exits: it saves its configuration on the way out
        open(toml, "w", encoding="utf-8").write(toml_orig)
    print("\n%d passed, %d failed" % (PASSES, len(FAILS)))
    for f in FAILS:
        print("  FAILED:", f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
