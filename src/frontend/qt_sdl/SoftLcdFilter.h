/*
    Software version of the LCD filter screen shader (main_shaders.h, kScreenFS), for the
    plain (non-OpenGL) display. Same parameters, same look at whole-number zooms.

    The work is split in two stages per screen:
      1. at DS resolution (256x192): blur, convert to linear light, and work out each DS pixel's
         light per stripe (the "primary" and "spill" of the luminance-preserving stripes)
      2. upscale by n: every output pixel is its cell's light times a per-position mask
         (stripes, line darkness), looked up in an n x n cell table made once per zoom/settings
    Threads take bands of whole DS rows of one screen: a band reads the input (shared, read
    only, so the blur's overlap across band edges needs no locking) and writes only its own
    output rows. Output rows are whole multiples of 64 bytes at any zoom of 1 or more
    (256 * n * 4 bytes), so two threads never write the same cache line.
*/

#ifndef SOFTLCDFILTER_H
#define SOFTLCDFILTER_H

#include <cstdint>
#include <memory>
#include <vector>

#include "SoftFilterPool.h"

struct SoftFilterParams
{
    bool grid = true;
    float gridDark = 0.1f, gridDarkH = 0.5f, gridWidth = 1.f, gridSoft = 3.f;
    float subStrength = 0.6f, subKeep = 0.9f;
    float blurX = 0.f, blurY = 0.f, halo = 0.f;
    int blurReachX = 1, blurReachY = 1;
    int subpixel = 0;             // 0 none, 1 DS Lite, 2 DSi, 3 DSi XL

    bool operator==(const SoftFilterParams& o) const   // C++17: no defaulted ==
    {
        return grid == o.grid && gridDark == o.gridDark && gridDarkH == o.gridDarkH && gridWidth == o.gridWidth &&
               gridSoft == o.gridSoft && subStrength == o.subStrength && subKeep == o.subKeep && blurX == o.blurX &&
               blurY == o.blurY && halo == o.halo && blurReachX == o.blurReachX && blurReachY == o.blurReachY &&
               subpixel == o.subpixel;
    }
};

class SoftLcdFilter
{
public:
    explicit SoftLcdFilter(int threads = 2);

    // src: two 256x192 XRGB8888 screens (top, bottom); dst: two (256n)x(192n) XRGB8888 images,
    // rows dstStride bytes apart. layer: 0 top, 1 bottom (the DSi XL bottom screen is R G B).
    // layers: bit 0 top, bit 1 bottom; a screen the layout doesn't show is skipped.
    void process(const uint32_t* const src[2], uint32_t* const dst[2], int dstStride, int n,
                 const SoftFilterParams& p, unsigned layers = 3);

    void setThreads(int threads);
    int threads() const { return pool ? pool->size() : 1; }

private:
    struct Cell            // per output position in a DS pixel
    {
        float s[3];        // stripe weight per channel (R G B), 1 without stripes
        uint16_t keep;     // (1 - line darkness), 0..256
    };
    struct Table
    {
        int n = 0;
        SoftFilterParams p;
        int layer = -1;
        float cellMean = 1.f;
        float lineComp[3] = {1, 1, 1};
        // grid lines over stripes: per channel, what the lines keep of that channel's stripe light
        // (lineA over lineT), and of evenly spread light (lineG over lineN); see makeTable
        bool lineBalance = false;
        float lineA[3] = {0, 0, 0}, lineT[3] = {0, 0, 0}, lineG = 0.f, lineN = 0.f;
        std::vector<Cell> cells;   // n * n
    };

    void makeTable(Table& t, int n, const SoftFilterParams& p, int layer);
    void band(int layer, int y0, int y1, const uint32_t* src, uint32_t* dst, int dstStride, const Table& t);

    std::unique_ptr<SoftFilterPool> pool;
    Table table[2];
    // what each screen was last filtered from, to skip it when nothing changed
    struct Last { std::vector<uint32_t> src; bool valid = false; const uint32_t* dst = nullptr; int stride = 0; };
    Last lastIn[2];
};

#endif
