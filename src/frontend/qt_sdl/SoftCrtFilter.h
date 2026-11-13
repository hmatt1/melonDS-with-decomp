/*
    Software version of the CRT filter screen shader (main_shaders.h: kCrtPrepassFS and kCrtFS),
    for the plain (non-OpenGL) display. Same parameters, same look at whole-number zooms (with
    curvature, the sideways signal is taken at the nearest of 2n points per DS pixel).

    After the CRT shader in "CRT shader for pixel art" (spritecook.ai): a horizontal-only signal
    blur in YIQ (sharp luma, wider chroma), Gaussian scanline beams whose width follows
    brightness, an aperture-grille mask in screen pixels, a glow, edge darkening and curvature.

    The work, per shown screen, in batches on the pool:
      1. at DS resolution (256x192): RGB -> YIQ with the luma [1 2 1] prefilter, and the glow
         (a separable 5x5 Gaussian in linear light)
      2. per DS row: the blurred signal at each of the 256n output columns (tap weights depend
         only on the column's phase in its DS pixel), stored as 12-bit gamma levels, and the glow
         widened to 256n columns
      3. per output row: two beam lookups per channel (a table per row phase and level), plus the
         glow lerped between two DS rows; then mask, edge darkening, brightness and encode.
         With curvature each output pixel instead finds its warped point: the signal there (from
         columns at 2n phases per DS pixel, the nearest one), two beams from a table by level and
         distance, and the glow read bilinearly, as the shader does
    Bands are whole rows, and output rows are whole multiples of 64 bytes, so threads never write
    the same cache line.
*/

#ifndef SOFTCRTFILTER_H
#define SOFTCRTFILTER_H

#include <cstdint>
#include <memory>
#include <vector>

#include "SoftFilterPool.h"

struct SoftCrtParams
{
    float curvature = 0.25f;     // 0..1; 1 = warp (0.124, 0.164)
    float lumaBlur = 0.45f;      // DS px
    float chromaBlur = 1.2f;     // DS px
    float beamMin = 0.21f;       // beam sigma on black, DS rows
    float beamMax = 0.40f;       // beam sigma on white
    float mask = 1.f;            // 0..1
    float maskPx = 1.f;          // screen px per stripe
    float glow = 0.15f;
    float vignette = 0.22f;
    float boost = 1.25f;

    bool operator==(const SoftCrtParams& o) const
    {
        return curvature == o.curvature && lumaBlur == o.lumaBlur && chromaBlur == o.chromaBlur &&
               beamMin == o.beamMin && beamMax == o.beamMax && mask == o.mask && maskPx == o.maskPx &&
               glow == o.glow && vignette == o.vignette && boost == o.boost;
    }
};

class SoftCrtFilter
{
public:
    explicit SoftCrtFilter(int threads = 2);

    // src: two 256x192 XRGB8888 screens (top, bottom); dst: two (256n)x(192n) XRGB8888 images,
    // rows dstStride bytes apart. layers: bit 0 top, bit 1 bottom; others are left alone.
    void process(const uint32_t* const src[2], uint32_t* const dst[2], int dstStride, int n,
                 const SoftCrtParams& p, unsigned layers = 3);

    void setThreads(int threads);
    int threads() const { return pool ? pool->size() : 1; }

private:
    static constexpr int kTaps = 9;

    struct Tables
    {
        int n = 0;
        int phases = 0;                  // signal columns per DS pixel: n flat, 2n curved
        SoftCrtParams p;
        bool valid = false;
        std::vector<float> tapL, tapC;   // [phase][kTaps], normalized
        std::vector<float> beam;         // x 1.32; flat: [row phase][2 rows][4096 levels],
                                         // curved: [256 levels][65 distances]
        std::vector<float> gain;         // [column][3]: mask x brightness
        std::vector<float> cx, cy;       // screen position -1..1 of each output column / row
    };

    struct Layer
    {
        std::vector<float> yiq;          // [192][256 + 8][3]: prefiltered luma, I, Q, edge-padded
        std::vector<float> glow;         // [192][256][3], linear, x glow strength
        std::vector<float> glowTmp;      // [192][256][3], the horizontal glow pass
        std::vector<uint16_t> sig;       // [192][256 * phases][3]: signal, 12-bit gamma level
        std::vector<float> glowH;        // flat only: [192][256n][3]
    };

    void makeTables(int n, const SoftCrtParams& p);
    void dsStage(Layer& L, const uint32_t* src);
    void rowStage(Layer& L, int y0, int y1);
    void outStage(Layer& L, int y0, int y1, uint32_t* dst, int stride);
    void curvedStage(Layer& L, int y0, int y1, uint32_t* dst, int stride);

    std::unique_ptr<SoftFilterPool> pool;
    Tables t;
    Layer layer[2];
};

#endif
