/* wb_match.c with one deliberate change in each function: the compare test expects these not to
   match (the original multiplies by 5, subtracts gain once, and XORs with 0x55). */

__attribute__((noinline)) s32 wb_match_leaf(s32 a, s32 b)
{
    s32 r = a * 3 - b;
    if (r < 0)
        r = -r;
    return r ^ 0x2A;
}

s32 wb_match_caller(s32 x)
{
    return wb_match_leaf(x, wb_match_gain) - wb_match_gain;
}

__attribute__((target("arm"))) u32 wb_match_arm(const u32* p, s32 n)
{
    u32 t = 0;
    while (n-- > 0)
        t += *p++ ^ 0x56;
    return t;
}
