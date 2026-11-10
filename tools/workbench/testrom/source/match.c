// The compare test's functions (match_src/wb_match.c), built into the ROM. The declarations here
// are what the workbench's context header gives the test's own compile of that file.
#include <nds/ndstypes.h>

s32 wb_match_gain = 7;
s32 wb_match_leaf(s32 a, s32 b);
s32 wb_match_caller(s32 x);
u32 wb_match_arm(const u32* p, s32 n);

#include "../match_src/wb_match.c"
