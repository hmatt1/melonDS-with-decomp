/*
    NTSC video signal chain: see NtscSignal.h.
*/

#include "NtscSignal.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

namespace Ntsc
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

// NTSC colour axes (SMPTE 170M): U = 0.492 (B'-Y'), V = 0.877 (R'-Y'), in units of white - black
constexpr double kUScale = 0.492111, kVScale = 0.877283;

// carrier frequency, the same for every NTSC console (master / carrier divider)
constexpr double kFsc = 315e6 / 88;   // 3.579545 MHz

// PAL TV and encoder figures (see Chain)
constexpr double kPalTrapHz = 4.43e6;       // CXA1645 trap for PAL (its 68 uH coil with about 19 pF)
constexpr double kPalBandQ = 3.0;           // PAL TV chroma band-pass (Philips TDA8362)
constexpr double kPalDemodHz = 1.0e6;       // PAL U/V low-pass (TDA8362: "approximately 1 MHz")

// RF noise at Settings::noise = 1: white noise with this RMS (of white - black), before the TV
constexpr double kNoiseRms = 0.12;
constexpr int kNoiseLines = 32;

// --- filters ------------------------------------------------------------------------------------

// a second-order section, designed from an analog prototype with the bilinear transform
// (RBJ cookbook), run causally like the analog circuit it stands for
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    double run(double x)
    {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }

    static Biquad make(double b0, double b1, double b2, double a0, double a1, double a2)
    {
        Biquad q;
        q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0;
        return q;
    }
    static Biquad lowpass(double fc, double Q, double fs)
    {
        double w = 2 * kPi * fc / fs, c = std::cos(w), a = std::sin(w) / (2 * Q);
        return make((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a);
    }
    static Biquad bandpass(double fc, double Q, double fs)   // 0 dB at fc
    {
        double w = 2 * kPi * fc / fs, c = std::cos(w), a = std::sin(w) / (2 * Q);
        return make(a, 0, -a, 1 + a, -2 * c, 1 - a);
    }
    static Biquad notch(double fc, double Q, double fs)
    {
        double w = 2 * kPi * fc / fs, c = std::cos(w), a = std::sin(w) / (2 * Q);
        return make(1, -2 * c, 1, 1 + a, -2 * c, 1 - a);
    }
};

struct Filter
{
    std::vector<Biquad> s;
    double run(double x) { for (auto& q : s) x = q.run(x); return x; }
    void reset() { for (auto& q : s) q.reset(); }

    // Butterworth low-pass of order 2 * sections
    static Filter butterLow(double fc, int sections, double fs)
    {
        Filter f;
        int n = 2 * sections;
        for (int k = 0; k < sections; k++)
            f.s.push_back(Biquad::lowpass(fc, 1.0 / (2 * std::cos(kPi * (2 * k + 1) / (2.0 * n))), fs));
        return f;
    }
};

// A board's filter network, from its exact transfer function H(s) = num(s) / den(s) (coefficients
// from s^0 up, any order) through the bilinear transform, prewarped at fWarp; run in transposed
// direct form II in double precision (fine at these orders and clock rates)
struct Iir
{
    std::vector<double> b, a, z;   // a[0] = 1

    double run(double x)
    {
        const size_t n = a.size() - 1;
        if (n == 0) return b[0] * x;
        const double y = b[0] * x + z[0];
        for (size_t i = 1; i < n; i++) z[i - 1] = b[i] * x - a[i] * y + z[i];
        z[n - 1] = b[n] * x - a[n] * y;
        return y;
    }
    void reset() { std::fill(z.begin(), z.end(), 0.0); }

    static Iir analog(std::vector<double> num, std::vector<double> den, double fs, double fWarp)
    {
        const size_t n = std::max(num.size(), den.size()) - 1;
        num.resize(n + 1, 0.0); den.resize(n + 1, 0.0);
        const double K = 2 * kPi * fWarp / std::tan(kPi * fWarp / fs);
        // s^k -> K^k (1 - z^-1)^k (1 + z^-1)^(n - k), as polynomials in z^-1
        auto expand = [&](const std::vector<double>& c)
        {
            std::vector<double> out(n + 1, 0.0);
            for (size_t k = 0; k <= n; k++)
            {
                std::vector<double> p{c[k] * std::pow(K, (double)k)};
                for (size_t i = 0; i < n; i++)
                {
                    const double sgn = i < k ? -1.0 : 1.0;   // (1 - z^-1) k times, then (1 + z^-1)
                    std::vector<double> q(p.size() + 1, 0.0);
                    for (size_t j = 0; j < p.size(); j++) { q[j] += p[j]; q[j + 1] += sgn * p[j]; }
                    p = q;
                }
                for (size_t j = 0; j <= n; j++) out[j] += p[j];
            }
            return out;
        };
        Iir f;
        f.b = expand(num);
        f.a = expand(den);
        const double a0 = f.a[0];
        for (double& v : f.b) v /= a0;
        for (double& v : f.a) v /= a0;
        f.z.assign(std::max<size_t>(n, 1), 0.0);
        return f;
    }
};

// polynomials in s, coefficients from s^0 up, for working out a network's H(s)
using Poly = std::vector<double>;
Poly operator*(const Poly& a, const Poly& b)
{
    Poly r(a.size() + b.size() - 1, 0.0);
    for (size_t i = 0; i < a.size(); i++)
        for (size_t j = 0; j < b.size(); j++) r[i + j] += a[i] * b[j];
    return r;
}
Poly operator+(const Poly& a, const Poly& b)
{
    Poly r(std::max(a.size(), b.size()), 0.0);
    for (size_t i = 0; i < a.size(); i++) r[i] += a[i];
    for (size_t i = 0; i < b.size(); i++) r[i] += b[i];
    return r;
}

// a delay line of whole clocks
struct Delay
{
    std::vector<double> buf;
    size_t pos = 0;
    explicit Delay(int n = 0) : buf(std::max(n, 0) + 1, 0.0) {}
    double run(double x)
    {
        buf[pos] = x;
        pos = (pos + 1) % buf.size();
        return buf[pos];
    }
};

// group delay at (near) DC of a filter, in clocks, measured from its step response's centroid
template <class F>
double groupDelay(F f, int len = 4096)
{
    f.reset();
    double prev = 0, num = 0, den = 0;
    for (int t = 0; t < len; t++)
    {
        double y = f.run(1.0);
        double h = y - prev;   // impulse response
        prev = y;
        num += h * t;
        den += h;
    }
    return den != 0 ? num / den : 0;
}

// deterministic noise: a hash of (line, clock, frame) -> roughly Gaussian, unit RMS
uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
double gauss(uint32_t seed)
{
    // sum of 4 uniforms, scaled to unit variance
    double s = 0;
    for (int i = 0; i < 4; i++) s += (hash32(seed * 4 + i) & 0xFFFFFF) / double(0x1000000);
    return (s - 2.0) * std::sqrt(3.0);
}

// --- consoles -------------------------------------------------------------------------------------

// Timings from hardware documentation:
//  Mega Drive (NTSC): MCLK 53.693175 MHz; carrier MCLK/15; pixels MCLK/8 in H40, MCLK/10 in H32
//    (H40's slower pixels during horizontal sync don't touch the picture); every line 3420 MCLK =
//    228 carrier cycles exactly, so every line and frame starts at the same phase: still artifacts.
//    (SpritesMind "VDP registers timings"; Kabuto's hardware notes; nesdev forum t=24447)
//  SNES (NTSC): master 21.47727 MHz; carrier master/6; pixels master/4; lines 1364 clocks = 227 1/3
//    cycles (a third of a cycle more each line: diagonal artifacts); every other frame has one
//    1360-clock line, so frames start at two phases a third of a cycle apart. (fullsnes "Timing",
//    "Long and Short Scanlines"; SNESdev wiki "Timing")
//  Mega Drive (PAL): MCLK 53.203424 MHz (12 x the PAL carrier); carrier MCLK/12 from the VDP;
//    pixels MCLK/8 or /10; 3420-clock lines = 285 cycles exactly, 313 lines: still artifacts, and the
//    PAL switch (free running in the encoder) flips each frame. (Genesis Plus GX system.c; Kabuto)
//  SNES (PAL): one 17.734475 MHz crystal; master = x6/5 = 21.28137 MHz = 4.8 x the carrier, so 5
//    carrier cycles in 24 clocks; dots master/4; 1364-clock lines = 284 1/6 cycles (+60 degrees a line,
//    a 6-line pattern); 312 lines, no short line, so still from frame to frame. (fullsnes)
//  NES (NTSC 2C02): master 21.47727 MHz; dots master/4; carrier master/6, the PPU's 12-step colour
//    wave stepping on both clock edges; 341-dot lines = 1364 clocks, as the SNES (+1/3 cycle a line);
//    with rendering on, every other frame skips a dot (4 clocks). (nesdev "NTSC video", "PPU frame
//    timing")
//  Mega Drive 1: the same VDP and clocks as the Mega Drive 2.
//  N64 (NTSC): VI clock = 13.6 x the carrier (the 14.318 MHz crystal x 17/5), so 5 cycles in 68
//    clocks; lines 3094 clocks = 227.5 cycles, broadcast NTSC's length, so chroma inverts every line
//    as on broadcast; 263 lines. A 320-wide game's pixels each span two 4-clock output pixels.
//    (n64brew "Video Interface", "Video DAC")
//  PlayStation (NTSC): GPU clock 53.693175 MHz, carrier /15; dots /8 (320 wide) or /10 (256); lines
//    alternate 3412 and 3413 clocks (227.5 cycles on average); 263 lines. PAL: 53.203425 MHz, /12,
//    3405-clock lines (283.75 cycles: no 25 Hz offset), 314 lines. (psx-spx; Mednafen; DuckStation)
//  Saturn (NTSC), 352 wide: system clock 28.636 MHz = 8 x the carrier, dots /4, 455-dot lines =
//    227.5 cycles; 263 lines. (Sega's Saturn overview manual; Mednafen.) Its 320-wide mode (26.874
//    MHz, the crystal x 122/65) repeats its carrier pattern only every 488 clocks, which takes
//    seconds to build a kernel for, so it isn't offered.
const Timing kTimings[Console_Count] =
{
    //  name                 master Hz     pixel carrier line  out  frame clocks (even, odd)               cycles lines/frame sub PAL    source
    {"Off",                 0,              1,  1,  1,    1, {1, 1},                                   1, {262, 262}, 1, false, Source_CXA1645},
    {"Mega Drive (H40)",    53693175.0,     8,  15, 3420, 2, {262LL * 3420, 262LL * 3420},             1, {262, 262}, 1, false, Source_CXA1645},
    {"Mega Drive (H32)",    53693175.0,     10, 15, 3420, 2, {262LL * 3420, 262LL * 3420},             1, {262, 262}, 1, false, Source_CXA1645},
    {"SNES",                21477272.0,     4,  6,  1364, 1, {262LL * 1364, 262LL * 1364 - 4},         1, {262, 262}, 1, false, Source_SEnc},
    {"Mega Drive PAL (H40)", 53203424.0,    8,  12, 3420, 2, {313LL * 3420, 313LL * 3420},             1, {313, 313}, 1, true,  Source_CXA1645},
    {"Mega Drive PAL (H32)", 53203424.0,    10, 12, 3420, 2, {313LL * 3420, 313LL * 3420},             1, {313, 313}, 1, true,  Source_CXA1645},
    {"SNES PAL",            21281370.0,     4,  24, 1364, 1, {312LL * 1364, 312LL * 1364},             5, {312, 312}, 5, true,  Source_SEnc},
    {"NES",                 21477272.0,     4,  6,  1364, 1, {262LL * 1364, 262LL * 1364 - 4},         1, {262, 262}, 2, false, Source_NES},
    {"Mega Drive 1 (H40)",  53693175.0,     8,  15, 3420, 2, {262LL * 3420, 262LL * 3420},             1, {262, 262}, 1, false, Source_CXA1145},
    {"Mega Drive 1 (H32)",  53693175.0,     10, 15, 3420, 2, {262LL * 3420, 262LL * 3420},             1, {262, 262}, 1, false, Source_CXA1145},
    {"Mega Drive 1 PAL (H40)", 53203424.0,  8,  12, 3420, 2, {313LL * 3420, 313LL * 3420},             1, {313, 313}, 1, true,  Source_CXA1145},
    {"Mega Drive 1 PAL (H32)", 53203424.0,  10, 12, 3420, 2, {313LL * 3420, 313LL * 3420},             1, {313, 313}, 1, true,  Source_CXA1145},
    {"N64",                 315e6 / 88 * 13.6, 8, 68, 3094, 2, {263LL * 3094, 263LL * 3094},         5, {263, 263}, 1, false, Source_BA7242},
    {"PlayStation (320)",   53693175.0,     8,  15, 3412, 2, {263LL * 3412 + 131, 263LL * 3412 + 132}, 1, {263, 263}, 1, false, Source_CXA1645, 1},
    {"PlayStation (256)",   53693175.0,     10, 15, 3412, 2, {263LL * 3412 + 131, 263LL * 3412 + 132}, 1, {263, 263}, 1, false, Source_CXA1645, 1},
    {"PlayStation PAL (320)", 53203425.0,   8,  12, 3405, 2, {314LL * 3405, 314LL * 3405},             1, {314, 314}, 1, true,  Source_CXA1645},
    {"Saturn (352)",        315e6 / 88 * 8, 4,  8,  1820, 1, {263LL * 1820, 263LL * 1820},             1, {263, 263}, 1, false, Source_CXA1645},
};
// The NES PPU's output (nesdev "NTSC video"): each palette entry $LH (luma row L, hue H) is a square
// wave between a low and a high level, 12 steps per carrier cycle, high for the 6 steps where
// (H + step) mod 12 < 6. Hue 0 is all high, hues 13 to 15 all low, and hues 14 and 15 use row 1's
// levels (black). The burst is hue 8's wave between its own two levels. Levels in volts into 75 ohms
// (nesdev's measurements, which blargg's earlier ones agree with); normalised here to black 0 ($0F,
// which is also blanking: the NES has no 7.5 IRE setup) and white 1 ($20).
constexpr double kNesLow[4] = {0.228, 0.312, 0.552, 0.880}, kNesHigh[4] = {0.616, 0.840, 1.100, 1.100};
constexpr double kNesBlack = 0.312, kNesWhite = 1.100;
constexpr double kNesBurstLow = 0.148, kNesBurstHigh = 0.524;
constexpr int kNesBlackIndex = 0x0F;
double nesNorm(double v) { return (v - kNesBlack) / (kNesWhite - kNesBlack); }
bool nesHigh(int hue, int step) { return (hue + step) % 12 < 6; }
double nesLevel(int index, int step)
{
    const int hue = index & 15;
    int row = (index >> 4) & 3;
    if (hue >= 14) row = 1;
    double lo = kNesLow[row], hi = kNesHigh[row];
    if (hue == 0) lo = hi;
    if (hue >= 13) hi = lo;
    return nesNorm(nesHigh(hue, step) ? hi : lo);
}
double nesBurst(int step) { return nesNorm(nesHigh(8, step) ? kNesBurstHigh : kNesBurstLow); }

// a 2nd-order band-pass's delay to the envelope of a signal at its centre frequency, in clocks
double bandpassEnvelopeDelay(double Q, double fc, double fs) { return 2 * Q / (2 * kPi * fc) * fs; }

} // namespace


const Timing& timing(int console) { return kTimings[std::clamp(console, 0, Console_Count - 1)]; }

bool isPal(int console) { return timing(console).pal; }

int decoderFor(int console, int decoder)
{
    const bool palDecoder = decoder == Decoder_PalD || decoder == Decoder_PalS || decoder == Decoder_PalDGlass;
    if (isPal(console)) return palDecoder ? decoder : Decoder_PalD;
    return palDecoder ? Decoder_Standard : decoder;
}

int cableFor(int console, int cable)
{
    const int src = timing(console).source;
    if (src == Source_NES && cable != Cable_RF) return Cable_Composite;
    if (src == Source_CXA1145 && cable == Cable_SVideo) return Cable_Composite;
    return cable;
}

namespace
{
// where a row starts in the carrier pattern (master clocks, mod carrierClocks)
int rowClockPhase(const Timing& tm, int frameClock, int row, uint32_t frame)
{
    return (int)((frameClock + tm.rowClocks(row, frame)) % tm.carrierClocks);
}

// the comb filter's delay line: 227.5 carrier cycles, broadcast NTSC's line (see Chain)
constexpr double kCombCycles = 227.5;

// how many DS rows each output depends on: two with a delay-line TV (PAL-D, or the NTSC comb),
// which holds a line
int rowsFor(const Settings& s)
{
    const int d = decoderFor(s.console, s.decoder);
    if (cableFor(s.console, s.cable) == Cable_RGB) return 1;
    return d == Decoder_PalD || d == Decoder_PalDGlass || d == Decoder_Comb ? 2 : 1;
}
// simulation clocks per master clock. The NES's colour wave steps on both edges of its clock, so it
// runs at twice the master clock; otherwise finer only where a fixed delay line isn't a whole number
// of master clocks (the 1980s PAL glass line's 283.5 cycles on a SNES: 1360.8 clocks; the comb's
// 227.5 cycles on a Mega Drive: 3412.5)
int simSub(const Settings& s)
{
    const Timing& tm = timing(s.console);
    if (tm.source == Source_NES) return 2;
    if (cableFor(s.console, s.cable) == Cable_RGB) return 1;
    const int d = decoderFor(s.console, s.decoder);
    if (d == Decoder_PalDGlass) return tm.sub;
    if (d == Decoder_Comb)
    {
        const double clocks = kCombCycles * tm.carrierClocks / tm.carrierCycles;
        return std::fabs(clocks - std::round(clocks)) < 1e-9 ? 1 : 2;
    }
    return 1;
}
// the glass delay line: 283.5 carrier cycles (63.943 us at 4.43 MHz), in simulation clocks
int palDelayClocks(const Timing& tm, int sub)
{
    return (int)std::lround(283.5 * tm.carrierClocks / tm.carrierCycles * sub);
}
}


// The filters of one signal path: the console's encoder, the cable and the TV.
//
// Encoders:
//  Mega Drive 2: Sony CXA1645 (datasheet): standard U/V axes and Rec.601 luma; no colour-difference
//    low-pass before the modulator, the modulated chroma goes through an internal band-pass (its
//    width isn't given: chosen to pass the standard's 1.3 MHz chroma, Q 1.1 at NTSC's carrier);
//    an internal Y delay line lines luma up with chroma; the composite output's luma goes through
//    the datasheet's recommended trap for the standard (not on the S-Video luma).
//    The PlayStation's early boards and most Saturns use the same chip (with the datasheet's
//    recommended 68 uH / 28 pF trap, 3.65 MHz).
//  SNES: S-ENC (Rohm BA6592F type): colour-difference filters are board parts whose values aren't
//    documented, so it gets SMPTE 170M's template (under 2 dB down at 1.3 MHz, 20 dB down by
//    3.6 MHz: 4th-order Linkwitz-Riley at 2.0 MHz, no overshoot to speak of); no luma trap.
//  Mega Drive 1: Sony CXA1145, with no filters inside; Sega's board networks (see Chain) instead of
//    the datasheet's delay line and ceramic band-pass.
//  N64: Rohm BA7242F (ENC-NUS, the first boards): no filters in its datasheet. (Later boards'
//    MAV-NUS has no published datasheet.)
//  NES: no encoder: the PPU makes the composite wave itself (see nesLevel).
// TV:
//  NTSC (consumer set): luma through a 3.58 MHz trap, chroma through a 3.58 +-0.5 MHz band-pass
//    (US4980737's description of the usual design), synchronous demodulation against the burst,
//    then the colour-difference axes of the chosen decoder (Sony CXA2025AS figures: R-Y 112 deg /
//    0.83 and G-Y 252 deg / 0.30 for US sets, 95 deg / 0.78 and 240 deg / 0.30 for Japanese).
//  NTSC comb (RCA's CCD comb, RCA Review March 1980): a delay line clocked at 3x the carrier from
//    the burst-locked oscillator, 682.5 cells = 227.5 cycles: broadcast NTSC's line, and whatever
//    the console's line length, the delayed carrier comes out inverted. Luma is half the sum
//    through a 4 MHz low-pass (the CCD clock), plus the half difference's content below 1 MHz
//    (vertical detail); chroma is the half difference through the band-pass. Consoles whose lines
//    aren't 227.5 cycles (Mega Drive 228, SNES and NES 227 1/3) get the line above shifted a
//    little sideways in the comb, so it separates them less cleanly. (Filter orders aren't given:
//    2nd-order Butterworth.)
//  PAL (Philips TDA8362, 1994): luma trap at the carrier (Q 2), chroma band-pass (Q 3),
//    demodulation against the burst on the standard U / V axes with V's sign following the line
//    switch, U / V low-pass about 1 MHz. PAL-D adds a delay line, which cancels phase errors:
//    1990s sets average the demodulated U and V of this line and the previous one, the delay
//    locked to the incoming line (TDA4661 / TDA4665, so exactly one line even for these consoles'
//    longer lines); 1980s sets sum and subtract the chroma signal and itself delayed in glass by
//    63.943 us (283.5 cycles, so its carrier comes out inverted). That fixed delay is shorter than
//    these consoles' lines, so the previous line's colour lands a little to the right: 2.25 pixels
//    on a Mega Drive in 320-wide mode, 0.8 on a SNES.
// The absolute carrier phase of the first pixel isn't documented; it only moves the hue of the
// artifact colours, and is 0 here.
struct Signal::Chain
{
    double fs, fsc;
    int sub = 1;
    bool nes = false;                                   // the NES's own composite wave, no encoder
    bool pal = false, palD = false, palGlass = false;   // PAL-D: either kind; palGlass: the 1980s kind
    bool comb = false;                                  // NTSC 2-line comb filter
    int combDelay = 0;
    // encoder
    bool encUV = false, encBand = false, encTrap = false;
    Filter encU, encV, encBP, encTrapF;
    bool encNet = false;            // board networks on the encoder's luma and chroma (Mega Drive 1)
    Iir encLumaNet, encChromaNet;
    bool viResample = false;        // N64: each pixel shown over two output pixels, the second interpolated
    int encLumaDelay = 0;
    // TV comb filter (RCA's CCD comb): luma low-pass, and the chroma difference's low end put back in luma
    Filter combLow, combDetail;
    // RF channel
    Filter rfChannel;
    int ghostDelay = 0;
    double ghost = 0;
    // TV
    Filter tvTrap, tvBand, tvDemodU, tvDemodV, tvPeak;
    int lumaDelay = 0;      // the TV's luma delay line, to line luma up with chroma
    int outDelay = 0;       // the whole luma path's delay, taken out so the picture doesn't move
    int palDelay = 0;       // PAL-D's delay line

    Chain(const Settings& s)
    {
        const Timing& tm = timing(s.console);
        sub = simSub(s);
        fs = tm.masterHz * sub;
        fsc = tm.fsc();
        pal = tm.pal;
        const int decoder = decoderFor(s.console, s.decoder);
        palD = pal && (decoder == Decoder_PalD || decoder == Decoder_PalDGlass);
        palGlass = pal && decoder == Decoder_PalDGlass;
        const bool composite = s.cable == Cable_Composite || s.cable == Cable_RF;
        const bool snes = tm.source == Source_SEnc;
        nes = tm.source == Source_NES;
        comb = !pal && decoder == Decoder_Comb && composite;

        double encChromaDelay = 0, encLumaPath = 0;
        if (nes)
        {
            // the PPU drives the video output itself (through a buffer transistor): no filters
        }
        else if (tm.source == Source_BA7242)
        {
            // N64 (ENC-NUS = Rohm BA7242F): no colour-difference low-pass, chroma band-pass or luma
            // trap in the datasheet; the VI resamples each 320-wide line to 640 output pixels
            viResample = true;
        }
        else if (tm.source == Source_CXA1145)
        {
            // Mega Drive 1: the CXA1145 has no filters of its own; Sega's board puts coil networks
            // where Sony's application circuit has a delay line and a ceramic band-pass (values from
            // Sega's Genesis II CXA1145 schematic; PAL from a model 1 PAL board, whose band-pass has
            // the extra series arm Sega's Mega Drive II PAL column also shows)
            encNet = true;
            // luma: 1.2k, then 100 uH || 18 pF (12 pF PAL) in series, into 180 pF || 1k (a notch at
            // 3.75 MHz, 4.59 PAL, and a soft roll-off); scaled to unity at DC
            const double R1 = 1200, Lt = 100e-6, Ct = pal ? 12e-12 : 18e-12, C2 = 180e-12, R2 = 1000;
            Poly num{R2, 0, R2 * Lt * Ct};
            Poly den{R1 + R2, R1 * R2 * C2 + Lt, R1 * Lt * Ct + Lt * R2 * C2 + R2 * Lt * Ct, R1 * Lt * Ct * R2 * C2};
            for (double& c : num) c *= (R1 + R2) / R2;
            encLumaNet = Iir::analog(num, den, fs, fsc);
            if (!pal)
            {
                // chroma: 1.2k into a shunt tank, 12 uH || 180 pF || 330 ohms (Q about 1, 3.42 MHz)
                const double R = 1200, L = 12e-6, C = 180e-12, Rp = 330;
                encChromaNet = Iir::analog({0, L}, {R, L * (1 + R / Rp), R * L * C}, fs, fsc);
            }
            else
            {
                // PAL: 1.2k into 12 uH || 100 pF, then 12 pF + 100 uH in series into 1k
                const double R = 1200, L5 = 12e-6, C5 = 100e-12, Cs = 12e-12, Ls = 100e-6, Ra = 1000;
                const Poly q{1, Ra * Cs, Ls * Cs};
                const Poly den = q * Poly{R, L5, R * L5 * C5} + Poly{0, 0, R * Cs * L5};
                encChromaNet = Iir::analog({0, 0, Ra * Cs * L5}, den, fs, fsc);
            }
            // no delay line: the networks' own delays are what the picture gets
            encLumaPath = groupDelay(encLumaNet);
            encChromaDelay = encLumaPath;
        }
        else if (snes)
        {
            encUV = true;
            encU = Filter::butterLow(2.0e6, 1, fs); encU.s.push_back(encU.s[0]);   // LR4 = Butterworth 2nd order, twice
            encV = encU;
            encChromaDelay = groupDelay(encU);
        }
        else   // Mega Drive 2, CXA1645
        {
            encBand = true;
            const double q = 1.1;   // its FO resistor moves the whole filter with the carrier (20 k NTSC, 16 k PAL)
            encBP.s.push_back(Biquad::bandpass(fsc, q, fs));
            encChromaDelay = bandpassEnvelopeDelay(q, fsc, fs);
            if (composite)
            {
                encTrap = true;
                encTrapF.s.push_back(Biquad::notch(pal ? kPalTrapHz : 3.65e6, 1.0, fs));
                encLumaPath = groupDelay(encTrapF);
            }
        }
        encLumaDelay = std::max(0, (int)std::lround(encChromaDelay - encLumaPath));

        // RF: the TV tuner/IF passes the system's video bandwidth
        rfChannel = Filter::butterLow(pal ? 5.0e6 : 4.2e6, 2, fs);
        ghost = s.ghost;
        ghostDelay = (int)std::lround(s.ghostDelay * 1e-6 * fs);

        // TV decoder
        const double bandQ = pal ? kPalBandQ : fsc / 1.0e6;   // NTSC: +-0.5 MHz
        tvTrap.s.push_back(Biquad::notch(fsc, 2.0, fs));
        tvBand.s.push_back(Biquad::bandpass(fsc, bandQ, fs));
        tvDemodU = Filter::butterLow(pal ? kPalDemodHz : 1.0e6, 1, fs);
        tvDemodV = tvDemodU;
        tvPeak = Filter::butterLow(2.6e6, 1, fs);
        if (palGlass) palDelay = palDelayClocks(tm, sub);
        else if (palD) palDelay = tm.lineClocks * sub;   // one incoming line
        if (comb)
        {
            combDelay = (int)std::lround(kCombCycles * tm.carrierClocks / tm.carrierCycles * sub);
            combLow = Filter::butterLow(4.0e6, 1, fs);      // removes the CCD's clock
            combDetail = Filter::butterLow(1.0e6, 1, fs);   // restores vertical detail
        }

        // the TV lines its luma up with its chroma (its delay line); then the luma path's whole
        // delay is taken out so the picture doesn't move
        double tvChroma = bandpassEnvelopeDelay(bandQ, fsc, fs) + groupDelay(tvDemodU);
        double tvLuma = comb ? groupDelay(combLow) : composite ? groupDelay(tvTrap) : 0.0;
        lumaDelay = std::max(0, (int)std::lround(tvChroma - tvLuma));
        outDelay = encLumaDelay + (int)std::lround(encLumaPath) + (int)std::lround(tvLuma) + lumaDelay;
        if (s.cable == Cable_RF) outDelay += (int)std::lround(groupDelay(rfChannel));

        measureBurst(s, tm);
    }

    // The TV demodulates against the colour burst, which went through the same filters as the
    // picture's chroma (encoder band-pass, RF channel, ghost, TV band-pass): so it locks to the
    // burst's phase as received, and its colour control (ACC) scales chroma by the burst's received
    // amplitude. Measured here by sending a carrier-frequency sine through those filters.
    double burstPhase = 0, burstGain = 1;
    void measureBurst(const Settings& s, const Timing& tm)
    {
        Filter bp = encBP, ch = rfChannel, band = tvBand;
        Iir net = encChromaNet;
        bp.reset(); ch.reset(); band.reset();
        if (encNet) net.reset();
        Delay gl(ghostDelay);
        const double w = 2 * kPi * tm.carrierCycles / ((double)tm.carrierClocks * sub);
        const int settle = (int)(20e-6 * fs), n = tm.carrierClocks * sub * 400;
        double si = 0, co = 0;
        const int period = tm.carrierClocks * sub;
        for (int t = 0; t < settle + n; t++)
        {
            double ph = w * t;
            // the burst sits opposite +U (B-Y): sent here as +U. The NES's is its hue 8 square wave,
            // which the TV's ACC then brings to the standard 40 IRE (20 IRE amplitude)
            double x = nes ? -(nesBurst(t % period) - 0.5 * (nesNorm(kNesBurstLow) + nesNorm(kNesBurstHigh)))
                           : std::sin(ph);
            if (encBand) x = bp.run(x);
            if (encNet) x = net.run(x);
            if (s.cable == Cable_RF)
            {
                if (ghostDelay > 0) x = x + ghost * gl.run(x);
                x = ch.run(x);
            }
            double y = band.run(x);
            if (t >= settle) { si += y * std::sin(ph); co += y * std::cos(ph); }
        }
        // y = A sin(ph - p): si = n A cos(p) / 2, co = -n A sin(p) / 2
        burstPhase = std::atan2(-co, si);
        burstGain = 2 * std::sqrt(si * si + co * co) / n;
        if (nes) burstGain /= 0.2;   // ACC: this burst decodes as the standard one
        if (burstGain < 1e-6) burstGain = 1;
    }
};


// One line (and, for a PAL-D TV, the line before it) through the chain, sample by sample, up to the
// TV's demodulators. out: per output sample the four signals the TV's controls then combine (see
// outputMatrix): luma, its high-pass for the sharpness control, and U and V as demodulated.
// lineStartPhase: master clocks into the carrier pattern the line starts at; lineSwitch: PAL's
// line switch (V's sign is negative when set); noiseIn: RF noise per simulation clock, or null.
// Not for the RGB cable, which has no chain.
void Signal::runChain(const float* rgb, const float* prev, float* out, int lineStartPhase, int lineSwitch,
                      const float* noiseIn) const
{
    const Timing& tm = timing(set.console);
    const int pc = tm.pixelClocks, step = tm.outStep, sub = simSub(set);
    const int c0 = step / 2;
    const int W = kWidth * opp;

    Chain ch(set);
    const double fs = ch.fs;
    const int A = (int)(12e-6 * fs);                  // where the picture starts in a line
    const int active = kWidth * pc * sub;
    const int lineLen = tm.lineClocks * sub;
    const bool twoLines = ch.palD || ch.comb;
    const int curStart = twoLines ? lineLen : 0;      // the current line, after the previous one
    const int total = curStart + A + active + (int)(12e-6 * fs) + ch.outDelay;
    const double w = 2 * kPi * tm.carrierCycles / ((double)tm.carrierClocks * sub);
    const double ph0 = 2 * kPi * tm.carrierCycles * (double)lineStartPhase / tm.carrierClocks;
    const int period = tm.carrierClocks * sub;        // NES: the 12 steps of its colour wave
    Delay encLumaLine(ch.encLumaDelay), lumaLine(ch.lumaDelay), ghostLine(ch.ghostDelay), combLine(ch.combDelay);
    Delay palLine(ch.palGlass ? ch.palDelay : 0), palU(ch.palGlass ? 0 : ch.palDelay), palV(ch.palGlass ? 0 : ch.palDelay);
    const double curSign = ch.pal && lineSwitch ? -1.0 : 1.0;

    for (int t = 0; t < total; t++)
    {
        // console: the pixel this clock belongs to (black outside the picture), held by the DAC
        const bool inPrev = twoLines && t < curStart;
        const float* row = inPrev ? prev : rgb;
        const double vSign = inPrev && ch.pal ? -curSign : curSign;   // PAL's switch flips every line
        double R = 0, G = 0, B = 0;
        int nesIndex = kNesBlackIndex;
        int ta = t - (inPrev ? 0 : curStart) - A;
        const int half = pc / 2 * sub;
        if (row && ch.viResample && ta >= -half && ta < active)
        {
            // N64 VI at X scale 0.5: output pixel j (half a DS pixel long) shows framebuffer
            // position j / 2, odd ones interpolated between the pixels either side. The DS picture
            // sits inside the N64's wider one, black around it, so the half-pixel just left of it
            // (j = -1) is still picture: black blended with pixel 0
            const int j = ta >= 0 ? ta / half : -1, x = j >= 0 ? j / 2 : -1;
            auto px = [&](int i, int c) { return i >= 0 && i < kWidth ? (double)row[i * 3 + c] : 0.0; };
            if (j & 1)
            {
                R = 0.5 * (px(x, 0) + px(x + 1, 0)); G = 0.5 * (px(x, 1) + px(x + 1, 1)); B = 0.5 * (px(x, 2) + px(x + 1, 2));
            }
            else { R = px(x, 0); G = px(x, 1); B = px(x, 2); }
        }
        else if (row && !ch.viResample && ta >= 0 && ta < active)
        {
            if (ch.nes) nesIndex = (int)row[ta / (pc * sub)] & 63;
            else
            {
                const float* px = row + (ta / (pc * sub)) * 3;
                R = px[0]; G = px[1]; B = px[2];
            }
        }
        // encoder
        double Y = 0.299 * R + 0.587 * G + 0.114 * B;
        double U = kUScale * (B - Y), V = kVScale * (R - Y);
        if (ch.encUV) { U = ch.encU.run(U); V = ch.encV.run(V); }
        double ph = w * (t - curStart - A) + ph0;   // phase ph0 at the line's first pixel
        double sn = std::sin(ph), cs = std::cos(ph);
        double C = U * sn + vSign * V * cs;
        if (ch.nes)
        {
            // the PPU's wave: the step of its 12 is the carrier phase in half clocks
            int step = (t - curStart - A + lineStartPhase * sub) % period;
            if (step < 0) step += period;
            Y = nesLevel(nesIndex, step);
            C = 0;
        }
        if (ch.encBand) C = ch.encBP.run(C);
        if (ch.encNet) { C = ch.encChromaNet.run(C); Y = ch.encLumaNet.run(Y); }
        Y = encLumaLine.run(Y);
        double Yc = ch.encTrap ? ch.encTrapF.run(Y) : Y;   // the composite output's trapped luma

        // cable
        double yIn, cIn;
        if (set.cable == Cable_SVideo) { yIn = Y; cIn = C; }
        else
        {
            double s = Yc + C;
            if (set.cable == Cable_RF)
            {
                if (ch.ghostDelay > 0) s = s + ch.ghost * ghostLine.run(s);
                if (noiseIn) s += noiseIn[t];
                s = ch.rfChannel.run(s);
            }
            yIn = s; cIn = s;
        }

        // TV
        double yd, cd;
        if (ch.comb)
        {
            // RCA's CCD comb: with the signal a broadcast line ago (227.5 cycles, so its carrier
            // comes out inverted), half the sum is luma and half the difference chroma. Chroma goes
            // through the band-pass; the difference's low end (vertical detail, which the sum lost)
            // goes back into luma, and luma through a low-pass that removes the CCD's clock
            const double back = combLine.run(cIn);
            const double diff = 0.5 * (cIn - back);
            cd = ch.tvBand.run(diff);
            yd = ch.combLow.run(0.5 * (cIn + back) + ch.combDetail.run(diff));
        }
        else
        {
            yd = set.cable == Cable_SVideo ? yIn : ch.tvTrap.run(yIn);
            cd = ch.tvBand.run(cIn);
        }
        yd = lumaLine.run(yd);
        const double yPeak = yd - ch.tvPeak.run(yd);   // what the sharpness control adds, per unit
        // demodulate against the burst as received (phase and amplitude); PAL takes V's sign from
        // the line switch, and PAL-D first separates U and V with the delay line
        double phr = ph - ch.burstPhase;
        double cu = 2 * cd, cv = 2 * cd;
        if (ch.palGlass)
        {
            double del = palLine.run(cd);   // the previous line, its carrier inverted
            cu = cd - del;
            cv = cd + del;
        }
        double u = ch.tvDemodU.run(cu * std::sin(phr)) / ch.burstGain;
        double v = ch.tvDemodV.run(cv * std::cos(phr)) / ch.burstGain * vSign;
        if (ch.palD && !ch.palGlass)
        {
            // the 1990s delay line: this line's U and V averaged with the previous line's
            u = 0.5 * (u + palU.run(u));
            v = 0.5 * (v + palV.run(v));
        }

        // sample the current line where each output sits, the luma path's delay taken out
        int to = t - curStart - A - ch.outDelay - c0 * sub;
        if (to >= 0 && to % (step * sub) == 0 && to / (step * sub) < W)
        {
            int o = to / (step * sub);
            out[o * 4 + 0] = (float)yd; out[o * 4 + 1] = (float)yPeak;
            out[o * 4 + 2] = (float)u;  out[o * 4 + 3] = (float)v;
        }
    }
}

// The TV's controls and colour decoding, after the demodulators: R'G'B' from luma, luma's
// high-pass, U and V. All linear, so this 3x4 matrix is applied to the chain's outputs (and its
// kernel) without simulating again when only these settings change.
void Signal::outputMatrix(float m[3][4]) const
{
    const double tr = set.tint * kPi / 180.0, ct = std::cos(tr), st = std::sin(tr), col = set.colour;
    // uu = a1 u + a2 v, vv = b1 u + b2 v (tint turns, colour scales)
    const double a1 = ct * col, a2 = -st * col, b1 = st * col, b2 = ct * col;
    const double sh = set.sharpness;
    double r[4], g[4], b[4];
    b[0] = 1; b[1] = sh; b[2] = a1 / kUScale; b[3] = a2 / kUScale;
    const int decoder = decoderFor(set.console, set.decoder);
    if (decoder == Decoder_SonyUS || decoder == Decoder_SonyJP)
    {
        // a decoder IC's own axes: each colour difference demodulated at its angle and gain
        // against B-Y (Sony CXA2025AS figures)
        const bool us = decoder == Decoder_SonyUS;
        const double axR = (us ? 112 : 95) * kPi / 180, gR = us ? 0.83 : 0.78;
        const double axG = (us ? 252 : 240) * kPi / 180, gG = 0.30;
        auto axis = [&](double ax, double gain, double* o)
        {
            o[0] = 1; o[1] = sh;
            o[2] = gain * (a1 * std::cos(ax) + b1 * std::sin(ax)) / kUScale;
            o[3] = gain * (a2 * std::cos(ax) + b2 * std::sin(ax)) / kUScale;
        };
        axis(axR, gR, r);
        axis(axG, gG, g);
    }
    else
    {
        r[0] = 1; r[1] = sh; r[2] = b1 / kVScale; r[3] = b2 / kVScale;
        // g = (y' - 0.299 r - 0.114 b) / 0.587: luma terms come out at 1 and sh
        g[0] = 1; g[1] = sh;
        for (int i = 2; i < 4; i++) g[i] = -(0.299 * r[i] + 0.114 * b[i]) / 0.587;
    }
    for (int i = 0; i < 4; i++) { m[0][i] = (float)r[i]; m[1][i] = (float)g[i]; m[2][i] = (float)b[i]; }
}


namespace
{
// the settings the simulation depends on: the TV's controls and NTSC decoder axes only feed
// outputMatrix, and the noise level only scales the noise lines
Settings chainKey(const Settings& s)
{
    Settings k = s;
    k.colour = 1.f; k.tint = 0.f; k.sharpness = 0.f; k.noise = 0.f;
    const int d = decoderFor(s.console, s.decoder);
    k.decoder = isPal(s.console) || d == Decoder_Comb ? d : Decoder_Standard;
    if (s.cable != Cable_RF) { k.ghost = 0.f; k.ghostDelay = 1.f; }
    return k;
}
}

void Signal::build(const Settings& in)
{
    Settings s = in;
    s.cable = cableFor(s.console, s.cable);
    const Settings old = set;
    const bool fresh = !built;
    set = s;
    built = true;
    if (!fresh && chainKey(s) == chainKey(old))
    {
        // the same signal: only the TV's controls (or the noise level) changed
        Settings a = s, b = old;
        a.noise = b.noise = 0.f;
        if (a == b) return;   // the noise level is applied when decoding
        finalize();
        gen++;
        return;
    }
    gen++;
    buildChain();
    finalize();
}

// Simulates the chain into the raw kernel: per output phase, the four TV signals' response to each
// nearby pixel of this row (and, PAL-D, the row above), per input channel
void Signal::buildChain()
{
    rawKernel.clear();
    rawNoise.clear();
    nNoise = 0;
    nRows = 1;
    nIn = 3;
    for (float& b : rawBase) b = 0.f;
    if (!active() || set.cable == Cable_RGB)
    {
        const Timing& tm = timing(set.console);
        opp = active() ? tm.pixelClocks / tm.outStep : 1;
        off0 = active() ? (tm.outStep / 2 + 0.5f) / tm.pixelClocks : 0.5f;
        nLinePhase = nOutPhase = nTaps = 1;
        tapL = 0;
        return;
    }

    const Timing& tm = timing(set.console);
    const int pc = tm.pixelClocks, cc = tm.carrierClocks, step = tm.outStep;
    opp = pc / step;
    off0 = (step / 2 + 0.5f) / pc;
    const int period = std::lcm(pc, cc);    // pixels and carrier line up again after this
    nOutPhase = period / step;
    const int pixPeriod = period / pc;
    const int W = kWidth * opp;
    nRows = rowsFor(set);

    // the line states that occur: carrier phase at the start, and the PAL switch
    std::set<std::pair<int, int>> states;
    for (uint32_t f = 0; f < (uint32_t)cc * 4; f++)
        for (int r = 0; r < 192; r++)
            states.insert({rowClockPhase(tm, frameClockPhase(f), r, f), tm.pal ? (frameSwitch(f) + r) & 1 : 0});
    linePhaseClock.clear();
    linePhaseSwitch.clear();
    linePhaseOf.assign(2 * cc, 0);
    for (const auto& st : states)
    {
        linePhaseOf[2 * st.first + st.second] = (int)linePhaseClock.size();
        linePhaseClock.push_back(st.first);
        linePhaseSwitch.push_back(st.second);
    }
    nLinePhase = (int)linePhaseClock.size();

    // impulses through the chain: one pixel of one input (R, G or B at 1; NES: one palette entry)
    // on black, less black
    const bool nes = tm.source == Source_NES;
    nIn = nes ? 64 : 3;
    const int perTap = 4 * nIn;
    const int span = 48;                         // pixels either side considered
    const int nAll = 2 * span + 1;
    auto fullAt = [&](int L, int op, int rr, int k) { return (((size_t)L * nOutPhase + op) * nRows + rr) * nAll + k; };
    std::vector<float> full((size_t)nLinePhase * nOutPhase * nRows * nAll * perTap, 0.f);
    // a black row: R'G'B' 0, or the NES's black entry
    std::vector<float> zero(kWidth * 3, nes ? (float)kNesBlackIndex : 0.f), blackOut(W * 4), resp(W * 4), in(kWidth * 3);
    const int p0base = (kWidth / 2) / pixPeriod * pixPeriod;
    for (int L = 0; L < nLinePhase; L++)
    {
        runChain(zero.data(), zero.data(), blackOut.data(), linePhaseClock[L], linePhaseSwitch[L], nullptr);
        if (L == 0) for (int c = 0; c < 4; c++) rawBase[c] = blackOut[(W / 2) * 4 + c];
        for (int rr = 0; rr < nRows; rr++)
            for (int q = 0; q < pixPeriod; q++)
                for (int ci = 0; ci < nIn; ci++)
                {
                    if (nes && ci == kNesBlackIndex) continue;   // black on black: nothing
                    const int p0 = p0base + q;
                    std::copy(zero.begin(), zero.end(), in.begin());
                    if (nes) in[p0] = (float)ci;
                    else in[p0 * 3 + ci] = 1.f;
                    if (rr == 0) runChain(in.data(), zero.data(), resp.data(), linePhaseClock[L], linePhaseSwitch[L], nullptr);
                    else runChain(zero.data(), in.data(), resp.data(), linePhaseClock[L], linePhaseSwitch[L], nullptr);
                    for (int o = 0; o < W; o++)
                    {
                        int p = (o * step + step / 2) / pc;
                        int k = p0 - p;
                        if (k < -span || k > span) continue;
                        int op = o % nOutPhase;
                        for (int co = 0; co < 4; co++)
                            full[fullAt(L, op, rr, k + span) * perTap + co * nIn + ci] = resp[o * 4 + co] - blackOut[o * 4 + co];
                    }
                }
    }

    // keep the taps that matter: a tap whose every weight is under this changes no output by a
    // visible amount (all the dropped taps together stay under a quarter of an 8-bit step)
    int lo = nAll, hi = -1;
    for (int k = 0; k < nAll; k++)
    {
        float m = 0;
        for (int L = 0; L < nLinePhase; L++)
            for (int op = 0; op < nOutPhase; op++)
                for (int rr = 0; rr < nRows; rr++)
                    for (int j = 0; j < perTap; j++)
                        m = std::max(m, std::fabs(full[fullAt(L, op, rr, k) * perTap + j]));
        if (m > 4e-5f) { lo = std::min(lo, k); hi = std::max(hi, k); }
    }
    if (hi < lo) { lo = hi = span; }
    nTaps = hi - lo + 1;
    tapL = span - lo;
    rawKernel.assign((size_t)nLinePhase * nOutPhase * nRows * nTaps * perTap, 0.f);
    for (int L = 0; L < nLinePhase; L++)
        for (int op = 0; op < nOutPhase; op++)
            for (int rr = 0; rr < nRows; rr++)
                for (int k = 0; k < nTaps; k++)
                    for (int j = 0; j < perTap; j++)
                        rawKernel[((((size_t)L * nOutPhase + op) * nRows + rr) * nTaps + k) * perTap + j] = full[fullAt(L, op, rr, k + lo) * perTap + j];

    // decoded noise lines (the noise through the TV), for RF. White noise at the simulation rate,
    // scaled so its density per MHz is the same whatever the console's clock
    if (set.cable == Cable_RF)
    {
        const int sub = simSub(set);
        const double fsim = tm.masterHz * sub;
        const double rms = kNoiseRms * std::sqrt(fsim / 21477272.0);
        std::vector<float> nz((size_t)(3 * tm.lineClocks + 4096) * sub);
        nNoise = kNoiseLines;
        rawNoise.assign((size_t)nNoise * W * 4, 0.f);
        for (int l = 0; l < nNoise; l++)
        {
            for (size_t t = 0; t < nz.size(); t++) nz[t] = (float)(gauss((uint32_t)(l * 1000003u + t)) * rms);
            runChain(zero.data(), zero.data(), resp.data(), linePhaseClock[0], linePhaseSwitch[0], nz.data());
            for (int i = 0; i < W * 4; i++) rawNoise[(size_t)l * W * 4 + i] = resp[i] - blackOut[i];
        }
    }
}

// The final kernel, bias and noise lines in R'G'B': the raw ones through outputMatrix
void Signal::finalize()
{
    kernel.clear();
    kernel4.clear();
    noiseData.clear();
    if (rawKernel.empty())
    {
        base[0] = base[1] = base[2] = 0.f;
        // RGB: no encoding, so each output is its pixel as it is. One tap of identity weights, so
        // the OpenGL decode pass (which always reads the kernel) gets the same as decodeLine
        if (active())
        {
            kernel = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            kernel4 = {V4{1, 0, 0, 0}, V4{0, 1, 0, 0}, V4{0, 0, 1, 0}};
        }
        return;
    }
    float m[3][4];
    outputMatrix(m);
    kernelIdx.clear();
    if (nIn != 3)
    {
        // NES: per tap and palette entry, the R'G'B' it adds
        const size_t taps = rawKernel.size() / (4 * nIn);
        kernel.assign(taps * nIn * 3, 0.f);
        kernelIdx.assign(taps * nIn, V4{});
        for (size_t e = 0; e < taps; e++)
            for (int i = 0; i < nIn; i++)
            {
                V4 o{};
                for (int co = 0; co < 3; co++)
                {
                    float v = 0.f;
                    for (int j = 0; j < 4; j++) v += m[co][j] * rawKernel[e * 4 * nIn + j * nIn + i];
                    kernel[(e * nIn + i) * 3 + co] = v;
                    o[co] = v;
                }
                kernelIdx[e * nIn + i] = o;
            }
    }
    const size_t taps = nIn == 3 ? rawKernel.size() / 12 : 0;
    kernel.resize(nIn == 3 ? taps * 9 : kernel.size(), 0.f);
    for (size_t e = 0; e < taps; e++)
        for (int co = 0; co < 3; co++)
            for (int ci = 0; ci < 3; ci++)
            {
                float v = 0.f;
                for (int j = 0; j < 4; j++) v += m[co][j] * rawKernel[e * 12 + j * 3 + ci];
                kernel[e * 9 + co * 3 + ci] = v;
            }
    kernel4.assign(taps * 3, V4{});
    for (size_t e = 0; e < taps; e++)
        for (int ci = 0; ci < 3; ci++)
            kernel4[e * 3 + ci] = V4{kernel[e * 9 + ci], kernel[e * 9 + 3 + ci], kernel[e * 9 + 6 + ci], 0.f};
    for (int co = 0; co < 3; co++)
    {
        base[co] = 0.f;
        for (int j = 0; j < 4; j++) base[co] += m[co][j] * rawBase[j];
    }
    if (nNoise)
    {
        const size_t n = rawNoise.size() / 4;
        noiseData.assign(n * 3, 0.f);
        for (size_t i = 0; i < n; i++)
            for (int co = 0; co < 3; co++)
            {
                float v = 0.f;
                for (int j = 0; j < 4; j++) v += m[co][j] * rawNoise[i * 4 + j];
                noiseData[i * 3 + co] = v;
            }
    }
}
// The NES palette as a standard NTSC TV shows it: each entry's flat field through the chain
// (composite, standard decoder), averaged over whole carrier cycles so the colour wave's ripple
// cancels. This is how the usual NES palettes are worked out from the signal.
const float* nesPalette()
{
    static const std::vector<float> pal = []
    {
        std::vector<float> p(64 * 3, 0.f);
        Signal sg;
        sg.set.console = Console_NES;
        sg.set.cable = Cable_Composite;
        sg.set.decoder = Decoder_Standard;
        const Timing& tm = timing(Console_NES);
        sg.opp = tm.pixelClocks / tm.outStep;
        const int W = sg.outWidth();
        const int cycle = tm.carrierClocks / tm.outStep;   // decoded samples per carrier cycle
        std::vector<float> row(Signal::kWidth * 3), black(Signal::kWidth * 3, (float)kNesBlackIndex), raw(W * 4);
        float m[3][4];
        sg.outputMatrix(m);
        for (int i = 0; i < 64; i++)
        {
            std::fill(row.begin(), row.end(), (float)i);
            sg.runChain(row.data(), black.data(), raw.data(), 0, 0, nullptr);
            double acc[4] = {0, 0, 0, 0};
            const int n = cycle * 20, o0 = W / 2 - n / 2;
            for (int o = o0; o < o0 + n; o++)
                for (int j = 0; j < 4; j++) acc[j] += raw[o * 4 + j];
            for (int c = 0; c < 3; c++)
            {
                double v = 0;
                for (int j = 0; j < 4; j++) v += m[c][j] * acc[j] / n;
                p[i * 3 + c] = (float)std::clamp(v, 0.0, 1.0);
            }
        }
        return p;
    }();
    return pal.data();
}

// Each 5-bit R'G'B' colour's nearest palette entry, by distance in Y'UV with colour counting three
// times as much as brightness (a tint on a grey shows far more than a slightly wrong brightness).
// $0D ("blacker than black", which upsets some TVs) and the duplicate blacks are never picked.
const uint8_t* nesIndexLut()
{
    static const std::vector<uint8_t> lut = []
    {
        const float* pal = nesPalette();
        struct Cand { int index; float y, u, v; };
        std::vector<Cand> cand;
        auto yuv = [](float r, float g, float b, float& y, float& u, float& v)
        {
            y = 0.299f * r + 0.587f * g + 0.114f * b;
            u = 0.492f * (b - y);
            v = 0.877f * (r - y);
        };
        for (int i = 0; i < 64; i++)
        {
            const int hue = i & 15;
            if (i == 0x0D || (hue >= 14 && i != kNesBlackIndex) || i == 0x1D) continue;
            Cand c{i};
            yuv(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2], c.y, c.u, c.v);
            cand.push_back(c);
        }
        std::vector<uint8_t> t(32 * 32 * 32);
        for (int r = 0; r < 32; r++)
            for (int g = 0; g < 32; g++)
                for (int b = 0; b < 32; b++)
                {
                    float y, u, v;
                    yuv(r / 31.f, g / 31.f, b / 31.f, y, u, v);
                    float best = 1e9f;
                    int bi = kNesBlackIndex;
                    for (const Cand& c : cand)
                    {
                        const float d = (y - c.y) * (y - c.y) + 3.f * ((u - c.u) * (u - c.u) + (v - c.v) * (v - c.v));
                        if (d < best) { best = d; bi = c.index; }
                    }
                    t[(r << 10) | (g << 5) | b] = (uint8_t)bi;
                }
        return t;
    }();
    return lut.data();
}

int Signal::frameClockPhase(uint32_t frame) const
{
    if (set.console == Console_Off) return 0;
    const Timing& tm = timing(set.console);
    const long long cc = tm.carrierClocks;
    // frames alternate between the two lengths
    const long long pair = (tm.frameClocks[0] + tm.frameClocks[1]) % cc;
    return (int)(((long long)((frame / 2) % (uint32_t)cc) * pair + ((frame & 1) ? tm.frameClocks[0] % cc : 0)) % cc);
}
int Signal::frameSwitch(uint32_t frame) const
{
    const Timing& tm = timing(set.console);
    if (!tm.pal) return 0;
    // the switch flips every line: a frame starts with it as the lines before it left it
    return (int)(((frame / 2) * (uint32_t)(tm.linesPerFrame[0] + tm.linesPerFrame[1]) + ((frame & 1) ? tm.linesPerFrame[0] : 0)) & 1);
}

int Signal::linePhase(int row, uint32_t frame) const
{
    if (!active() || linePhaseOf.empty()) return 0;
    const Timing& tm = timing(set.console);
    const int clock = rowClockPhase(tm, frameClockPhase(frame), row, frame);
    const int sw = tm.pal ? (frameSwitch(frame) + row) & 1 : 0;
    return linePhaseOf[2 * clock + sw];
}

int Signal::noiseLineFor(int row, uint32_t frame) const
{
    return nNoise ? (int)(hash32(row * 7919u + frame * 104729u) % (uint32_t)nNoise) : 0;
}

void Signal::decodeLine(const float* rgb, const float* prev, float* out, int row, uint32_t frame) const
{
    const int W = outWidth();
    if (!active() || set.cable == Cable_RGB)
    {
        const Timing& tm = timing(set.console);
        const int pc = active() ? tm.pixelClocks : 1, step = active() ? tm.outStep : 1;
        for (int o = 0; o < W; o++)
        {
            int p = (o * step + step / 2) / pc;
            for (int c = 0; c < 3; c++) out[o * 3 + c] = rgb[p * 3 + c];
        }
        return;
    }
    const Timing& tm = timing(set.console);
    const int pc = tm.pixelClocks, step = tm.outStep;
    const int L = linePhase(row, frame);
    if (nIn != 3)
    {
        // NES: each pixel shown as its nearest palette entry, and each tap looks up what that
        // entry adds (black, past the edges, adds nothing)
        const int pad = nTaps;
        thread_local std::vector<uint8_t> idx[2];
        for (int rr = 0; rr < nRows; rr++)
        {
            const float* src = rr == 0 ? rgb : prev;
            idx[rr].assign(kWidth + 2 * pad, (uint8_t)kNesBlackIndex);
            if (src)
                for (int x = 0; x < kWidth; x++) idx[rr][pad + x] = (uint8_t)nesIndexOf(src[x * 3], src[x * 3 + 1], src[x * 3 + 2]);
        }
        const V4 b4{base[0], base[1], base[2], 0.f};
        for (int o = 0; o < W; o++)
        {
            const int p = (o * step + step / 2) / pc, op = o % nOutPhase;
            V4 acc = b4;
            for (int rr = 0; rr < nRows; rr++)
            {
                const V4* kw = &kernelIdx[((((size_t)L * nOutPhase + op) * nRows + rr) * nTaps) * nIn];
                const uint8_t* px = &idx[rr][pad + p - tapL];
                for (int k = 0; k < nTaps; k++, kw += nIn) acc += kw[px[k]];
            }
            out[o * 3 + 0] = acc[0]; out[o * 3 + 1] = acc[1]; out[o * 3 + 2] = acc[2];
        }
        if (nNoise && set.noise > 0)
        {
            const float* nl = &noiseData[(size_t)noiseLineFor(row, frame) * W * 3];
            for (int i = 0; i < W * 3; i++) out[i] += set.noise * nl[i];
        }
        return;
    }
    // the rows as 4-wide pixels, padded with black where the taps reach past the edges, so the
    // inner loop has no bounds checks: each tap is three 4-wide multiply-adds (kernel4: per input
    // channel, the weights to output R, G, B)
    const int pad = nTaps;
    thread_local std::vector<V4> row4[2];
    for (int rr = 0; rr < nRows; rr++)
    {
        const float* src = rr == 0 ? rgb : prev;
        row4[rr].assign(kWidth + 2 * pad, V4{});
        if (src)
            for (int x = 0; x < kWidth; x++) row4[rr][pad + x] = V4{src[x * 3], src[x * 3 + 1], src[x * 3 + 2], 0.f};
    }
    const V4 b4{base[0], base[1], base[2], 0.f};
    // one output phase at a time, so its weights stay in the L1 cache for all its outputs
    for (int op = 0; op < nOutPhase && op < W; op++)
        for (int rr = 0; rr < nRows; rr++)
        {
            const V4* kw0 = &kernel4[((((size_t)L * nOutPhase + op) * nRows + rr) * nTaps) * 3];
            for (int o = op; o < W; o += nOutPhase)
            {
                const int p = (o * step + step / 2) / pc;
                V4 acc = rr == 0 ? b4 : V4{out[o * 3], out[o * 3 + 1], out[o * 3 + 2], 0.f};
                const V4* kw = kw0;
                const V4* px = &row4[rr][pad + p - tapL];
                for (int k = 0; k < nTaps; k++, kw += 3, px++)
                    acc += kw[0] * (*px)[0] + kw[1] * (*px)[1] + kw[2] * (*px)[2];
                out[o * 3 + 0] = acc[0]; out[o * 3 + 1] = acc[1]; out[o * 3 + 2] = acc[2];
            }
        }
    if (nNoise && set.noise > 0)
    {
        const float* nl = &noiseData[(size_t)noiseLineFor(row, frame) * W * 3];
        for (int i = 0; i < W * 3; i++) out[i] += set.noise * nl[i];
    }
}

void Signal::simulateLine(const float* rgb, const float* prev, float* out, int row, uint32_t frame, bool noise) const
{
    if (!active()) { decodeLine(rgb, prev, out, row, frame); return; }
    const int clock = rowClockPhase(timing(set.console), frameClockPhase(frame), row, frame);
    const int sw = timing(set.console).pal ? (frameSwitch(frame) + row) & 1 : 0;
    if (set.cable == Cable_RGB) { decodeLine(rgb, prev, out, row, frame); return; }
    std::vector<float> zero(kWidth * 3, 0.f), raw(outWidth() * 4);
    if (indexed())
    {
        // the NES takes palette entries: each pixel's nearest one
        std::vector<float> ci(kWidth * 3, (float)kNesBlackIndex), pi(kWidth * 3, (float)kNesBlackIndex);
        for (int x = 0; x < kWidth; x++)
        {
            ci[x] = (float)nesIndexOf(rgb[x * 3], rgb[x * 3 + 1], rgb[x * 3 + 2]);
            if (prev) pi[x] = (float)nesIndexOf(prev[x * 3], prev[x * 3 + 1], prev[x * 3 + 2]);
        }
        runChain(ci.data(), pi.data(), raw.data(), clock, sw, nullptr);
    }
    else
        runChain(rgb, prev ? prev : zero.data(), raw.data(), clock, sw, nullptr);
    float m[3][4];
    outputMatrix(m);
    for (int o = 0; o < outWidth(); o++)
        for (int co = 0; co < 3; co++)
        {
            float v = 0.f;
            for (int j = 0; j < 4; j++) v += m[co][j] * raw[o * 4 + j];
            out[o * 3 + co] = v;
        }
    if (noise && nNoise && set.noise > 0)
    {
        const float* nl = &noiseData[(size_t)noiseLineFor(row, frame) * outWidth() * 3];
        for (int i = 0; i < outWidth() * 3; i++) out[i] += set.noise * nl[i];
    }
}

} // namespace Ntsc