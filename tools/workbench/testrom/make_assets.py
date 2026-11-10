"""Builds the deterministic NitroFS files for the workbench test ROM.

payload.bin: 256 bytes, byte i = (i * 7 + 3) & 0xFF
packed.lz:   LZ77 (BIOS type 0x10) of 1024 bytes whose words 2 and 3 are
             0xCAFE0002 and 0xBEEF0003 (copied into the test ROM's car struct)
plain.lz:    LZ77 of a text table that the ROM unpacks in software, every literal through a
             lookup table (a decoder whose taint is lossy)
vfs.zip, vfs.bin, vfs.dat: a small Octane engine VFS (the layout TrackMania DS uses):
             Models/test.nsbmd is stored first, so the ROM can read it without
             inflating; the other files are deflated
gfx.narc:    a NARC with names: model.nsbmd (LZ77-packed on its own), sub.narc (a nested NARC
             without names, holding a raw file and an LZ77-packed NARC of its own), bg/title.ncgr
             and bg/title.nclr
gfx.carc:    a NARC packed whole with LZ77: ui/icon.ncgr and ui/font.bin
sound.sdat:  an SDAT with symbol names: SEQ_TITLE and SEQ_RACE (files 0 and 1), WAVE_SE (file 2)
gfx/:        graphics files with known pixels: t.nclr, t4.ncgr, t8.ncgr, t.nscr, t.ncer, t.nsbtx
"""
import os
import shutil
import struct
import subprocess
import sys
import zipfile

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "nitrofiles")
os.makedirs(out, exist_ok=True)

with open(os.path.join(out, "payload.bin"), "wb") as f:
    f.write(bytes(((i * 7 + 3) & 0xFF) for i in range(256)))

raw = bytearray(1024)
for i in range(256):
    struct.pack_into("<I", raw, i * 4, 0x01010101 * (i & 0xF))
struct.pack_into("<I", raw, 8, 0xCAFE0002)
struct.pack_into("<I", raw, 12, 0xBEEF0003)
struct.pack_into("<I", raw, 20, 0x02345678)      # data that looks like a main RAM address
raw_path = os.path.join(here, "build_unpacked.bin")
os.makedirs(os.path.dirname(raw_path), exist_ok=True)
with open(raw_path, "wb") as f:
    f.write(raw)

def find_gbalzss():
    found = shutil.which("gbalzss")
    if found:
        return found
    for root in (os.environ.get("DEVKITPRO", ""), "C:/devkitPro"):
        if root.startswith("/opt/devkitpro"):
            root = "C:/devkitPro"
        for name in ("gbalzss.exe", "gbalzss"):
            p = os.path.join(root, "tools", "bin", name)
            if os.path.isfile(p):
                return p
    sys.exit("gbalzss not found (install devkitPro's general-tools)")


def write_vfs(files):
    """vfs.zip plus the index the Octane engine uses: vfs.bin (a linked tree of lower-case names whose
    file records hold the ZIP index, then each file's central directory offset) and vfs.dat."""
    zpath = os.path.join(out, "vfs.zip")
    with zipfile.ZipFile(zpath, "w") as z:
        for name, data, deflate in files:
            zi = zipfile.ZipInfo(name, date_time=(2008, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED if deflate else zipfile.ZIP_STORED
            z.writestr(zi, data)
    zdata = open(zpath, "rb").read()
    eocd = zdata.rfind(b"PK\x05\x06")
    cd_start = struct.unpack_from("<I", zdata, eocd + 16)[0]
    cd_offsets, p = [], cd_start
    for _ in files:
        cd_offsets.append(p - cd_start)
        nlen, xlen, clen = struct.unpack_from("<HHH", zdata, p + 28)
        p += 46 + nlen + xlen + clen

    # tree: {name: subtree} for directories, {name: file index} for files, in ZIP order
    root = {}
    for i, (name, _, _) in enumerate(files):
        node = root
        parts = name.lower().split("/")
        for d in parts[:-1]:
            node = node.setdefault(d, {})
        node[parts[-1]] = i
    names, name_off, entries = b"", {}, []

    def name_ref(n):
        nonlocal names
        if n not in name_off:
            name_off[n] = len(names)
            names += n.encode() + b"\0"
        return name_off[n]

    def emit(node):
        prev = None
        for n, v in node.items():
            idx = len(entries)
            if prev is not None:
                entries[prev][1] = idx
            if isinstance(v, dict):
                entries.append([name_ref(n), 0xFFFF, idx + 1, 0x7FFF])
                emit(v)
            else:
                entries.append([name_ref(n), 0xFFFF, 0xFFFF, v])
            prev = idx
    emit(root)
    bin_ = b"\x00\x00\xff\xff\x00\x00\xff\xff" + b"".join(struct.pack("<4H", *e) for e in entries)
    bin_ += b"".join(struct.pack("<I", o) for o in cd_offsets) + names
    open(os.path.join(out, "vfs.bin"), "wb").write(bin_)
    open(os.path.join(out, "vfs.dat"), "wb").write(("%d, %d, %d\r\n" % (len(entries), len(files), len(names))).encode())


model = bytes(((i * 37 + (i >> 3) * 11 + 5) & 0xFF) for i in range(768))
other = bytes(((i * 13) ^ (i >> 4)) & 0xFF for i in range(2000))
text = b"".join(b"line %d of the readme\n" % i for i in range(60))
# Text/decoded.str is "unpacked" by the ROM through a lookup table, the way a Huffman decoder works
decoded = b"".join(b"entry %03d: speed %d grip %d\n" % (i, i * 7 % 101, i * 13 % 37) for i in range(40))
write_vfs([("Models/test.nsbmd", model, False), ("Models/Other.bin", other, True), ("Text/readme.str", text, True),
           ("Text/decoded.str", decoded, False)])

subprocess.check_call([find_gbalzss(), "e", raw_path, os.path.join(out, "packed.lz")])
os.remove(raw_path)

plain = b"".join(b"track %02d: laps %d par %d.%02d\n" % (i, 3 + i % 3, 40 + i * 3 % 29, i * 17 % 100) for i in range(28))
plain_path = os.path.join(here, "build_plain.bin")
with open(plain_path, "wb") as f:
    f.write(plain)
subprocess.check_call([find_gbalzss(), "e", plain_path, os.path.join(out, "plain.lz")])
os.remove(plain_path)


# ---- NARC and SDAT containers

def varied(seed, n, magic=b""):
    """n bytes no other test file shares, starting with a format's magic"""
    v = bytearray()
    for _ in range(n):
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        v.append((seed >> 16) & 0xFF)
    v[:len(magic)] = magic
    return bytes(v)


def lz10_literals(d):
    """LZ77 (BIOS type 0x10) holding only literals: valid for the BIOS, and no tool needed"""
    o = bytearray(struct.pack("<I", 0x10 | (len(d) << 8)))
    for i in range(0, len(d), 8):
        o.append(0)
        o += d[i:i + 8]
    while len(o) & 3:
        o.append(0)
    return bytes(o)


def pad4(b):
    return b + b"\0" * (-len(b) & 3)


def narc(files, names=True):
    """files in file id order, [(name, data)]; "dir/x" puts x in a directory (one level; the root's
    files first, then each directory's). names=False: a name table with the root only"""
    if not names:
        fnt = struct.pack("<IHH", 8, 0, 1) + b"\0"
    else:
        dirs = []
        for n, _ in files:
            if "/" in n and n.split("/")[0] not in dirs:
                dirs.append(n.split("/")[0])
        subs, first, fid = [], [], 0
        for d in [None] + dirs:
            first.append(fid)
            s = b""
            for n, _ in files:
                if (d is None and "/" not in n) or (d is not None and n.startswith(d + "/")):
                    leaf = n if d is None else n[len(d) + 1:]
                    s += bytes([len(leaf)]) + leaf.encode()
                    fid += 1
            if d is None:
                for k, dn in enumerate(dirs):
                    s += bytes([0x80 | len(dn)]) + dn.encode() + struct.pack("<H", 0xF001 + k)
            subs.append(s + b"\0")
        at = (len(dirs) + 1) * 8
        fnt = b""
        for k, s in enumerate(subs):
            fnt += struct.pack("<IHH", at, first[k], len(dirs) + 1 if k == 0 else 0xF000)
            at += len(s)
        fnt += b"".join(subs)
    fnt = pad4(fnt)
    data, btaf = b"", b""
    for _, d in files:
        btaf += struct.pack("<II", len(data), len(data) + len(d))
        data = pad4(data + d)
    body = (b"BTAF" + struct.pack("<IHH", 12 + len(btaf), len(files), 0) + btaf +
            b"BTNF" + struct.pack("<I", 8 + len(fnt)) + fnt +
            b"GMIF" + struct.pack("<I", 8 + len(data)) + data)
    return b"NARC\xfe\xff\x00\x01" + struct.pack("<IHH", 16 + len(body), 16, 3) + body


def sdat(seqs, waves):
    """[(symbol name, data)] for sequences and wave archives: the sequences are files 0.., the wave
    archives the files after them"""
    hdr_len = 0x40
    # SYMB: 8 record offsets, then the records, then the names
    recs, strs = {}, b""
    symb_body = bytearray(b"SYMB\0\0\0\0" + b"\0" * 32)
    for t, lst in ((0, seqs), (3, waves)):
        struct.pack_into("<I", symb_body, 8 + t * 4, len(symb_body))
        rec_at = len(symb_body)
        symb_body += struct.pack("<I", len(lst)) + b"\0" * (4 * len(lst))
        recs[t] = rec_at
    name_base = len(symb_body)
    for t, lst in ((0, seqs), (3, waves)):
        for i, (name, _) in enumerate(lst):
            struct.pack_into("<I", symb_body, recs[t] + 4 + i * 4, name_base + len(strs))
            strs += name.encode() + b"\0"
    symb_body += strs
    symb_body = bytearray(pad4(bytes(symb_body)))
    struct.pack_into("<I", symb_body, 4, len(symb_body))
    # INFO: a record per type; sequences and wave archives name their files
    info = bytearray(b"INFO\0\0\0\0" + b"\0" * 32)
    for t in range(8):
        struct.pack_into("<I", info, 8 + t * 4, len(info))
        lst = seqs if t == 0 else waves if t == 3 else []
        at = len(info)
        info += struct.pack("<I", len(lst)) + b"\0" * (4 * len(lst))
        for i in range(len(lst)):
            struct.pack_into("<I", info, at + 4 + i * 4, len(info))
            info += struct.pack("<HHI", i if t == 0 else len(seqs) + i, 0, 0)
    struct.pack_into("<I", info, 4, len(info))
    files = [d for _, d in seqs] + [d for _, d in waves]
    symb_off = hdr_len
    info_off = symb_off + len(symb_body)
    fat_off = info_off + len(info)
    fat_len = 12 + 16 * len(files)
    file_off = fat_off + fat_len
    fat = b"FAT " + struct.pack("<II", fat_len, len(files))
    fblock = b"FILE" + b"\0\0\0\0" + struct.pack("<II", len(files), 0)
    for d in files:
        fat += struct.pack("<IIII", file_off + len(fblock), len(d), 0, 0)
        fblock = pad4(fblock + d)
    fblock = fblock[:4] + struct.pack("<I", len(fblock)) + fblock[8:]
    total = file_off + len(fblock)
    hdr = b"SDAT\xff\xfe\x00\x01" + struct.pack("<IHH", total, hdr_len, 4)
    hdr += struct.pack("<8I", symb_off, len(symb_body), info_off, len(info), fat_off, fat_len, file_off, len(fblock))
    hdr += b"\0" * (hdr_len - len(hdr))
    return hdr + bytes(symb_body) + bytes(info) + fat + fblock


# gfx.narc: names in a directory, a file packed on its own, and a nested NARC (stored, without names)
# that holds a packed NARC: three levels
deep = narc([("", varied(41, 256, b"RECN"))], names=False)
sub = narc([("", varied(42, 300)), ("", lz10_literals(deep))], names=False)
open(os.path.join(out, "gfx.narc"), "wb").write(narc([
    ("model.nsbmd", lz10_literals(varied(43, 600, b"BMD0"))),
    ("sub.narc", sub),
    ("bg/title.ncgr", varied(44, 512, b"RGCN")),
    ("bg/title.nclr", varied(45, 200, b"RLCN"))]))

# gfx.carc: a NARC packed whole
carc_path = os.path.join(here, "build_carc.bin")
open(carc_path, "wb").write(narc([("ui/icon.ncgr", varied(46, 400, b"RGCN")), ("ui/font.bin", varied(47, 300))]))
subprocess.check_call([find_gbalzss(), "e", carc_path, os.path.join(out, "gfx.carc")])
os.remove(carc_path)

# sound.sdat: two sequences and a wave archive, with symbol names
open(os.path.join(out, "sound.sdat"), "wb").write(sdat(
    [("SEQ_TITLE", varied(48, 180, b"SSEQ")), ("SEQ_RACE", varied(49, 240, b"SSEQ"))],
    [("WAVE_SE", varied(50, 500, b"SWAR"))]))
print("assets written to", out)


# ---------------------------------------------------------------- graphics files with known pixels
# gfx/: an NCLR, NCGR at 4 and 8 bpp, an NSCR with flips, an NCER, and an NSBTX with one texture of
# each TEX0 format. Every pixel follows a formula the tests compute too (keep in step with TestGfx in
# wbtests.cpp and tests/test_assets.py).

def pal_color(i):
    return (i & 31) | (((31 - i) & 31) << 5) | (((i * 3) & 31) << 10)


def tile_pixel(bpp, x, y):
    return (x + 3 * y) & 15 if bpp == 4 else (7 * x + y) & 255


def map_entry(k):
    return (k % 8) | ((k & 1) << 10) | (((k >> 1) & 1) << 11) | ((k % 2) << 12)


def file2d(magic, sections):
    body = b""
    for m, b in sections:
        body += m + struct.pack("<I", len(b) + 8) + b
    return magic + struct.pack("<HHIHH", 0xFEFF, 0x0100, 16 + len(body), 0x10, len(sections)) + body


def nclr():
    b = struct.pack("<IIII", 3, 0, 64, 0x10) + b"".join(struct.pack("<H", pal_color(i)) for i in range(32))
    return file2d(b"RLCN", [(b"TTLP", b)])


def ncgr(bpp):
    data = bytearray()
    for t in range(8):
        for py in range(8):
            for px in range(0, 8, 8 // bpp):
                x, y = (t % 4) * 8 + px, (t // 4) * 8 + py
                data.append(tile_pixel(4, x, y) | (tile_pixel(4, x + 1, y) << 4) if bpp == 4 else tile_pixel(8, x, y))
    b = struct.pack("<HHIIIII", 2, 4, 3 if bpp == 4 else 4, 0, 0, len(data), 0x18) + bytes(data)
    return file2d(b"RGCN", [(b"RAHC", b)])


def nscr():
    b = struct.pack("<HHHHI", 32, 16, 0, 0, 16) + b"".join(struct.pack("<H", map_entry(k)) for k in range(8))
    return file2d(b"RCSN", [(b"NRCS", b)])


def ncer():
    b = struct.pack("<HHIIIII", 1, 0, 0x18, 1, 0, 0, 0)
    b += struct.pack("<HHI", 2, 0, 0)
    b += struct.pack("<HHH", 0xF8, 0x4000 | 0x1F8, 0)               # 16x16 at (-8, -8), tile 0
    b += struct.pack("<HHH", 0x00, 0x1000 | 0x008, 0x1004)          # 8x8 at (8, 0), H-flip, tile 4, palette 1
    return file2d(b"RECN", [(b"KBEC", b)])


TEX_NAMES = ["a3i5", "c4", "c16", "c256", "cmp4x4", "a5i3", "direct"]


def tex_index(fmt, x, y):
    v = x + 8 * y
    return {1: (v & 31) | (((v >> 3) & 7) << 5), 2: v & 3, 3: v & 15, 4: v & 255,
            6: (v & 7) | (((v >> 1) & 31) << 3), 7: pal_color(v) | ((v & 1) << 15)}.get(fmt, 0)


def align8(b):
    return b + b"\0" * (-len(b) % 8)


def tex_dict(names, entries, esize):
    n = len(names)
    d = struct.pack("<BBH", 0, n, 0) + struct.pack("<HHI", 8, 12 + 4 * n, 0x17F) + b"\0" * (4 * n)
    d += struct.pack("<HH", esize, 4 + esize * n) + b"".join(entries)
    d += b"".join(nm.encode().ljust(16, b"\0") for nm in names)
    return d[:2] + struct.pack("<H", len(d)) + d[4:]


def nsbtx():
    tex, cmp, idx, pal = b"", b"", b"", b""
    tex_entries, pal_entries, pal_names = [], [], []
    for f in range(1, 8):
        if f == 5:
            param = len(cmp) >> 3
            for k in range(4):
                w = 0
                for i in range(16):
                    w |= (((i % 4) + 4 * (i // 4) + k) & 3) << (i * 2)
                cmp += struct.pack("<I", w)
                idx += struct.pack("<H", (k * 2) | (k << 14))
        else:
            param = len(tex) >> 3
            bits = {1: 8, 2: 2, 3: 4, 4: 8, 6: 8, 7: 16}[f]
            acc = have = 0
            out_b = bytearray()
            for y in range(8):
                for x in range(8):
                    acc |= tex_index(f, x, y) << have
                    have += bits
                    while have >= 8:
                        out_b.append(acc & 0xFF)
                        acc >>= 8
                        have -= 8
            tex = align8(tex + bytes(out_b))
        param |= (f << 26) | ((1 << 29) if f == 3 else 0)
        tex_entries.append(struct.pack("<II", param, 0x00800008))
        if f != 7:
            colors = {1: 32, 2: 4, 3: 16, 4: 256, 5: 16, 6: 8}[f]
            pal_entries.append(struct.pack("<HH", len(pal) >> 3, 0))
            pal_names.append(TEX_NAMES[f - 1] + "_pl")
            pal = align8(pal + b"".join(struct.pack("<H", pal_color(i + f)) for i in range(colors)))
    td = tex_dict(TEX_NAMES, tex_entries, 8)
    pd = tex_dict(pal_names, pal_entries, 4)
    t0 = bytearray(b"TEX0" + b"\0" * (0x3C - 4))
    offs = []
    for block in (td, pd, tex, cmp, idx, pal):
        offs.append(len(t0))
        t0 += block
        t0 += b"\0" * (-len(t0) % 8)     # each block ends 8-aligned within TEX0
    tex_dict_off, pal_dict_off, tex_off, cmp_off, idx_off, pal_off = offs
    struct.pack_into("<I", t0, 4, len(t0))
    struct.pack_into("<HH", t0, 0x0C, len(tex) >> 3, tex_dict_off)
    struct.pack_into("<I", t0, 0x14, tex_off)
    struct.pack_into("<HH", t0, 0x1C, len(cmp) >> 3, tex_dict_off)
    struct.pack_into("<II", t0, 0x24, cmp_off, idx_off)
    struct.pack_into("<III", t0, 0x30, len(pal) >> 3, pal_dict_off, pal_off)
    hdr = b"BTX0" + struct.pack("<HHIHHI", 0xFEFF, 1, 0x14 + len(t0), 0x10, 1, 0x14)
    return hdr + bytes(t0)


gfx_dir = os.path.join(out, "gfx")
os.makedirs(gfx_dir, exist_ok=True)
for name, data in (("t.nclr", nclr()), ("t4.ncgr", ncgr(4)), ("t8.ncgr", ncgr(8)), ("t.nscr", nscr()),
                   ("t.ncer", ncer()), ("t.nsbtx", nsbtx())):
    open(os.path.join(gfx_dir, name), "wb").write(data)
print("graphics written to", gfx_dir)
