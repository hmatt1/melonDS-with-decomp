// Workbench test ROM, ARM7 side: the default ARM7 core (calico's template), plus code the
// integration test records on the ARM7:
//   - a tail call (tail7.s)
//   - an archived file's bytes, which the ARM9 read into main RAM, copied as they are and copied
//     through a lookup table (data origin and content labelling on the ARM7)
// The ARM9 sends the address of its Arm7Box over PXI; the ARM7 fills in its side of it.

#include <calico.h>
#include <nds.h>
#include <maxmod7.h>

typedef struct
{
    u32 src, size;              // ARM9: Models/test.nsbmd as read from vfs.zip
    u32 copy_dst, lut_dst;      // ARM9: where the ARM7 puts the two copies
    u32 tail_target, tail_target_end, tail_caller, tail_caller_end;   // ARM7: its functions
    u32 done;                   // ARM7: the copies are made
    u32 frames;                 // ARM7: frames run since
} Arm7Box;

extern u32 wb7_tail_target(u32 v);
extern u32 wb7_tail_caller(void);
extern char wb7_tail_target_end[], wb7_tail_caller_end[];

static volatile u32 box_addr;
static u8 lut[256];

static void OnBox(void* user, u32 data)
{
    (void)user;
    box_addr = data;
}

// apart from the copy: a table set up there could take the data's label through its loop counter
__attribute__((noinline)) static void InitLut(void)
{
    for (int i = 0; i < 256; i++) lut[i] = (u8)i;
}

__attribute__((noinline)) static void CopyPlain(const u8* src, u8* dst, u32 n)
{
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
}

// the output equals the input, but only through table lookups
__attribute__((noinline)) static void CopyThroughLut(const u8* src, u8* dst, u32 n)
{
    for (u32 i = 0; i < n; i++) dst[i] = lut[src[i]];
}

int main()
{
    envReadNvramSettings();
    keypadStartExtServer();
    lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
    irqEnable(IRQ_VBLANK);
    rtcInit();
    rtcSyncTime();
    pmInit();
    blkInit();
    touchInit();
    touchStartServer(80, MAIN_THREAD_PRIO);
    soundStartServer(MAIN_THREAD_PRIO-0x10);
    micStartServer(MAIN_THREAD_PRIO-0x18);
    wlmgrStartServer(MAIN_THREAD_PRIO-8);
    mmInstall(MAIN_THREAD_PRIO+1);

    pxiSetHandler(PxiChannel_User0, OnBox, NULL);

    u32 acc = 0;
    while (pmMainLoop())
    {
        threadWaitForVBlank();
        volatile Arm7Box* box = (volatile Arm7Box*)box_addr;
        if (!box) continue;
        if (!box->done)
        {
            box->tail_target = (u32)wb7_tail_target;
            box->tail_target_end = (u32)wb7_tail_target_end;
            box->tail_caller = (u32)wb7_tail_caller;
            box->tail_caller_end = (u32)wb7_tail_caller_end;
            InitLut();
            CopyPlain((const u8*)box->src, (u8*)box->copy_dst, box->size);
            CopyThroughLut((const u8*)box->src, (u8*)box->lut_dst, box->size);
            box->done = 1;
        }
        acc += wb7_tail_target(acc);    // a normal call first: the entry becomes known
        acc += wb7_tail_caller();       // then a tail call into it
        box->frames++;
    }
    return 0;
}
