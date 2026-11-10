/* Functions the compare test (tools/workbench/tests/test_compare_gcc.py) matches byte for byte.
   source/match.c builds them into the test ROM; the test compiles this file again with the same
   compiler and flags, with the workbench's context header (types, prototypes, globals) in front,
   and diffs each function against its bytes in RAM. No declarations here: the context has them. */

__attribute__((noinline)) s32 wb_match_leaf(s32 a, s32 b)
{
    s32 r = a * 5 - b;
    if (r < 0)
        r = -r;
    return r ^ 0x2A;
}

s32 wb_match_caller(s32 x)
{
    return wb_match_leaf(x, wb_match_gain) + wb_match_gain;
}

__attribute__((target("arm"))) u32 wb_match_arm(const u32* p, s32 n)
{
    u32 t = 0;
    while (n-- > 0)
        t += *p++ ^ 0x55;
    return t;
}
