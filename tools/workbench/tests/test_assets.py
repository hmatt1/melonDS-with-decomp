"""End-to-end test of the graphics file support (asset.list, asset.describe, asset.render) on the test ROM.

    python tools/workbench/tests/test_assets.py [--melonds build/melonDS.exe] [--rom tools/workbench/testrom/wbtest.nds]

The ROM reads gfx/t.nclr, t4.ncgr, t.nscr, t.ncer and t.nsbtx (make_assets.py: every pixel follows a
formula) into RAM, and copies the tiles and map into VRAM bank A, the colours into BG palette RAM
and the texture file into VRAM bank B. This test checks the files are found in the ROM and in RAM,
where their parts are in VRAM, and that the images drawn from the ROM and from RAM are the same and
have the expected pixels. A damaged file (the NARC's stand-in bg/title.ncgr) is described as far as
it goes.
"""
import argparse
import base64
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb import Client, ApiError  # noqa: E402
from test_recording import find_manifest, read_manifest  # noqa: E402

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


def png_pixels(b64):
    """(width, height, rows of (r, g, b, a)) of an 8-bit RGB or RGBA PNG."""
    data = base64.b64decode(b64)
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w, h, ctype = 8, b"", 0, 0, 6
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6), (depth, ctype)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    bpp = 4 if ctype == 6 else 3
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 0xFF
            elif f == 2: line[i] = (line[i] + b) & 0xFF
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append([tuple(line[x * bpp:x * bpp + bpp]) + ((255,) if bpp == 3 else ()) for x in range(w)])
        prev = line
    return w, h, rows


# the formulas make_assets.py builds the files from
def pal_color(i):
    return (i & 31) | (((31 - i) & 31) << 5) | (((i * 3) & 31) << 10)


def up5(v):
    return (v << 3) | (v >> 2)


def rgb(c, a=255):
    return (up5(c & 31), up5((c >> 5) & 31), up5((c >> 10) & 31), a)


def tile_pixel(x, y):
    return (x + 3 * y) & 15


def texel_index(fmt, x, y):
    v = x + 8 * y
    return {1: (v & 31) | (((v >> 3) & 7) << 5), 2: v & 3, 3: v & 15, 4: v & 255,
            6: (v & 7) | (((v >> 1) & 31) << 3), 7: pal_color(v) | ((v & 1) << 15)}.get(fmt, 0)


def texel_expected(f, x, y):
    palc = lambda i: rgb(pal_color(i + f))
    v = texel_index(f, x, y)
    if f == 1:
        a = v >> 5
        return palc(v & 31)[:3] + (up5(a * 4 + a // 2),)
    if f == 2 or f == 4:
        return palc(v)
    if f == 3:
        return palc(v) if v else (0, 0, 0, 0)
    if f == 6:
        return palc(v & 7)[:3] + (up5(v >> 3),)
    if f == 7:
        return rgb(v & 0x7FFF, 255 if v >> 15 else 0)
    k = (y // 4) * 2 + x // 4
    i = ((x % 4) + 4 * (y % 4) + k) & 3
    c0, c1 = palc(k * 4), palc(k * 4 + 1)
    mix = lambda wa, wb, d: tuple((c0[j] * wa + c1[j] * wb) // d for j in range(3)) + (255,)
    if i < 2:
        return c1 if i else c0
    if k == 0:
        return palc(2) if i == 2 else (0, 0, 0, 0)
    if k == 1:
        return mix(1, 1, 2) if i == 2 else (0, 0, 0, 0)
    if k == 2:
        return palc(k * 4 + i)
    return mix(5, 3, 8) if i == 2 else mix(3, 5, 8)


TEX_NAMES = ["a3i5", "c4", "c16", "c256", "cmp4x4", "a5i3", "direct"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--melonds", default=os.path.join(HERE, "..", "..", "..", "build", "melonDS.exe"))
    ap.add_argument("--rom", default=os.path.join(HERE, "..", "testrom", "wbtest.nds"))
    ap.add_argument("--port", type=int, default=47930)
    a = ap.parse_args()
    exe = os.path.abspath(a.melonds)
    exedir = os.path.dirname(exe)
    info = os.path.join(exedir, "workbench", "api.json")
    if os.path.exists(info):
        os.remove(info)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    kbdir = tempfile.mkdtemp(prefix="dswb_assets_")
    proc = subprocess.Popen([exe, "--api", "--api-port", str(a.port), "--kb", os.path.join(kbdir, "knowledge.db"), os.path.abspath(a.rom)],
                            cwd=exedir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(info):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        c = Client.connect(info)
        m = None
        maddr = None
        for _ in range(30):
            c.execution_control("run_frames", count=30)
            maddr = maddr or find_manifest(c)
            if maddr:
                m = read_manifest(c, maddr)
                if m["ready"]:
                    break
        check(m and m["ready"] and not (m["status"] & 512), "the ROM read its graphics files (status 0x%x)" % (m["status"] if m else -1))
        c.execution_control("pause")

        print("asset.list")
        lst = None
        for _ in range(50):
            lst = c.asset_list()
            if lst.status == "ready":
                break
            time.sleep(0.2)
        files = {f.path: f for f in lst.files}
        want = {"/gfx/t.nclr": "NCLR", "/gfx/t4.ncgr": "NCGR", "/gfx/t8.ncgr": "NCGR", "/gfx/t.nscr": "NSCR",
                "/gfx/t.ncer": "NCER", "/gfx/t.nsbtx": "BTX0"}
        check(all(p in files and files[p].kind == k for p, k in want.items()), "the test files are listed with their kinds: %s" % lst.summary)
        check("/gfx.narc:bg/title.ncgr" in files and files["/gfx.narc:bg/title.ncgr"].source == "container", "files inside containers are listed too")

        print("asset.describe")
        d = c.asset_describe(path="/gfx/t4.ncgr")
        check(d.format == "NCGR" and d.unreadable_from == 0 and d.tiles == 8 and d.bpp == 4 and d.tiles_x == 4, "t4.ncgr: 8 tiles at 4bpp, 4 wide")
        check(any(p.kind == "tiles" and p.offset == 0x30 and p.size == 256 for p in d.parts), "its tile data is at +0x30, 256 bytes")
        check(m["gfx_ncgr_buf"] in d.ram_copies, "its RAM copy is found: %s" % [hex(x) for x in d.ram_copies])
        check(any("tiles in VRAM bank A +0x4000" in v for v in d.video_memory), "its tiles are found in VRAM: %s" % d.video_memory)
        d = c.asset_describe(path="/gfx/t.nclr")
        check(d.palettes == 2 and d.colors_per_palette == 16, "t.nclr: two 16-colour palettes")
        check(any("palette RAM 0x05000000 (BG main, palette 0)" in v for v in d.video_memory), "its colours are found in palette RAM: %s" % d.video_memory)
        d = c.asset_describe(path="/gfx/t.nscr")
        check(d.map_width == 32 and d.map_height == 16 and any("map in VRAM bank A +0x0" in v for v in d.video_memory), "t.nscr: 32x16, in VRAM: %s" % d.video_memory)
        d = c.asset_describe(path="/gfx/t.nsbtx")
        check([t.name for t in d.textures] == TEX_NAMES and all(t.width == 8 and t.height == 8 for t in d.textures), "t.nsbtx: the 7 textures")
        check(d.textures[0].palette == "a3i5_pl" and d.textures[6].palette != "", "textures pair with their same-named palettes")
        check(any("texture 'a3i5' in VRAM bank B" in v for v in d.video_memory), "its textures are found in VRAM bank B: %s" % d.video_memory[:3])
        d = c.asset_describe(addr=m["gfx_ncgr_buf"] + 0x44)
        check(d.path == "/gfx/t4.ncgr" and d.ram_addr == m["gfx_ncgr_buf"] and d.format == "NCGR", "an address inside the RAM copy finds the file")
        d = c.asset_describe(path="/gfx.narc:bg/title.ncgr")
        check(d.format == "NCGR" and d.unreadable_from != 0 and d.problem, "a damaged file is described as far as it goes: +0x%x %s" % (d.unreadable_from, d.problem))
        try:
            c.asset_describe(path="/nope.ncgr")
            check(False, "an unknown path is refused")
        except ApiError:
            check(True, "an unknown path is refused")

        print("asset.render")
        rom = c.asset_render(path="/gfx/t4.ncgr", palette_path="/gfx/t.nclr")
        ram = c.asset_render(addr=m["gfx_ncgr_buf"], palette_path="/gfx/t.nclr")
        check(rom.png == ram.png and rom.width == 32 and rom.height == 16, "tiles from the ROM and from RAM are the same image (%dx%d)" % (rom.width, rom.height))
        w, h, px = png_pixels(rom.png)
        check(all(px[y][x] == rgb(pal_color(tile_pixel(x, y))) for y in range(h) for x in range(w)), "every tile pixel is the expected colour")
        live = c.asset_render(path="/gfx/t4.ncgr")
        check(live.palette == "palette RAM (BG main)" and live.png == rom.png, "with no same-named palette, the colours in palette RAM are used: %s" % live.palette)
        raw = c.asset_render(addr=m["gfx_ncgr_buf"] + 0x30, size=256, part="raw", bpp=4, tiled=True, width=32, palette_path="/gfx/t.nclr")
        check(raw.png == rom.png, "the bytes viewed as 4bpp tiles, 32 wide, are the same image")
        mp = c.asset_render(path="/gfx/t.nscr")
        check(mp.tiles == "/gfx/t4.ncgr" and mp.palette == "/gfx/t.nclr" and mp.width == 32 and mp.height == 16,
              "the map pairs with t4.ncgr and t.nclr: %s, %s" % (mp.tiles, mp.palette))
        w, h, px = png_pixels(mp.png)
        ok = True
        for k in range(8):
            e = (k % 8) | ((k & 1) << 10) | (((k >> 1) & 1) << 11) | ((k % 2) << 12)
            tile, hf, vf, pn = e & 0x3FF, (e >> 10) & 1, (e >> 11) & 1, e >> 12
            for py in range(8):
                for pxx in range(8):
                    sx, sy = (7 - pxx if hf else pxx), (7 - py if vf else py)
                    v = tile_pixel((tile % 4) * 8 + sx, (tile // 4) * 8 + sy)
                    want_c = rgb(pal_color(pn * 16 + v)) if v else (0, 0, 0, 0)
                    if px[(k // 4) * 8 + py][(k % 4) * 8 + pxx] != want_c:
                        ok = False
        check(ok, "every map pixel (flips and palettes) is the expected colour")
        ce = c.asset_render(path="/gfx/t.ncer", index=0)
        check(ce.width == 24 and ce.height == 16, "the cell is drawn 24x16")
        for f in range(1, 8):
            t = c.asset_render(path="/gfx/t.nsbtx", part="texture", index=f - 1)
            tr = c.asset_render(addr=m["gfx_btx_buf"], part="texture", index=f - 1)
            w, h, px = png_pixels(t.png)
            check(t.png == tr.png and (w, h) == (8, 8) and all(px[y][x] == texel_expected(f, x, y) for y in range(8) for x in range(8)),
                  "texture %s: the expected texels, the same from RAM" % TEX_NAMES[f - 1])
        big = c.asset_render(path="/gfx/t.nclr", scale=4)
        check(big.width == 16 * 8 * 4 and big.height == 2 * 8 * 4, "palettes drawn as swatches, scaled: %dx%d" % (big.width, big.height))
    except ApiError as e:
        check(False, "API error: %s" % e)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
    print("\n%d passed, %d failed" % (PASSES, len(FAILS)))
    for f in FAILS:
        print("  FAIL " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
