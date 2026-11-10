// Workbench recording test ROM.
//
// Exercises every kind of evidence the recording engine produces, with known
// addresses published in a manifest the integration test finds in RAM:
//   - an ARM function and a Thumb function with literal pools (testfuncs.s)
//   - a data table read by both, and copied every frame by DMA
//   - a NitroFS file read through the card (provenance: ROM file -> RAM)
//   - an LZ77 file decompressed by the BIOS (provenance: decompression)
//   - an LZ77 file unpacked in software through a lookup table (data origin: content labelling)
//   - a struct field computed from the decompressed data (data origin)
//   - a variable written every frame by a known function (write history)
//   - ARM7 code of its own (arm7/): a tail call, and an archived file's bytes copied on the ARM7
//   - files inside NARC and SDAT containers: read through the card, unpacked by the BIOS, nested, and
//     a NARC packed whole (gfx.carc)

#include <nds.h>
#include <filesystem.h>
#include <stdio.h>
#include <string.h>

extern u32 wb_arm_func(void);
extern u32 wb_thumb_func(void);
extern u32 wb_tail_target(u32 v);
extern u32 wb_tail_caller(void);
extern char wb_tail_target_end[], wb_tail_caller_end[];
extern u32 wb_kb_caller(void);
extern u32 wb_cond_caller(void);
extern char wb_kb_target[], wb_kb_target_end[], wb_kb_caller_end[], wb_cond_caller_end[];
extern u32 wb_data_table[4];
extern char wb_arm_func_end[], wb_thumb_func_end[], wb_thumb_leaf[], wb_thumb_leaf_end[], wb_data_table_end[];

typedef struct
{
    u32 magic;
    u32 speed;      // = unpacked word 2 (from packed.lz)
    u32 grip;       // = unpacked word 3 + 1
    u32 frames;     // not from the ROM
    u32 class_id;   // = wb_class_table[unpacked byte 16 & 7]: a table lookup indexed by file data
    u32 ratio;      // = speed / 7 on the hardware divider
    u32 root;       // = sqrt(grip) on the hardware square root unit
    u32 addr_like;  // = unpacked word 5 (0x02345678: data that looks like an address) + frame, every frame
    u32 mix;        // = unpacked word 2 + plain.lz's first word: two files in one value (a merged label)
    u32 total;      // SumWords over a word each of packed.lz, plain.lz and Models/test.nsbmd: a running total
} CarParams;

static const u32 wb_class_table[8] = {100, 200, 300, 400, 500, 600, 700, 800};

// one add instruction adds a word of each file to its own result: data origin shows it as a running
// total. The count is volatile so the loop stays a loop
static volatile int wb_parts_count = 3;
__attribute__((noinline)) static u32 SumWords(const u32* const* p, int n)
{
    u32 t = 0;
    for (int i = 0; i < n; i++) t += *p[i];
    return t;
}

typedef struct
{
    char magic[8];              // "DSWBTEST"
    u32 self;                   // address of this manifest
    u32 version;
    u32 arm_func, arm_func_end;
    u32 thumb_func, thumb_func_end;
    u32 thumb_leaf, thumb_leaf_end;
    u32 data_table, data_table_end;
    u32 dma_dst, dma_size;
    u32 payload_buf, payload_size;
    u32 packed_buf, packed_size;
    u32 unpacked_buf, unpacked_size;
    u32 car, car_speed_offset, car_grip_offset, car_class_offset;
    u32 write_target, writer_func;
    u32 main_func;
    u32 frame;
    u32 status;                 // bit0 nitroFS failed, bit1 payload missing, bit2 packed missing, bit3 vfs.zip unreadable, bits 6-8 a container unreadable, bit 9 a graphics file missing
    u32 ready;
    u32 vfs_buf, vfs_size;      // Models/test.nsbmd read out of vfs.zip (stored, so no inflating)
    u32 tail_target, tail_target_end, tail_caller, tail_caller_end;   // wb_tail_caller ends in "b wb_tail_target"
    u32 decoded_buf, decoded_size;   // Text/decoded.str, passed byte by byte through a lookup table
    u32 car_ratio_offset, car_root_offset;
    u32 kb_target, kb_target_end, kb_caller, kb_caller_end, cond_caller, cond_caller_end;
    u32 car_addr_like_offset;
    u32 plain_buf, plain_size;  // plain.lz unpacked in software, every literal through a lookup table
    u32 arm7_box;               // the Arm7Box shared with the ARM7
    u32 car_mix_offset, car_total_offset;
    u32 narc_title_buf, narc_title_size;    // gfx.narc bg/title.ncgr read through the card
    u32 narc_model_buf, narc_model_size;    // gfx.narc model.nsbmd, read packed and unpacked by the BIOS
    u32 nested_buf, nested_size;            // gfx.narc sub.narc read whole: a stored nested NARC
    u32 deep_buf, deep_size;                // its file 1 (a packed NARC) unpacked by the BIOS
    u32 carc_buf, carc_size;                // gfx.carc unpacked whole by the BIOS
    u32 sseq_buf, sseq_size;                // sound.sdat SEQ_RACE.sseq read through the card
    u32 gfx_nclr_buf, gfx_nclr_size;        // gfx/t.nclr, read whole; its colours also in BG palette RAM
    u32 gfx_ncgr_buf, gfx_ncgr_size;        // gfx/t4.ncgr; its tiles also in VRAM bank A
    u32 gfx_nscr_buf, gfx_nscr_size;        // gfx/t.nscr; its map also in VRAM bank A
    u32 gfx_ncer_buf, gfx_ncer_size;        // gfx/t.ncer
    u32 gfx_btx_buf, gfx_btx_size;          // gfx/t.nsbtx; copied whole into VRAM bank B
} Manifest;

// shared with arm7/source/main.c (keep in step)
typedef struct
{
    u32 src, size;              // ARM9: Models/test.nsbmd as read from vfs.zip
    u32 copy_dst, lut_dst;      // ARM9: where the ARM7 puts the two copies
    u32 tail_target, tail_target_end, tail_caller, tail_caller_end;   // ARM7: its functions
    u32 done;                   // ARM7: the copies are made
    u32 frames;                 // ARM7: frames run since
} Arm7Box;

Arm7Box wb_arm7_box __attribute__((aligned(32)));
static u8 arm7_copy[256] __attribute__((aligned(32)));
static u8 arm7_lut[256] __attribute__((aligned(32)));

Manifest wb_manifest __attribute__((aligned(32)));
s32 wb_match_caller(s32 x);
u32 wb_match_arm(const u32* p, s32 n);
volatile u32 wb_match_result;
CarParams wb_car __attribute__((aligned(32)));
u32 wb_dma_dst[4] __attribute__((aligned(32)));
volatile u32 wb_write_target;
static u8 payload[256] __attribute__((aligned(32)));
static u8 packed[2048] __attribute__((aligned(32)));
static u8 unpacked[1024] __attribute__((aligned(32)));

__attribute__((noinline, target("arm"))) void wb_writer(u32 v)
{
    wb_write_target = v * 3 + 1;
}

static u8 vfs_model[1024] __attribute__((aligned(32)));

static u32 ReadFile(const char* path, u8* buf, u32 max)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    u32 n = fread(buf, 1, max, f);
    fclose(f);
    return n;
}

static u8 vfs_decoded[2048] __attribute__((aligned(32)));
static u8 decode_table[256];
static u8 plain_lz[1024] __attribute__((aligned(32)));
static u8 plain_out[1024] __attribute__((aligned(32)));

// apart from the decoder: a table set up there could take the file's label through its loop counter
__attribute__((noinline)) static void InitDecodeTable(void)
{
    for (int i = 0; i < 256; i++) decode_table[i] = (u8)i;
}

// LZ77 (type 0x10) in software, every literal through the lookup table: the output equals the
// file's unpacked form, but its taint is lost
__attribute__((noinline)) static u32 SoftUnpackLz(const u8* in, u32 len, u8* out, u32 max)
{
    u32 size = in[1] | (in[2] << 8) | (in[3] << 16);
    if (len < 4 || in[0] != 0x10 || size > max) return 0;
    u32 p = 4, n = 0;
    while (n < size && p < len)
    {
        u8 flags = in[p++];
        for (int b = 7; b >= 0 && n < size; b--)
        {
            if (flags & (1 << b))
            {
                u32 l = (in[p] >> 4) + 3, d = (((in[p] & 0xF) << 8) | in[p + 1]) + 1;
                p += 2;
                while (l-- && n < size) { out[n] = out[n - d]; n++; }
            }
            else out[n++] = decode_table[in[p++]];
        }
    }
    return n;
}

// one chunk through the table: the output equals the input, but only through table lookups
__attribute__((noinline)) static void DecodeChunk(const u8* in, u8* out, u32 n)
{
    for (u32 i = 0; i < n; i++) out[i] = decode_table[in[i]];
}

// a stored member of vfs.zip by name: walk the local headers, then stream its data in chunks
static u32 ReadDecoded(const char* name, u8* buf, u32 max)
{
    FILE* f = fopen("nitro:/vfs.zip", "rb");
    if (!f) return 0;
    for (int i = 0; i < 256; i++) decode_table[i] = (u8)i;
    u32 pos = 0, n = 0;
    u8 hdr[30];
    char fname[64];
    while (fseek(f, pos, SEEK_SET) == 0 && fread(hdr, 1, 30, f) == 30 && hdr[0] == 'P' && hdr[1] == 'K' && hdr[2] == 3)
    {
        u32 csize = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
        u32 nlen = hdr[26] | (hdr[27] << 8), xlen = hdr[28] | (hdr[29] << 8);
        u32 len = nlen < sizeof(fname) - 1 ? nlen : sizeof(fname) - 1;
        fread(fname, 1, len, f);
        fname[len] = 0;
        if (!strcmp(fname, name) && hdr[8] == 0 && csize <= max)
        {
            fseek(f, pos + 30 + nlen + xlen, SEEK_SET);
            u8 chunk[64];
            while (n < csize)
            {
                u32 k = csize - n < sizeof(chunk) ? csize - n : sizeof(chunk);
                if (fread(chunk, 1, k, f) != k) break;
                DecodeChunk(chunk, buf + n, k);
                n += k;
            }
            break;
        }
        pos += 30 + nlen + xlen + csize;
    }
    fclose(f);
    return n;
}

// the first member of vfs.zip, the way the Octane engine reads one file: local header, then its data
static u32 ReadArchived(u8* buf, u32 max)
{
    FILE* f = fopen("nitro:/vfs.zip", "rb");
    if (!f) return 0;
    u8 hdr[30];
    u32 n = 0;
    if (fread(hdr, 1, 30, f) == 30 && hdr[0] == 'P' && hdr[1] == 'K' && hdr[8] == 0)   // stored
    {
        u32 size = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24);
        u32 skip = (hdr[26] | (hdr[27] << 8)) + (hdr[28] | (hdr[29] << 8));
        if (size <= max && fseek(f, 30 + skip, SEEK_SET) == 0) n = fread(buf, 1, size, f);
    }
    fclose(f);
    return n;
}

// ---- NARC and SDAT

static u32 Rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// where file index of a NARC is: offset and size from its BTAF, relative to the NARC's start. Only the
// headers need to be in n (len bytes)
static int NarcEntry(const u8* n, u32 len, u32 index, u32* off, u32* size)
{
    if (len < 16 || memcmp(n, "NARC", 4)) return 0;
    u32 pos = n[12] | (n[13] << 8), btaf = 0, gmif = 0;
    for (int s = 0; s < 3 && pos + 8 <= len; s++)
    {
        if (!memcmp(n + pos, "BTAF", 4)) btaf = pos;
        else if (!memcmp(n + pos, "GMIF", 4)) gmif = pos;
        pos += Rd32(n + pos + 4);
    }
    if (!btaf || !gmif || btaf + 12 + index * 8 + 8 > len || index >= (u32)(n[btaf + 8] | (n[btaf + 9] << 8))) return 0;
    u32 s = Rd32(n + btaf + 12 + index * 8), e = Rd32(n + btaf + 16 + index * 8);
    *off = gmif + 8 + s;
    *size = e - s;
    return 1;
}

// one file of a NARC in NitroFS: its headers, then just its bytes
static u32 ReadNarcFile(const char* path, u32 index, u8* buf, u32 max)
{
    static u8 hdr[256] __attribute__((aligned(4)));
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    u32 n = 0, off, size;
    u32 got = fread(hdr, 1, sizeof(hdr), f);
    if (NarcEntry(hdr, got, index, &off, &size) && size <= max && fseek(f, off, SEEK_SET) == 0) n = fread(buf, 1, size, f);
    fclose(f);
    return n;
}

// one file of an SDAT in NitroFS, by its FAT index
static u32 ReadSdatFile(const char* path, u32 index, u8* buf, u32 max)
{
    u8 h[0x40], e[8];
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    u32 n = 0;
    if (fread(h, 1, sizeof(h), f) == sizeof(h) && !memcmp(h, "SDAT", 4) &&
        fseek(f, Rd32(h + 0x20) + 12 + index * 16, SEEK_SET) == 0 && fread(e, 1, 8, f) == 8 &&
        Rd32(e + 4) <= max && fseek(f, Rd32(e), SEEK_SET) == 0)
        n = fread(buf, 1, Rd32(e + 4), f);
    fclose(f);
    return n;
}

static u8 narc_title[1024] __attribute__((aligned(32)));
static u8 narc_model_packed[1024] __attribute__((aligned(32)));
static u8 narc_model[1024] __attribute__((aligned(32)));
static u8 nested[1024] __attribute__((aligned(32)));
static u8 deep[1024] __attribute__((aligned(32)));
static u8 carc_packed[2048] __attribute__((aligned(32)));
static u8 carc[2048] __attribute__((aligned(32)));
static u8 sseq[1024] __attribute__((aligned(32)));

static void ReadContainers(Manifest* m)
{
    m->narc_title_buf = (u32)narc_title;
    m->narc_title_size = ReadNarcFile("nitro:/gfx.narc", 2, narc_title, sizeof(narc_title));
    m->narc_model_buf = (u32)narc_model;
    if (ReadNarcFile("nitro:/gfx.narc", 0, narc_model_packed, sizeof(narc_model_packed)))
    {
        m->narc_model_size = Rd32(narc_model_packed) >> 8;
        DC_FlushRange(narc_model_packed, sizeof(narc_model_packed));
        swiDecompressLZSSWram(narc_model_packed, narc_model);
    }
    m->nested_buf = (u32)nested;
    m->nested_size = ReadNarcFile("nitro:/gfx.narc", 1, nested, sizeof(nested));
    u32 off, size;
    m->deep_buf = (u32)deep;
    if (NarcEntry(nested, m->nested_size, 1, &off, &size) && off + size <= m->nested_size)
    {
        m->deep_size = Rd32(nested + off) >> 8;
        DC_FlushRange(nested, sizeof(nested));
        swiDecompressLZSSWram(nested + off, deep);
    }
    m->carc_buf = (u32)carc;
    if (ReadFile("nitro:/gfx.carc", carc_packed, sizeof(carc_packed)))
    {
        m->carc_size = Rd32(carc_packed) >> 8;
        DC_FlushRange(carc_packed, sizeof(carc_packed));
        swiDecompressLZSSWram(carc_packed, carc);
    }
    m->sseq_buf = (u32)sseq;
    m->sseq_size = ReadSdatFile("nitro:/sound.sdat", 1, sseq, sizeof(sseq));
    if (!m->narc_title_size || !m->narc_model_size || !m->deep_size) m->status |= 64;
    if (!m->carc_size) m->status |= 128;
    if (!m->sseq_size) m->status |= 256;
}

// the graphics test files (gfx/): read whole into RAM, and their tiles, map, colours and textures
// copied into VRAM and palette RAM. VRAM takes 16-bit writes only
static u8 gfx_nclr[128] __attribute__((aligned(32)));
static u8 gfx_ncgr[320] __attribute__((aligned(32)));
static u8 gfx_nscr[64] __attribute__((aligned(32)));
static u8 gfx_ncer[96] __attribute__((aligned(32)));
static u8 gfx_btx[1600] __attribute__((aligned(32)));

static void Copy16(volatile u16* dst, const u8* src, u32 size)
{
    for (u32 i = 0; i + 1 < size; i += 2) dst[i / 2] = (u16)(src[i] | (src[i + 1] << 8));
}

static void ReadGraphics(Manifest* m)
{
    m->gfx_nclr_buf = (u32)gfx_nclr;
    m->gfx_nclr_size = ReadFile("nitro:/gfx/t.nclr", gfx_nclr, sizeof(gfx_nclr));
    m->gfx_ncgr_buf = (u32)gfx_ncgr;
    m->gfx_ncgr_size = ReadFile("nitro:/gfx/t4.ncgr", gfx_ncgr, sizeof(gfx_ncgr));
    m->gfx_nscr_buf = (u32)gfx_nscr;
    m->gfx_nscr_size = ReadFile("nitro:/gfx/t.nscr", gfx_nscr, sizeof(gfx_nscr));
    m->gfx_ncer_buf = (u32)gfx_ncer;
    m->gfx_ncer_size = ReadFile("nitro:/gfx/t.ncer", gfx_ncer, sizeof(gfx_ncer));
    m->gfx_btx_buf = (u32)gfx_btx;
    m->gfx_btx_size = ReadFile("nitro:/gfx/t.nsbtx", gfx_btx, sizeof(gfx_btx));
    if (!m->gfx_nclr_size || !m->gfx_ncgr_size || !m->gfx_nscr_size || !m->gfx_ncer_size || !m->gfx_btx_size)
    {
        m->status |= 512;
        return;
    }
    // bank A as the main engine's BG: the map at +0, the tiles at +0x4000 (the files' layouts are fixed:
    // tiles at 0x30, map at 0x24, colours at 0x28)
    vramSetBankA(VRAM_A_MAIN_BG);
    Copy16((volatile u16*)BG_GFX, gfx_nscr + 0x24, 16);
    Copy16((volatile u16*)BG_GFX + 0x2000, gfx_ncgr + 0x30, 256);
    Copy16((volatile u16*)BG_PALETTE, gfx_nclr + 0x28, 64);
    // bank B, as plain memory: the texture file
    vramSetBankB(VRAM_B_LCD);
    Copy16((volatile u16*)VRAM_B + 0x800, gfx_btx, m->gfx_btx_size);
}

int main(void)
{
    consoleDemoInit();
    Manifest* m = &wb_manifest;
    memset(m, 0, sizeof(*m));
    m->self = (u32)m;
    m->version = 1;
    m->arm_func = (u32)wb_arm_func;
    m->arm_func_end = (u32)wb_arm_func_end;
    m->thumb_func = (u32)wb_thumb_func & ~1u;
    m->thumb_func_end = (u32)wb_thumb_func_end;
    m->thumb_leaf = (u32)wb_thumb_leaf & ~1u;
    m->thumb_leaf_end = (u32)wb_thumb_leaf_end;
    m->data_table = (u32)wb_data_table;
    m->data_table_end = (u32)wb_data_table_end;
    m->dma_dst = (u32)wb_dma_dst;
    m->dma_size = sizeof(wb_dma_dst);
    m->payload_buf = (u32)payload;
    m->packed_buf = (u32)packed;
    m->unpacked_buf = (u32)unpacked;
    m->car = (u32)&wb_car;
    m->car_speed_offset = offsetof(CarParams, speed);
    m->car_grip_offset = offsetof(CarParams, grip);
    m->car_class_offset = offsetof(CarParams, class_id);
    m->write_target = (u32)&wb_write_target;
    m->writer_func = (u32)wb_writer;
    m->main_func = (u32)main & ~1u;
    m->tail_target = (u32)wb_tail_target;
    m->tail_target_end = (u32)wb_tail_target_end;
    m->tail_caller = (u32)wb_tail_caller;
    m->tail_caller_end = (u32)wb_tail_caller_end;
    m->kb_target = (u32)wb_kb_target;
    m->kb_target_end = (u32)wb_kb_target_end;
    m->kb_caller = (u32)wb_kb_caller;
    m->kb_caller_end = (u32)wb_kb_caller_end;
    m->cond_caller = (u32)wb_cond_caller;
    m->cond_caller_end = (u32)wb_cond_caller_end;

    if (!nitroFSInit(NULL)) m->status |= 1;
    m->payload_size = ReadFile("nitro:/payload.bin", payload, sizeof(payload));
    if (!m->payload_size) m->status |= 2;
    m->packed_size = ReadFile("nitro:/packed.lz", packed, sizeof(packed));
    if (!m->packed_size) m->status |= 4;
    m->vfs_buf = (u32)vfs_model;
    m->vfs_size = ReadArchived(vfs_model, sizeof(vfs_model));
    if (!m->vfs_size) m->status |= 8;
    m->decoded_buf = (u32)vfs_decoded;
    m->decoded_size = ReadDecoded("Text/decoded.str", vfs_decoded, sizeof(vfs_decoded));
    if (!m->decoded_size) m->status |= 16;
    m->plain_buf = (u32)plain_out;
    InitDecodeTable();
    m->plain_size = SoftUnpackLz(plain_lz, ReadFile("nitro:/plain.lz", plain_lz, sizeof(plain_lz)), plain_out, sizeof(plain_out));
    if (!m->plain_size) m->status |= 32;
    if (m->packed_size)
    {
        m->unpacked_size = *(u32*)packed >> 8;
        DC_FlushRange(packed, sizeof(packed));
        swiDecompressLZSSWram(packed, unpacked);
    }
    wb_car.magic = 0x52414321;
    wb_car.speed = ((u32*)unpacked)[2];
    wb_car.grip = ((u32*)unpacked)[3] + 1;
    wb_car.class_id = wb_class_table[unpacked[16] & 7];
    wb_car.ratio = (u32)div32((int)wb_car.speed & 0x7FFFFFFF, 7);
    wb_car.root = sqrt32((int)(wb_car.grip & 0x7FFFFFFF));
    m->car_ratio_offset = offsetof(CarParams, ratio);
    m->car_root_offset = offsetof(CarParams, root);
    m->car_addr_like_offset = offsetof(CarParams, addr_like);
    wb_car.mix = ((u32*)unpacked)[2] + ((u32*)plain_out)[0];
    const u32* parts[3] = {(u32*)unpacked + 4, (u32*)plain_out + 1, (u32*)vfs_model + 2};
    wb_car.total = SumWords(parts, wb_parts_count);
    m->car_mix_offset = offsetof(CarParams, mix);
    m->car_total_offset = offsetof(CarParams, total);
    ReadContainers(m);
    ReadGraphics(m);
    wb_arm7_box.src = (u32)vfs_model;
    wb_arm7_box.size = m->vfs_size < sizeof(arm7_copy) ? m->vfs_size : sizeof(arm7_copy);
    wb_arm7_box.copy_dst = (u32)arm7_copy;
    wb_arm7_box.lut_dst = (u32)arm7_lut;
    m->arm7_box = (u32)&wb_arm7_box;
    DC_FlushAll();
    // the ARM7 writes these: no stale cache lines of ours may be written back over them
    DC_InvalidateRange(&wb_arm7_box, sizeof(wb_arm7_box));
    DC_InvalidateRange(arm7_copy, sizeof(arm7_copy));
    DC_InvalidateRange(arm7_lut, sizeof(arm7_lut));
    pxiWaitRemote(PxiChannel_User0);
    pxiSend(PxiChannel_User0, (u32)&wb_arm7_box);

    // the compare test's functions (match.c): called once, so the linker keeps them
    wb_match_result = (u32)wb_match_caller((s32)m->version) + wb_match_arm(wb_data_table, 4);

    memcpy(m->magic, "DSWBTEST", 8);
    m->ready = 1;

    printf("workbench test ROM\nstatus %lu\npayload %lu packed %lu\nunpacked %lu\n",
           (unsigned long)m->status, (unsigned long)m->payload_size, (unsigned long)m->packed_size, (unsigned long)m->unpacked_size);

    u32 frame = 0;
    while (1)
    {
        swiWaitForVBlank();
        u32 a = wb_arm_func();
        u32 b = wb_thumb_func();
        wb_writer(frame);
        a += wb_tail_target(frame);         // a normal call first: the entry becomes known
        a += wb_tail_caller();              // then a tail call into it
        a += wb_kb_caller();                // a tail call into a function only the knowledge store knows
        a += wb_cond_caller();              // a conditional tail call
        dmaCopyWords(3, wb_data_table, wb_dma_dst, sizeof(wb_dma_dst));
        wb_car.frames = frame;
        wb_car.addr_like = ((volatile u32*)unpacked)[5] + frame;
        frame++;
        m->frame = frame;
        if ((frame & 63) == 0) printf("\x1b[6;0Hframe %lu %08lx %08lx\n", (unsigned long)frame, (unsigned long)a, (unsigned long)b);
    }
    return 0;
}
