"""Reader for workbench session files (.dswbs).

Layouts come from the generated schema module, so this stays in step with the
C++ writer in src/workbench/Session.cpp.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Optional, Tuple

from ._generated import (ByteRecord, CallEdge, FirstRunEntry, LabelMergeEntry, OverlayEvent, PageHeader, ProvenanceEvent, RecEvent,
                         SessionHeader, SessionSection, SessionSectionHeader, TcmEvent, WriteHistoryEntry,
                         Region, SCHEMA_VERSION)

PAGE_SIZE = 4096
ZMAGIC = b"DSWBSZ1\0"


def region_name(r: int) -> str:
    if Region.OverlayARM9Base <= r < Region.OverlayARM7Base:
        return f"ov9_{r - Region.OverlayARM9Base}"
    if Region.OverlayARM7Base <= r < Region.RegionMax:
        return f"ov7_{r - Region.OverlayARM7Base}"
    try:
        return Region(r).name
    except ValueError:
        return f"region{r}"


@dataclass
class Session:
    header: SessionHeader
    sections: Dict[int, Tuple[SessionSectionHeader, memoryview]] = field(default_factory=dict)

    @property
    def rom_sha1(self) -> str:
        return self.header.rom_sha1.hex()

    def section(self, sid: SessionSection) -> Optional[Tuple[SessionSectionHeader, memoryview]]:
        return self.sections.get(int(sid))

    def pages(self) -> Iterator[Tuple[int, int, memoryview]]:
        """Yield (region, page, raw 4096*32 bytes of ByteRecords)."""
        s = self.section(SessionSection.ByteRecords)
        if not s:
            return
        hdr, data = s
        stride = PageHeader.SIZE + ByteRecord.SIZE * PAGE_SIZE
        for i in range(hdr.count):
            ph = PageHeader.unpack_from(data, i * stride)
            yield ph.region, ph.page, data[i * stride + PageHeader.SIZE:(i + 1) * stride]

    def byte_record(self, region: int, offset: int) -> Optional[ByteRecord]:
        for r, p, raw in self.pages():
            if r == region and p == offset >> 12:
                return ByteRecord.unpack_from(raw, (offset & 0xFFF) * ByteRecord.SIZE)
        return None

    def first_frames(self, region: int, offset: int) -> Optional[Tuple[Optional[int], Optional[int], Optional[int], Optional[int]]]:
        """The first frame a byte was executed, read, written and DMA'd in this session (None: never)."""
        s = self.section(SessionSection.FirstFrames)
        if not s:
            return None
        hdr, data = s
        stride = PageHeader.SIZE + 16 * PAGE_SIZE
        for i in range(hdr.count):
            ph = PageHeader.unpack_from(data, i * stride)
            if ph.region == region and ph.page == offset >> 12:
                f = struct.unpack_from("<4I", data, i * stride + PageHeader.SIZE + (offset & 0xFFF) * 16)
                return tuple(x - 1 if x else None for x in f)
        return None

    def _records(self, sid, cls):
        s = self.section(sid)
        if not s:
            return []
        hdr, data = s
        return [cls.unpack_from(data, i * cls.SIZE) for i in range(hdr.count)]

    def write_history(self) -> List[WriteHistoryEntry]:
        return self._records(SessionSection.WriteHistory, WriteHistoryEntry)

    def first_run(self) -> List[FirstRunEntry]:
        """The function on the call stack when each instruction first ran."""
        return self._records(SessionSection.FirstRun, FirstRunEntry)

    def label_merges(self) -> List[LabelMergeEntry]:
        """How each data origin label naming several sources was first made (label, the two it joined, pc, frame, op)."""
        return self._records(SessionSection.LabelMerges, LabelMergeEntry)

    def call_edges(self) -> List[CallEdge]:
        return self._records(SessionSection.CallEdges, CallEdge)

    def provenance(self) -> List[ProvenanceEvent]:
        return self._records(SessionSection.Provenance, ProvenanceEvent)

    def overlay_events(self) -> List[OverlayEvent]:
        return self._records(SessionSection.OverlayEvents, OverlayEvent)

    def tcm_events(self) -> List[TcmEvent]:
        return self._records(SessionSection.TcmEvents, TcmEvent)

    def rec_events(self) -> List[RecEvent]:
        return self._records(SessionSection.RecEvents, RecEvent)

    def sources(self) -> List[dict]:
        s = self.section(SessionSection.Sources)
        out = []
        if not s:
            return out
        hdr, data = s
        pos = 0
        for _ in range(hdr.count):
            sid, kind, enabled, cpu, reg, file_id, addr, size, n = struct.unpack_from("<IBBBBIIII", data, pos)
            pos += 24
            name = bytes(data[pos:pos + n]).decode("utf-8", "replace")
            pos += n
            out.append(dict(id=sid, kind=kind, enabled=bool(enabled), cpu=cpu, reg=reg, file_id=file_id, addr=addr, size=size, name=name))
        return out

    def labels(self) -> List[List[int]]:
        s = self.section(SessionSection.Labels)
        out = []
        if not s:
            return out
        hdr, data = s
        pos = 0
        for _ in range(hdr.count):
            (n,) = struct.unpack_from("<I", data, pos)
            pos += 4
            out.append(list(struct.unpack_from(f"<{n}I", data, pos)))
            pos += 4 * n
        return out


def load(path: str) -> Session:
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:8] == ZMAGIC:       # compressed: magic, u64 uncompressed size, zlib stream
        raw = zlib.decompress(raw[16:])
    data = memoryview(raw)
    hdr = SessionHeader.unpack_from(data, 0)
    if hdr.magic != b"DSWBSESS":
        raise ValueError("not a workbench session file")
    if hdr.version > SCHEMA_VERSION:
        raise ValueError(f"session schema {hdr.version} is newer than this reader ({SCHEMA_VERSION})")
    pos = hdr.header_size or SessionHeader.SIZE
    s = Session(hdr)
    for _ in range(hdr.section_count):
        sh = SessionSectionHeader.unpack_from(data, pos)
        pos += SessionSectionHeader.SIZE
        s.sections[sh.id] = (sh, data[pos:pos + sh.size])
        pos += sh.size
    return s


HMAGIC = b"DSWBH1\0\0"
_HTRAILER = struct.Struct("<8s3Q4I")     # magic, pos, used, slots, frame start, frame end, entry size, reserved


@dataclass
class History:
    """A session's whole write history (<session>.dswbh), saved when the session file can't hold it all."""
    pos: int
    used: int
    slots: int
    frame_start: int
    frame_end: int
    data: memoryview

    @property
    def wrapped(self) -> bool:
        """The oldest writes were dropped before the session ended."""
        return self.pos > self.used

    def __len__(self) -> int:
        return self.used

    def __iter__(self) -> Iterator[WriteHistoryEntry]:
        """Oldest first."""
        start = (self.pos - self.used) % self.slots if self.slots else 0
        for i in range(self.used):
            yield WriteHistoryEntry.unpack_from(self.data, ((start + i) % self.slots) * WriteHistoryEntry.SIZE)


def load_history(path: str) -> History:
    """Read a .dswbh file (pass the .dswbs path or the .dswbh one)."""
    if path.endswith(".dswbs"):
        path = path[:-6] + ".dswbh"
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < _HTRAILER.size:
        raise ValueError("not a workbench history file")
    magic, pos, used, slots, fs, fe, esize, _ = _HTRAILER.unpack_from(raw, len(raw) - _HTRAILER.size)
    if magic != HMAGIC or esize != WriteHistoryEntry.SIZE or used > slots or slots * esize > len(raw) - _HTRAILER.size:
        raise ValueError("not a workbench history file")
    return History(pos, used, slots, fs, fe, memoryview(raw)[:slots * esize])
