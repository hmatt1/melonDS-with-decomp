/*
    Software LCD filter: see SoftLcdFilter.h. Follows kScreenFS in main_shaders.h;
    a change to one should go to the other.
*/

#include "SoftLcdFilter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{

constexpr int kW = 256, kH = 192;
constexpr int kLut = 4096;
constexpr int kLut8 = 16384;   // finer: the gamma curve is steep near black

// lookup tables, filled once: 8-bit -> linear, linear (0..1 in kLut steps) -> 8-bit gamma,
// and float gamma <-> linear for the blur
struct Luts
{
    float lin8[256];
    float lin[kLut + 1];      // gamma value -> linear
    float gam[kLut + 1];      // linear value -> gamma value
    uint8_t gam8[kLut8 + 1];  // linear value -> 8-bit gamma
    Luts()
    {
        for (int i = 0; i < 256; i++) lin8[i] = std::pow(i / 255.f, 2.2f);
        for (int i = 0; i <= kLut; i++)
        {
            float v = i / (float)kLut;
            lin[i] = std::pow(v, 2.2f);
            gam[i] = std::pow(v, 1.f / 2.2f);
        }
        for (int i = 0; i <= kLut8; i++)
            gam8[i] = (uint8_t)std::lround(std::pow(i / (float)kLut8, 1.f / 2.2f) * 255.f);
    }
};
const Luts& luts() { static const Luts l; return l; }

inline int lutIndex(float v) { return (int)(std::min(std::max(v, 0.f), 1.f) * kLut + 0.5f); }
inline int lut8Index(float v) { return (int)(std::min(std::max(v, 0.f), 1.f) * kLut8 + 0.5f); }

inline float smoothstep(float e0, float e1, float x)
{
    float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// kScreenFS stripeWeights(): how much of the left, middle and right stripe column i of n
// lights in row j
void stripeWeights(int i, int j, int n, float w[3])
{
    if (n == 4)
    {
        bool odd = j & 1;
        float a[3];
        if (i == 0)      { a[0] = 1; a[1] = 0; a[2] = 0; }
        else if (i == 3) { a[0] = 0; a[1] = 0; a[2] = 1; }
        else if (i == 1) { a[0] = odd ? 0 : 0.5f; a[1] = odd ? 1 : 0.5f; a[2] = 0; }
        else             { a[0] = 0; a[1] = odd ? 0.5f : 1; a[2] = odd ? 0.5f : 0; }
        w[0] = a[0]; w[1] = a[1] * 2.5f / 3.f; w[2] = a[2];
        return;
    }
    float a0 = i / (float)n, a1 = (i + 1) / (float)n;
    for (int k = 0; k < 3; k++)
        w[k] = std::clamp(std::min(a1, (k + 1) / 3.f) - std::max(a0, k / 3.f), 0.f, 1.f) * n;
}

} // namespace


// --- the filter -------------------------------------------------------------------------------

SoftLcdFilter::SoftLcdFilter(int threads)
{
    luts();
    setThreads(threads);
}

void SoftLcdFilter::setThreads(int threads)
{
    threads = std::max(1, threads);
    if (!pool || pool->size() != threads)
        pool = std::make_unique<SoftFilterPool>(threads);
}

void SoftLcdFilter::makeTable(Table& t, int n, const SoftFilterParams& p, int layer)
{
    t.n = n; t.p = p; t.layer = layer;
    t.cells.assign(n * n, Cell{});

    float aa = 1.f / n;
    float w = aa * p.gridWidth;
    float s = std::max(aa * p.gridSoft * 0.5f, 1e-4f);
    auto edge = [&](float d) { return p.grid ? 1.f - smoothstep(w - s, w + s, d) : 0.f; };

    // stripes in screen order (left, middle, right) as R G B weights
    bool rgb = p.subpixel == 3 && layer == 1;
    const float s0[3] = {rgb ? 1.f : 0.f, 0, rgb ? 0.f : 1.f};
    const float s1[3] = {0, 1, 0};
    const float s2[3] = {rgb ? 0.f : 1.f, 0, rgb ? 1.f : 0.f};
    bool sub = p.subpixel != 0;

    for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++)
    {
        Cell& c = t.cells[j * n + i];
        float ex = edge(1.f - (i + 0.5f) / n), ey = edge(1.f - (j + 0.5f) / n);
        float dark = std::max(p.gridDark * ex, p.gridDarkH * ey);
        c.keep = (uint16_t)std::lround(std::clamp(1.f - dark, 0.f, 1.f) * 256.f);
        if (sub)
        {
            float sw[3];
            stripeWeights(i, j, n, sw);
            for (int k = 0; k < 3; k++)
            {
                float m = sw[0] * s0[k] + sw[1] * s1[k] + sw[2] * s2[k];
                c.s[k] = 1.f + (m - 1.f) * p.subStrength;
            }
        }
        else
            c.s[0] = c.s[1] = c.s[2] = 1.f;
    }

    t.cellMean = 1.f;
    t.lineComp[0] = t.lineComp[1] = t.lineComp[2] = 1.f;
    t.lineBalance = false;
    if (!sub)
        return;
    float stripeMean = n == 4 ? 2.5f / 8.f : 1.f / 3.f;
    t.cellMean = 1.f + (stripeMean - 1.f) * p.subStrength;
    if (p.grid && (p.gridDark > 0.f || p.gridDarkH > 0.f))
    {
        // the vertical line covers the right stripe(s) more than the others. Sum what the lines
        // keep of each stripe's light (A over T) and of light spread evenly (G over N); each
        // pixel's mix of the two (its primary and spill, in band) then gives how much of each
        // channel the lines keep, and the other channels are dimmed to match (as kScreenFS)
        int m = std::clamp(n, 1, 8);
        float a[3] = {0, 0, 0}, tt[3] = {0, 0, 0}, g = 0.f;
        for (int jj = 0; jj < m; jj++)
        for (int ii = 0; ii < m; ii++)
        {
            float cx = (ii + 0.5f) / m, cy = (jj + 0.5f) / m;
            float dk = std::max(p.gridDark * edge(1.f - cx), p.gridDarkH * edge(1.f - cy));
            float sw[3];
            stripeWeights(ii, jj, m, sw);
            float lk = std::pow(1.f - dk, 2.2f);
            for (int k = 0; k < 3; k++)
            {
                float tk = 1.f + (sw[k] - 1.f) * p.subStrength;   // in stripe order
                a[k] += tk * lk;
                tt[k] += tk;
            }
            g += lk;
        }
        for (int k = 0; k < 3; k++)   // stripe order to channels
        {
            t.lineA[k] = a[0] * s0[k] + a[1] * s1[k] + a[2] * s2[k];
            t.lineT[k] = tt[0] * s0[k] + tt[1] * s1[k] + tt[2] * s2[k];
        }
        t.lineG = g;
        t.lineN = (float)(m * m);
        t.lineBalance = true;
    }
}

void SoftLcdFilter::process(const uint32_t* const src[2], uint32_t* const dst[2], int dstStride, int n,
                            const SoftFilterParams& p, unsigned layers)
{
    n = std::clamp(n, 1, 16);
    int list[2], nl = 0;
    for (int layer = 0; layer < 2; layer++)
        if (layers & (1u << layer))
        {
            bool rebuilt = false;
            if (table[layer].n != n || table[layer].layer != layer || !(table[layer].p == p))
            {
                makeTable(table[layer], n, p, layer);
                rebuilt = true;
            }
            // a screen whose picture and settings are as last time is left as it is (DS games
            // often draw at 30 fps, and one screen is often a still menu or map)
            Last& last = lastIn[layer];
            if (!rebuilt && last.valid && last.dst == dst[layer] && last.stride == dstStride &&
                std::memcmp(last.src.data(), src[layer], (size_t)kW * kH * 4) == 0)
                continue;
            last.src.assign(src[layer], src[layer] + (size_t)kW * kH);
            last.valid = true;
            last.dst = dst[layer];
            last.stride = dstStride;
            list[nl++] = layer;
        }
    if (!nl) return;

    // as many threads as screens: one screen each. More: as many bands per screen as threads, so
    // a thread that finishes early takes another band (bands only re-read the few rows the blur reaches)
    int bands = pool->size() <= nl ? 1 : pool->size();
    pool->run(nl * bands, [&](int job) {
        int layer = list[job / bands], b = job % bands;
        int y0 = kH * b / bands, y1 = kH * (b + 1) / bands;
        band(layer, y0, y1, src[layer], dst[layer], dstStride, table[layer]);
    });
}

void SoftLcdFilter::band(int layer, int y0, int y1, const uint32_t* src, uint32_t* dst, int dstStride,
                         const Table& t)
{
    const Luts& L = luts();
    const SoftFilterParams& p = t.p;
    const int n = t.n;
    const int stride = dstStride / 4;
    const bool sub = t.cellMean < 0.999f;
    const int rx = p.blurReachX > 0 && p.blurX > 0.f ? p.blurReachX : 0;
    const int ry = p.blurReachY > 0 && p.blurY > 0.f ? p.blurReachY : 0;
    const bool blur = rx > 0 || ry > 0;
    const float keepLight = sub ? (t.cellMean + (1.f - t.cellMean) * p.subKeep) / t.cellMean : 1.f;
    const float spillScale = sub ? t.cellMean / (1.f - t.cellMean) : 0.f;

    // per DS row: the light of each DS pixel (or primary and spill with stripes), in floats;
    // a ring of the horizontally blurred rows the vertical blur reaches. thread_local: each
    // pool thread keeps its own, so nothing is shared and nothing is allocated per frame.
    thread_local std::vector<float> rowLightV, ringV, convV;
    rowLightV.resize(kW * 6);
    float* rowLight = rowLightV.data();
    const int hRows = 2 * ry + 1;
    ringV.resize((size_t)hRows * kW * 6);   // per slot: plain (gamma) sums, then light sums
    convV.resize((kW + 2 * 3) * 6);           // one source row as gamma and linear, edge-padded
    float* ring = ringV.data();
    float* conv = convV.data();

    float wsumX = 1.f;
    for (int d = 1; d <= rx; d++) wsumX += 2.f * p.blurX;
    float wsumY = 1.f;
    for (int d = 1; d <= ry; d++) wsumY += 2.f * p.blurY;

    // horizontal blur of source row y (clamped) into ring slot `slot`
    auto hblur = [&](int y, int slot) {
        const uint32_t* in = src + std::clamp(y, 0, kH - 1) * kW;
        // convert once, padded by rx on each side with the edge pixel (the shader clamps)
        for (int x = -rx; x < kW + rx; x++)
        {
            uint32_t c = in[std::clamp(x, 0, kW - 1)];
            float* g = conv + (x + rx) * 6;
            int r = (c >> 16) & 255, gg = (c >> 8) & 255, bb = c & 255;
            g[0] = r * (1.f / 255.f); g[1] = gg * (1.f / 255.f); g[2] = bb * (1.f / 255.f);
            g[3] = L.lin8[r]; g[4] = L.lin8[gg]; g[5] = L.lin8[bb];
        }
        float* out = ring + (size_t)slot * kW * 6;
        for (int x = 0; x < kW; x++)
        {
            const float* c = conv + (x + rx) * 6;
            float acc[6];
            for (int k = 0; k < 6; k++) acc[k] = c[k];
            for (int d = 1; d <= rx; d++)
                for (int k = 0; k < 6; k++) acc[k] += (c[k - 6 * d] + c[k + 6 * d]) * p.blurX;
            for (int k = 0; k < 6; k++) out[x * 6 + k] = acc[k];
        }
    };
    if (blur)
        for (int y = y0 - ry; y < y0 + ry; y++) hblur(y, (y - (y0 - ry)) % hRows);

    for (int y = y0; y < y1; y++)
    {
        // stage 1: this DS row's light
        if (blur)
        {
            hblur(y + ry, (y + ry - (y0 - ry)) % hRows);
            const float* rows[7];
            float wy[7];
            for (int dy = -ry; dy <= ry; dy++)
            {
                rows[dy + ry] = ring + (size_t)((y + dy - (y0 - ry)) % hRows) * kW * 6;
                wy[dy + ry] = dy == 0 ? 1.f : p.blurY;
            }
            const float inv = 1.f / (wsumX * wsumY);
            for (int x = 0; x < kW; x++)
            {
                float acc[6] = {0, 0, 0, 0, 0, 0};
                for (int r = 0; r < hRows; r++)
                    for (int k = 0; k < 6; k++) acc[k] += rows[r][x * 6 + k] * wy[r];
                for (int k = 0; k < 3; k++)
                {
                    float plain = acc[k] * inv;
                    float pix = plain + (L.gam[lutIndex(acc[k + 3] * inv)] - plain) * p.halo;
                    rowLight[x * 3 + k] = L.lin[lutIndex(pix)];
                }
            }
        }
        else
        {
            const uint32_t* in = src + y * kW;
            for (int x = 0; x < kW; x++)
            {
                uint32_t c = in[x];
                rowLight[x * 3 + 0] = L.lin8[(c >> 16) & 255];
                rowLight[x * 3 + 1] = L.lin8[(c >> 8) & 255];
                rowLight[x * 3 + 2] = L.lin8[c & 255];
            }
        }
        if (sub)  // luminance preservation with overflow: primary in rowLight[0..], spill after it
            for (int x = 0; x < kW; x++)
                for (int k = 0; k < 3; k++)
                {
                    float target = rowLight[x * 3 + k] * keepLight;
                    float primary = std::min(target, 1.f);
                    rowLight[x * 3 + k] = primary;
                    rowLight[kW * 3 + x * 3 + k] = (target - primary) * spillScale;
                }

        // stage 2: the n output rows of this DS row; the cell table row and this DS row's
        // light stay in L1, the output is written once, in order
        for (int j = 0; j < n; j++)
        {
            uint32_t* out = dst + (size_t)(y * n + j) * stride;
            const Cell* crow = &t.cells[j * n];
            if (!sub)
            {
                for (int x = 0; x < kW; x++)
                {
                    int c[3];
                    for (int k = 0; k < 3; k++)
                        c[k] = L.gam8[lut8Index(rowLight[x * 3 + k] * t.lineComp[k])];
                    for (int i = 0; i < n; i++)
                    {
                        unsigned kp = crow[i].keep;
                        *out++ = 0xFF000000u | ((c[0] * kp >> 8) << 16) | ((c[1] * kp >> 8) << 8) | (c[2] * kp >> 8);
                    }
                }
            }
            else
            {
                for (int x = 0; x < kW; x++)
                {
                    // light = (spill + (primary - spill) * stripe) * lineComp = a + b * stripe
                    float comp[3] = {1.f, 1.f, 1.f};
                    if (t.lineBalance)
                    {
                        // how much of each channel's light the grid lines keep, given this
                        // pixel's primary and spill; the others dimmed to the lowest
                        float keep[3], kmin = 1.f;
                        for (int k = 0; k < 3; k++)
                        {
                            float pr = rowLight[x * 3 + k], sp = rowLight[kW * 3 + x * 3 + k];
                            float num = sp * t.lineG + (pr - sp) * t.lineA[k];
                            float den = sp * t.lineN + (pr - sp) * t.lineT[k];
                            keep[k] = den > 1e-6f ? num / den : 1.f;
                            kmin = std::min(kmin, keep[k]);
                        }
                        for (int k = 0; k < 3; k++) comp[k] = kmin / keep[k];
                    }
                    float a[3], b[3];
                    for (int k = 0; k < 3; k++)
                    {
                        float pr = rowLight[x * 3 + k], sp = rowLight[kW * 3 + x * 3 + k];
                        a[k] = sp * comp[k];
                        b[k] = (pr - sp) * comp[k];
                    }
                    for (int i = 0; i < n; i++)
                    {
                        const Cell& cl = crow[i];
                        unsigned v[3];
                        for (int k = 0; k < 3; k++)
                            v[k] = L.gam8[lut8Index(a[k] + b[k] * cl.s[k])] * (unsigned)cl.keep >> 8;
                        *out++ = 0xFF000000u | (v[0] << 16) | (v[1] << 8) | v[2];
                    }
                }
            }
        }
    }
}
