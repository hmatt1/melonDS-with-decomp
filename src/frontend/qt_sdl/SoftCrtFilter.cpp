/*
    Software CRT filter: see SoftCrtFilter.h. Follows kCrtPrepassFS and kCrtFS in main_shaders.h;
    a change to one should go to the other.
*/

#include "SoftCrtFilter.h"

#include <algorithm>
#include <cmath>

namespace
{

constexpr int kW = 256, kH = 192;
constexpr int kPad = 4;                  // signal taps reach 4 DS pixels each way
constexpr int kRowW = kW + 2 * kPad;     // a padded YIQ row
constexpr int kLevels = 4096;            // signal levels (12-bit gamma)
constexpr int kEnc = 16384;              // encode steps: the gamma curve is steep near black

// the shaders' constants
constexpr float kBeamGain = 1.32f;
constexpr float kGlowVar = 2.56f;
constexpr float kWarpX = 0.124f, kWarpY = 0.164f;   // at curvature 1; 0.25 gives (0.031, 0.041)
// curved screen: beams by distance, in kBeamSteps steps of a DS row (interpolated), for 256 levels
constexpr int kBeamSteps = 64;
constexpr int kBeamLevels = 256;

struct Luts
{
    float lin24[256];        // 8-bit -> linear (2.4), for the glow
    uint8_t enc[kEnc + 1];   // linear -> 8-bit gamma (1/2.2)
    Luts()
    {
        for (int i = 0; i < 256; i++) lin24[i] = std::pow(i / 255.f, 2.4f);
        for (int i = 0; i <= kEnc; i++)
            enc[i] = (uint8_t)std::lround(std::pow(i / (float)kEnc, 1.f / 2.2f) * 255.f);
    }
};
const Luts& luts() { static const Luts l; return l; }

inline uint32_t encode(const Luts& L, float r, float g, float b)
{
    auto idx = [](float v) { return (int)(std::min(std::max(v, 0.f), 1.f) * kEnc + 0.5f); };
    return 0xFF000000u | ((uint32_t)L.enc[idx(r)] << 16) | ((uint32_t)L.enc[idx(g)] << 8) | L.enc[idx(b)];
}

// edge darkening from a (warped) screen position -1..1
inline float vignette(float x, float y, float amount)
{
    float t = std::min((x * x + y * y) * 0.5f, 1.f);
    return 1.f - amount * t * std::sqrt(t);
}


} // namespace


SoftCrtFilter::SoftCrtFilter(int threads)
{
    luts();
    setThreads(threads);
}

void SoftCrtFilter::setThreads(int threads)
{
    threads = std::max(1, threads);
    if (!pool || pool->size() != threads)
        pool = std::make_unique<SoftFilterPool>(threads);
}

void SoftCrtFilter::makeTables(int n, const SoftCrtParams& p)
{
    const bool curved = p.curvature > 0.f;
    t.n = n; t.p = p; t.valid = true;
    t.phases = curved ? 2 * n : n;
    const int W = kW * n, H = kH * n, P = t.phases;

    // signal taps: phase i of P in a DS pixel sits at x + (i + 0.5) / P; tap k is the DS pixel
    // k - 4 from it, at its centre
    t.tapL.assign(P * kTaps, 0.f);
    t.tapC.assign(P * kTaps, 0.f);
    const float il = -0.5f / std::max(p.lumaBlur * p.lumaBlur, 1e-4f);
    const float ic = -0.5f / std::max(p.chromaBlur * p.chromaBlur, 1e-4f);
    for (int i = 0; i < P; i++)
    {
        float px = (i + 0.5f) / P, sl = 0.f, sc = 0.f;
        for (int k = 0; k < kTaps; k++)
        {
            float d = (k - kPad) + 0.5f - px;
            sl += t.tapL[i * kTaps + k] = std::exp(d * d * il);
            sc += t.tapC[i * kTaps + k] = std::exp(d * d * ic);
        }
        for (int k = 0; k < kTaps; k++) { t.tapL[i * kTaps + k] /= sl; t.tapC[i * kTaps + k] /= sc; }
    }

    auto sigma = [&](float lin) { return std::max(p.beamMin + (p.beamMax - p.beamMin) * std::pow(lin, 0.6f), 1e-3f); };
    if (curved)
    {
        // any distance: the beam per level at kBeamSteps + 1 distances from 0 to 1 DS row,
        // read with linear interpolation (the error stays under 0.001 of full brightness)
        t.beam.assign((size_t)kBeamLevels * (kBeamSteps + 1), 0.f);
        for (int l = 0; l < kBeamLevels; l++)
        {
            float lin = std::pow(l / (float)(kBeamLevels - 1), 2.4f);
            float s = sigma(lin);
            for (int k = 0; k <= kBeamSteps; k++)
            {
                float d = k / (float)kBeamSteps;
                t.beam[(size_t)l * (kBeamSteps + 1) + k] = lin * std::exp(-0.5f * d * d / (s * s)) * kBeamGain;
            }
        }
    }
    else
    {
        // row j of n in a DS row sits (j + 0.5) / n - 0.5 below the row's centre line; the two
        // nearest rows are f and 1 - f away
        t.beam.assign((size_t)n * 2 * kLevels, 0.f);
        for (int j = 0; j < n; j++)
        {
            float fy = (j + 0.5f) / n - 0.5f;
            float f = fy < 0.f ? fy + 1.f : fy;
            for (int r = 0; r < 2; r++)
            {
                float d = r ? 1.f - f : f;
                float* b = &t.beam[((size_t)j * 2 + r) * kLevels];
                for (int l = 0; l < kLevels; l++)
                {
                    float lin = std::pow(l / (float)(kLevels - 1), 2.4f);
                    float s = sigma(lin);
                    b[l] = lin * std::exp(-0.5f * d * d / (s * s)) * kBeamGain;
                }
            }
        }
    }

    // aperture grille: R G B stripes maskPx screen pixels wide, by output column
    const float light = (1.f + (1.22f - 1.f) * p.mask) * p.boost;
    const float dark = (1.f + (0.62f - 1.f) * p.mask) * p.boost;
    const float mpx = std::max(p.maskPx, 1.f);
    t.gain.resize((size_t)W * 3);
    for (int x = 0; x < W; x++)
    {
        int m = (int)std::floor((x + 0.5f) / mpx) % 3;
        for (int c = 0; c < 3; c++) t.gain[x * 3 + c] = c == m ? light : dark;
    }

    t.cx.resize(W);
    t.cy.resize(H);
    for (int x = 0; x < W; x++) t.cx[x] = 2.f * (x + 0.5f) / W - 1.f;
    for (int y = 0; y < H; y++) t.cy[y] = 2.f * (y + 0.5f) / H - 1.f;
}

// stage 1, at DS resolution: YIQ with the luma prefilter, edge-padded for the taps; and the glow
void SoftCrtFilter::dsStage(Layer& L, const uint32_t* src)
{
    const Luts& lut = luts();
    float yRaw[kW];
    for (int y = 0; y < kH; y++)
    {
        const uint32_t* in = src + y * kW;
        float* row = &L.yiq[(size_t)y * kRowW * 3];
        for (int x = 0; x < kW; x++)
        {
            uint32_t c = in[x];
            float r = ((c >> 16) & 255) * (1.f / 255.f), g = ((c >> 8) & 255) * (1.f / 255.f), b = (c & 255) * (1.f / 255.f);
            yRaw[x] = 0.299f * r + 0.587f * g + 0.114f * b;
            row[(x + kPad) * 3 + 1] = 0.596f * r - 0.274f * g - 0.322f * b;
            row[(x + kPad) * 3 + 2] = 0.211f * r - 0.523f * g + 0.312f * b;
        }
        for (int x = 0; x < kW; x++)
            row[(x + kPad) * 3] = 0.25f * (yRaw[std::max(x - 1, 0)] + 2.f * yRaw[x] + yRaw[std::min(x + 1, kW - 1)]);
        // the shader clamps the tap position, so the padding repeats the edge pixels
        for (int x = 0; x < kPad; x++)
            for (int c = 0; c < 3; c++)
            {
                row[x * 3 + c] = row[kPad * 3 + c];
                row[(kPad + kW + x) * 3 + c] = row[(kPad + kW - 1) * 3 + c];
            }
    }

    // glow: 5x5 Gaussian of linear light, done as two 5-tap passes (edges clamped), and
    // multiplied by its strength here so the later stages just add it
    float w[5], ws = 0.f;
    for (int d = -2; d <= 2; d++) ws += w[d + 2] = std::exp(-0.5f * d * d / kGlowVar);
    for (float& v : w) v /= ws;
    std::vector<float>& h = L.glowTmp;
    h.resize((size_t)kW * kH * 3);
    for (int y = 0; y < kH; y++)
    {
        const uint32_t* in = src + y * kW;
        for (int x = 0; x < kW; x++)
        {
            float acc[3] = {0, 0, 0};
            for (int d = -2; d <= 2; d++)
            {
                uint32_t c = in[std::clamp(x + d, 0, kW - 1)];
                acc[0] += lut.lin24[(c >> 16) & 255] * w[d + 2];
                acc[1] += lut.lin24[(c >> 8) & 255] * w[d + 2];
                acc[2] += lut.lin24[c & 255] * w[d + 2];
            }
            for (int c = 0; c < 3; c++) h[((size_t)y * kW + x) * 3 + c] = acc[c];
        }
    }
    const float strength = t.p.glow;
    for (int y = 0; y < kH; y++)
    {
        float* out = &L.glow[(size_t)y * kW * 3];
        for (int i = 0; i < kW * 3; i++) out[i] = 0.f;
        for (int d = -2; d <= 2; d++)
        {
            const float* in = &h[(size_t)std::clamp(y + d, 0, kH - 1) * kW * 3];
            float wd = w[d + 2] * strength;
            for (int i = 0; i < kW * 3; i++) out[i] += in[i] * wd;
        }
    }
}

// stage 2, per DS row: the horizontally blurred signal at every phase column, and the glow
// widened to the same columns (the shader's bilinear glow read, horizontally)
void SoftCrtFilter::rowStage(Layer& L, int y0, int y1)
{
    const int P = t.phases, SW = kW * P;
    const bool curved = t.p.curvature > 0.f;
    for (int y = y0; y < y1; y++)
    {
        const float* row = &L.yiq[(size_t)y * kRowW * 3];
        uint16_t* sig = &L.sig[(size_t)y * SW * 3];
        for (int x = 0; x < kW; x++)
        {
            const float* tap = row + x * 3;   // the DS pixel 4 to the left, padded
            for (int i = 0; i < P; i++)
            {
                const float* wl = &t.tapL[i * kTaps];
                const float* wc = &t.tapC[i * kTaps];
                float Y = 0.f, I = 0.f, Q = 0.f;
                for (int k = 0; k < kTaps; k++)
                {
                    Y += tap[k * 3] * wl[k];
                    I += tap[k * 3 + 1] * wc[k];
                    Q += tap[k * 3 + 2] * wc[k];
                }
                float rgb[3] = {Y + 0.956f * I + 0.621f * Q,
                                Y - 0.272f * I - 0.647f * Q,
                                Y - 1.106f * I + 1.703f * Q};
                uint16_t* o = sig + (x * P + i) * 3;
                if (curved)   // 8-bit levels, already as offsets into the beam table
                    for (int c = 0; c < 3; c++)
                        o[c] = (uint16_t)((int)(std::min(std::max(rgb[c], 0.f), 1.f) * (kBeamLevels - 1) + 0.5f) * (kBeamSteps + 1));
                else
                    for (int c = 0; c < 3; c++)
                        o[c] = (uint16_t)(std::min(std::max(rgb[c], 0.f), 1.f) * (kLevels - 1) + 0.5f);
            }
        }

        const float* g = &L.glow[(size_t)y * kW * 3];
        float* gh = &L.glowH[(size_t)y * SW * 3];
        for (int i = 0; i < P; i++)
        {
            float gx = (i + 0.5f) / P - 0.5f;   // relative to the DS pixel's centre
            int off = gx < 0.f ? -1 : 0;
            float fx = gx - off;
            for (int x = 0; x < kW; x++)
            {
                int xa = std::clamp(x + off, 0, kW - 1), xb = std::clamp(x + off + 1, 0, kW - 1);
                float* o = gh + (x * P + i) * 3;
                for (int c = 0; c < 3; c++) o[c] = g[xa * 3 + c] + (g[xb * 3 + c] - g[xa * 3 + c]) * fx;
            }
        }
    }
}

// stage 3, flat screen, per output row: beams of the two nearest DS rows from the table, plus the
// glow lerped between them; then mask, edge darkening, brightness and encode
void SoftCrtFilter::outStage(Layer& L, int y0, int y1, uint32_t* dst, int stride)
{
    const Luts& lut = luts();
    const int n = t.n, W = kW * n;
    const float vig = t.p.vignette;
    for (int Y = y0; Y < y1; Y++)
    {
        int y = Y / n, j = Y % n;
        float fy = (j + 0.5f) / n - 0.5f;
        int r0 = fy < 0.f ? y - 1 : y;
        float f = fy < 0.f ? fy + 1.f : fy;
        int ra = std::clamp(r0, 0, kH - 1), rb = std::clamp(r0 + 1, 0, kH - 1);
        const float* ba = &t.beam[((size_t)j * 2) * kLevels];
        const float* bb = &t.beam[((size_t)j * 2 + 1) * kLevels];
        const uint16_t* sa = &L.sig[(size_t)ra * W * 3];
        const uint16_t* sb = &L.sig[(size_t)rb * W * 3];
        const float* ga = &L.glowH[(size_t)ra * W * 3];
        const float* gb = &L.glowH[(size_t)rb * W * 3];

        uint32_t* o = dst + (size_t)Y * stride;
        const float cy = t.cy[Y];
        for (int x = 0; x < W; x++)
        {
            float v = vig > 0.f ? vignette(t.cx[x], cy, vig) : 1.f;
            float col[3];
            for (int c = 0; c < 3; c++)
            {
                int i = x * 3 + c;
                col[c] = (ba[sa[i]] + bb[sb[i]] + ga[i] + (gb[i] - ga[i]) * f) * t.gain[i] * v;
            }
            o[x] = encode(lut, col[0], col[1], col[2]);
        }
    }
}

// stage 3, curved screen: each output pixel finds its warped point and works it out there, as
// kCrtFS does. The sideways signal and glow come from the nearest of the 2n phase columns, the
// beams from a table by level and distance. Outside the curved screen is black
void SoftCrtFilter::curvedStage(Layer& L, int y0, int y1, uint32_t* dst, int stride)
{
    const Luts& lut = luts();
    const int n = t.n, W = kW * n, H = kH * n, P = t.phases, SW = kW * P;
    const float wx = t.p.curvature * kWarpX, wy = t.p.curvature * kWarpY, vig = t.p.vignette;
    const float* beamT = t.beam.data();
    for (int Y = y0; Y < y1; Y++)
    {
        uint32_t* o = dst + (size_t)Y * stride;
        const float cy0 = t.cy[Y];
        for (int x = 0; x < W; x++)
        {
            float cx0 = t.cx[x];
            float cx = cx0 * (1.f + cy0 * cy0 * wx);
            float cy = cy0 * (1.f + cx0 * cx0 * wy);
            float u = cx * 0.5f + 0.5f, v = cy * 0.5f + 0.5f;
            // antialiased edge, as kCrtFS: how far inside, in output pixels (one output pixel is
            // about 1/W of u and 1/H of v; the warp changes that by a few percent at most)
            float edge = std::min(std::min(u, 1.f - u) * W, std::min(v, 1.f - v) * H) + 0.5f;
            if (edge <= 0.f) { o[x] = 0xFF000000u; continue; }
            const bool onEdge = edge < 1.f;   // only the outermost pixel of the curve
            if (onEdge) { u = std::clamp(u, 0.f, 1.f); v = std::clamp(v, 0.f, 1.f); }
            float px = u * kW, py = v * kH;

            // signal column: the nearest phase
            int sx = std::min((int)(px * P), SW - 1);
            float yy = py - 0.5f;                 // -0.5 .. 191.5
            int r0 = (int)(yy + 1.f) - 1;         // floor, without a call
            float f = yy - r0;
            int ra = std::clamp(r0, 0, kH - 1), rb = std::clamp(r0 + 1, 0, kH - 1);
            const uint16_t* sa = &L.sig[((size_t)ra * SW + sx) * 3];
            const uint16_t* sb = &L.sig[((size_t)rb * SW + sx) * 3];
            // the two rows are f and 1 - f away: table steps and weights
            float fa = f * kBeamSteps, fb = (1.f - f) * kBeamSteps;
            int ia = std::min((int)fa, kBeamSteps - 1), ib = std::min((int)fb, kBeamSteps - 1);
            float wa = fa - ia, wb = fb - ib;

            // glow: already widened to the phase columns; lerped between the beams' two rows
            const float* g0 = &L.glowH[((size_t)ra * SW + sx) * 3];
            const float* g1 = &L.glowH[((size_t)rb * SW + sx) * 3];

            float vg = vig > 0.f ? vignette(cx, cy, vig) : 1.f;
            float col[3];
            for (int c = 0; c < 3; c++)
            {
                const float* ta = beamT + sa[c] + ia;
                const float* tb = beamT + sb[c] + ib;
                float beam = ta[0] + (ta[1] - ta[0]) * wa + tb[0] + (tb[1] - tb[0]) * wb;
                col[c] = (beam + g0[c] + (g1[c] - g0[c]) * f) * t.gain[x * 3 + c] * vg;
            }
            if (onEdge)   // blended after clamping, as kCrtFS does
                for (float& v : col) v = std::min(v, 1.f) * edge;
            o[x] = encode(lut, col[0], col[1], col[2]);
        }
    }
}

void SoftCrtFilter::process(const uint32_t* const src[2], uint32_t* const dst[2], int dstStride, int n,
                            const SoftCrtParams& p, unsigned layers)
{
    n = std::clamp(n, 1, 16);
    int list[2], nl = 0;
    for (int l = 0; l < 2; l++)
        if (layers & (1u << l)) list[nl++] = l;
    if (!nl) return;

    if (!t.valid || t.n != n || !(t.p == p))
        makeTables(n, p);

    const int H = kH * n;
    const bool curved = p.curvature > 0.f;
    for (int i = 0; i < nl; i++)
    {
        Layer& L = layer[list[i]];
        L.yiq.resize((size_t)kH * kRowW * 3);
        L.glow.resize((size_t)kW * kH * 3);
        L.sig.resize((size_t)kH * kW * t.phases * 3);
        L.glowH.resize((size_t)kH * kW * t.phases * 3);
    }

    const int stride = dstStride / 4;
    const int threads = pool->size();
    // DS-row and output-row stages in bands; several per thread, so one that finishes early
    // takes another
    const int bands = threads <= 1 ? 1 : threads * 2;

    pool->run(nl, [&](int job) { dsStage(layer[list[job]], src[list[job]]); });
    pool->run(nl * bands, [&](int job) {
        int l = list[job / bands], b = job % bands;
        rowStage(layer[l], kH * b / bands, kH * (b + 1) / bands);
    });
    pool->run(nl * bands, [&](int job) {
        int l = list[job / bands], b = job % bands;
        if (curved) curvedStage(layer[l], H * b / bands, H * (b + 1) / bands, dst[l], stride);
        else outStage(layer[l], H * b / bands, H * (b + 1) / bands, dst[l], stride);
    });
}
