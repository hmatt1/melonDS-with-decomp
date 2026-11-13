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
double groupDelay(Filter f, int len = 4096)
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
const Timing kTimings[Console_Count] =
{
    //  name                 master Hz     pixel carrier line  out  frame clocks (even, odd)               cycles lines/frame sub PAL
    {"Off",                 0,              1,  1,  1,    1, {1, 1},                                   1, {262, 262}, 1, false},
    {"Mega Drive (H40)",    53693175.0,     8,  15, 3420, 2, {262LL * 3420, 262LL * 3420},             1, {262, 262}, 1, false},
    {"Mega Drive (H32)",    53693175.0,     10, 15, 3420, 2, {262LL * 3420, 262LL * 3420},             1, {262, 262}, 1, false},
    {"SNES",                21477272.0,     4,  6,  1364, 1, {262LL * 1364, 262LL * 1364 - 4},         1, {262, 262}, 1, false},
    {"Mega Drive PAL (H40)", 53203424.0,    8,  12, 3420, 2, {313LL * 3420, 313LL * 3420},             1, {313, 313}, 1, true},
    {"Mega Drive PAL (H32)", 53203424.0,    10, 12, 3420, 2, {313LL * 3420, 313LL * 3420},             1, {313, 313}, 1, true},
    {"SNES PAL",            21281370.0,     4,  24, 1364, 1, {312LL * 1364, 312LL * 1364},             5, {312, 312}, 5, true},
};
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

namespace
{
// how many DS rows each output depends on: two with a PAL-D TV (its delay line holds a line)
int rowsFor(const Settings& s)
{
    const int d = decoderFor(s.console, s.decoder);
    return isPal(s.console) && s.cable != Cable_RGB && (d == Decoder_PalD || d == Decoder_PalDGlass) ? 2 : 1;
}
// simulation clocks per master clock: finer only where the 1980s glass delay line's fixed 283.5
// cycles isn't a whole number of master clocks (SNES PAL: 1360.8)
int simSub(const Settings& s)
{
    return decoderFor(s.console, s.decoder) == Decoder_PalDGlass && s.cable != Cable_RGB ? timing(s.console).sub : 1;
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
//  SNES: S-ENC (Rohm BA6592F type): colour-difference filters are board parts whose values aren't
//    documented, so it gets SMPTE 170M's template (under 2 dB down at 1.3 MHz, 20 dB down by
//    3.6 MHz: 4th-order Linkwitz-Riley at 2.0 MHz, no overshoot to speak of); no luma trap.
// TV:
//  NTSC (consumer set): luma through a 3.58 MHz trap, chroma through a 3.58 +-0.5 MHz band-pass
//    (US4980737's description of the usual design), synchronous demodulation against the burst,
//    then the colour-difference axes of the chosen decoder (Sony CXA2025AS figures: R-Y 112 deg /
//    0.83 and G-Y 252 deg / 0.30 for US sets, 95 deg / 0.78 and 240 deg / 0.30 for Japanese).
//    A line comb filter is not offered: these consoles' lines don't alternate chroma phase by 180
//    degrees as broadcast NTSC does, so a comb can't separate them.
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
    bool pal = false, palD = false, palGlass = false;   // PAL-D: either kind; palGlass: the 1980s kind
    // encoder
    bool encUV = false, encBand = false, encTrap = false;
    Filter encU, encV, encBP, encTrapF;
    int encLumaDelay = 0;
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
        const bool snes = s.console == Console_SNES || s.console == Console_SNESPal;

        double encChromaDelay = 0, encLumaPath = 0;
        if (snes)
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

        // the TV lines its luma up with its chroma (its delay line); then the luma path's whole
        // delay is taken out so the picture doesn't move
        double tvChroma = bandpassEnvelopeDelay(bandQ, fsc, fs) + groupDelay(tvDemodU);
        double tvLuma = composite ? groupDelay(tvTrap) : 0.0;
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
        bp.reset(); ch.reset(); band.reset();
        Delay gl(ghostDelay);
        const double w = 2 * kPi * tm.carrierCycles / ((double)tm.carrierClocks * sub);
        const int settle = (int)(20e-6 * fs), n = tm.carrierClocks * sub * 400;
        double si = 0, co = 0;
        for (int t = 0; t < settle + n; t++)
        {
            double ph = w * t;
            double x = std::sin(ph);
            if (encBand) x = bp.run(x);
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
    const bool twoLines = ch.palD;
    const int curStart = twoLines ? lineLen : 0;      // the current line, after the previous one
    const int total = curStart + A + active + (int)(12e-6 * fs) + ch.outDelay;
    const double w = 2 * kPi * tm.carrierCycles / ((double)tm.carrierClocks * sub);
    const double ph0 = 2 * kPi * tm.carrierCycles * (double)lineStartPhase / tm.carrierClocks;
    Delay encLumaLine(ch.encLumaDelay), lumaLine(ch.lumaDelay), ghostLine(ch.ghostDelay);
    Delay palLine(ch.palGlass ? ch.palDelay : 0), palU(ch.palGlass ? 0 : ch.palDelay), palV(ch.palGlass ? 0 : ch.palDelay);
    const double curSign = ch.pal && lineSwitch ? -1.0 : 1.0;

    for (int t = 0; t < total; t++)
    {
        // console: the pixel this clock belongs to (black outside the picture), held by the DAC
        const bool inPrev = twoLines && t < curStart;
        const float* row = inPrev ? prev : rgb;
        const double vSign = inPrev ? -curSign : curSign;
        double R = 0, G = 0, B = 0;
        int ta = t - (inPrev ? 0 : curStart) - A;
        if (row && ta >= 0 && ta < active)
        {
            const float* px = row + (ta / (pc * sub)) * 3;
            R = px[0]; G = px[1]; B = px[2];
        }
        // encoder
        double Y = 0.299 * R + 0.587 * G + 0.114 * B;
        double U = kUScale * (B - Y), V = kVScale * (R - Y);
        if (ch.encUV) { U = ch.encU.run(U); V = ch.encV.run(V); }
        double ph = w * (t - curStart - A) + ph0;   // phase ph0 at the line's first pixel
        double sn = std::sin(ph), cs = std::cos(ph);
        double C = U * sn + vSign * V * cs;
        if (ch.encBand) C = ch.encBP.run(C);
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
        double yd = set.cable == Cable_SVideo ? yIn : ch.tvTrap.run(yIn);
        double cd = ch.tvBand.run(cIn);
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
    k.decoder = isPal(s.console) ? decoderFor(s.console, s.decoder) : Decoder_Standard;
    if (s.cable != Cable_RF) { k.ghost = 0.f; k.ghostDelay = 1.f; }
    return k;
}
}

void Signal::build(const Settings& s)
{
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
            states.insert({(frameClockPhase(f) + r * (tm.lineClocks % cc)) % cc, tm.pal ? (frameSwitch(f) + r) & 1 : 0});
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

    // impulses through the chain
    const int span = 48;                         // pixels either side considered
    const int nAll = 2 * span + 1;
    auto fullAt = [&](int L, int op, int rr, int k) { return (((size_t)L * nOutPhase + op) * nRows + rr) * nAll + k; };
    std::vector<float> full((size_t)nLinePhase * nOutPhase * nRows * nAll * 12, 0.f);
    std::vector<float> zero(kWidth * 3, 0.f), blackOut(W * 4), resp(W * 4), in(kWidth * 3);
    const int p0base = (kWidth / 2) / pixPeriod * pixPeriod;
    for (int L = 0; L < nLinePhase; L++)
    {
        runChain(zero.data(), zero.data(), blackOut.data(), linePhaseClock[L], linePhaseSwitch[L], nullptr);
        if (L == 0) for (int c = 0; c < 4; c++) rawBase[c] = blackOut[(W / 2) * 4 + c];
        for (int rr = 0; rr < nRows; rr++)
            for (int q = 0; q < pixPeriod; q++)
                for (int ci = 0; ci < 3; ci++)
                {
                    const int p0 = p0base + q;
                    std::fill(in.begin(), in.end(), 0.f);
                    in[p0 * 3 + ci] = 1.f;
                    if (rr == 0) runChain(in.data(), zero.data(), resp.data(), linePhaseClock[L], linePhaseSwitch[L], nullptr);
                    else runChain(zero.data(), in.data(), resp.data(), linePhaseClock[L], linePhaseSwitch[L], nullptr);
                    for (int o = 0; o < W; o++)
                    {
                        int p = (o * step + step / 2) / pc;
                        int k = p0 - p;
                        if (k < -span || k > span) continue;
                        int op = o % nOutPhase;
                        for (int co = 0; co < 4; co++)
                            full[fullAt(L, op, rr, k + span) * 12 + co * 3 + ci] = resp[o * 4 + co] - blackOut[o * 4 + co];
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
                    for (int j = 0; j < 12; j++)
                        m = std::max(m, std::fabs(full[fullAt(L, op, rr, k) * 12 + j]));
        if (m > 4e-5f) { lo = std::min(lo, k); hi = std::max(hi, k); }
    }
    if (hi < lo) { lo = hi = span; }
    nTaps = hi - lo + 1;
    tapL = span - lo;
    rawKernel.assign((size_t)nLinePhase * nOutPhase * nRows * nTaps * 12, 0.f);
    for (int L = 0; L < nLinePhase; L++)
        for (int op = 0; op < nOutPhase; op++)
            for (int rr = 0; rr < nRows; rr++)
                for (int k = 0; k < nTaps; k++)
                    for (int j = 0; j < 12; j++)
                        rawKernel[((((size_t)L * nOutPhase + op) * nRows + rr) * nTaps + k) * 12 + j] = full[fullAt(L, op, rr, k + lo) * 12 + j];

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
    if (rawKernel.empty()) { base[0] = base[1] = base[2] = 0.f; return; }
    float m[3][4];
    outputMatrix(m);
    const size_t taps = rawKernel.size() / 12;
    kernel.assign(taps * 9, 0.f);
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
int Signal::lineClockStep() const { return active() ? timing(set.console).lineClocks % timing(set.console).carrierClocks : 0; }
int Signal::carrierClocks() const { return active() ? timing(set.console).carrierClocks : 1; }

int Signal::linePhase(int row, uint32_t frame) const
{
    if (!active()) return 0;
    int clock = (frameClockPhase(frame) + row * lineClockStep()) % carrierClocks();
    int sw = (frameSwitch(frame) + row) & 1;
    return linePhaseOf[2 * clock + (timing(set.console).pal ? sw : 0)];
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
    const int clock = (frameClockPhase(frame) + row * lineClockStep()) % carrierClocks();
    const int sw = timing(set.console).pal ? (frameSwitch(frame) + row) & 1 : 0;
    if (set.cable == Cable_RGB) { decodeLine(rgb, prev, out, row, frame); return; }
    std::vector<float> zero(kWidth * 3, 0.f), raw(outWidth() * 4);
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