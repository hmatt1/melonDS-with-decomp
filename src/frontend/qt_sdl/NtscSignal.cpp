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
const Timing kTimings[Console_Count] =
{
    {"Off",                 0,              1,  1,  1,    1, {1, 1}},
    {"Mega Drive (H40)",    53693175.0,     8,  15, 3420, 2, {262LL * 3420, 262LL * 3420}},
    {"Mega Drive (H32)",    53693175.0,     10, 15, 3420, 2, {262LL * 3420, 262LL * 3420}},
    {"SNES",                21477272.0,     4,  6,  1364, 1, {262LL * 1364, 262LL * 1364 - 4}},
};

// a 2nd-order band-pass's delay to the envelope of a signal at its centre frequency, in clocks
double bandpassEnvelopeDelay(double Q, double fc, double fs) { return 2 * Q / (2 * kPi * fc) * fs; }

} // namespace


const Timing& timing(int console) { return kTimings[std::clamp(console, 0, Console_Count - 1)]; }


// The filters of one signal path: the console's encoder, the cable and the TV.
//
// Encoders:
//  Mega Drive 2: Sony CXA1645 (datasheet): standard U/V axes and Rec.601 luma; no colour-difference
//    low-pass before the modulator, the modulated chroma goes through an internal band-pass (its
//    width isn't given: chosen to pass SMPTE 170M's 1.3 MHz chroma, Q 1.1); an internal Y delay line
//    lines luma up with chroma; the composite output's luma goes through the datasheet's
//    recommended NTSC trap (68 uH + 28 pF, 3.65 MHz, Q about 1; not on the S-Video luma).
//  SNES: S-ENC (Rohm BA6592F type): colour-difference filters are board parts whose values aren't
//    documented, so it gets SMPTE 170M's template (under 2 dB down at 1.3 MHz, 20 dB down by
//    3.6 MHz: 4th-order Linkwitz-Riley at 2.0 MHz, no overshoot to speak of); no luma trap.
// TV (consumer NTSC set): luma through a 3.58 MHz trap, chroma through a 3.58 +-0.5 MHz band-pass
//  (US4980737's description of the usual design), synchronous demodulation against the burst,
//  then the colour-difference axes of the chosen decoder (Sony CXA2025AS figures: R-Y 112 deg / 0.83
//  and G-Y 252 deg / 0.30 for US sets, 95 deg / 0.78 and 240 deg / 0.30 for Japanese, against B-Y).
//  A line comb filter is not offered: these consoles' lines don't alternate chroma phase by 180
//  degrees as broadcast NTSC does, so a comb can't separate them (adaptive combs fall back to the
//  trap). The absolute carrier phase of the first pixel isn't documented; it only moves the hue of
//  the artifact colours, and is 0 here.
struct Signal::Chain
{
    double fs;
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
    double sharp = 0;
    bool standardAxes = true;
    double axR = 0, gR = 0, axG = 0, gG = 0;   // decoder axes (radians from B-Y) and gains (against B-Y)

    Chain(const Settings& s)
    {
        const Timing& tm = timing(s.console);
        fs = tm.masterHz;
        const bool composite = s.cable == Cable_Composite || s.cable == Cable_RF;

        double encChromaDelay = 0, encLumaPath = 0;
        if (s.console == Console_SNES)
        {
            encUV = true;
            encU = Filter::butterLow(2.0e6, 1, fs); encU.s.push_back(encU.s[0]);   // LR4 = Butterworth 2nd order, twice
            encV = encU;
            encChromaDelay = groupDelay(encU);
        }
        else   // Mega Drive 2, CXA1645
        {
            encBand = true;
            encBP.s.push_back(Biquad::bandpass(kFsc, 1.1, fs));
            encChromaDelay = bandpassEnvelopeDelay(1.1, kFsc, fs);
            if (composite)
            {
                encTrap = true;
                encTrapF.s.push_back(Biquad::notch(3.65e6, 1.0, fs));
                encLumaPath = groupDelay(encTrapF);
            }
        }
        encLumaDelay = std::max(0, (int)std::lround(encChromaDelay - encLumaPath));

        // RF: the TV tuner/IF passes about 4.2 MHz of video
        rfChannel = Filter::butterLow(4.2e6, 2, fs);
        ghost = s.ghost;
        ghostDelay = (int)std::lround(s.ghostDelay * 1e-6 * fs);

        // TV decoder
        const double bandQ = kFsc / 1.0e6;   // +-0.5 MHz
        tvTrap.s.push_back(Biquad::notch(kFsc, 2.0, fs));
        tvBand.s.push_back(Biquad::bandpass(kFsc, bandQ, fs));
        tvDemodU = Filter::butterLow(1.0e6, 1, fs);
        tvDemodV = Filter::butterLow(1.0e6, 1, fs);
        tvPeak = Filter::butterLow(2.6e6, 1, fs);
        sharp = s.sharpness;
        switch (s.decoder)
        {
        case Decoder_SonyUS: standardAxes = false; axR = 112; gR = 0.83; axG = 252; gG = 0.30; break;
        case Decoder_SonyJP: standardAxes = false; axR = 95;  gR = 0.78; axG = 240; gG = 0.30; break;
        default: break;
        }
        axR *= kPi / 180; axG *= kPi / 180;

        // the TV lines its luma up with its chroma (its delay line); then the luma path's whole
        // delay is taken out so the picture doesn't move
        double tvChroma = bandpassEnvelopeDelay(bandQ, kFsc, fs) + groupDelay(tvDemodU);
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
        const int cc = tm.carrierClocks;
        const int settle = (int)(20e-6 * fs), n = cc * 400;
        double si = 0, co = 0;
        for (int t = 0; t < settle + n; t++)
        {
            double ph = 2 * kPi * t / cc;
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


void Signal::runChain(const float* rgb, float* out, int lineStartPhase, const float* noiseIn) const
{
    const Timing& tm = timing(set.console);
    const int pc = tm.pixelClocks, cc = tm.carrierClocks, step = tm.outStep;
    const int c0 = step / 2;
    const int W = kWidth * opp;

    if (set.cable == Cable_RGB)
    {
        // no encoding: the pixels as they are, held for their clocks
        for (int o = 0; o < W; o++)
        {
            int p = (o * step + c0) / pc;
            for (int c = 0; c < 3; c++) out[o * 3 + c] = rgb[p * 3 + c];
        }
        return;
    }

    Chain ch(set);
    const double fs = tm.masterHz;
    const int before = (int)(12e-6 * fs), after = (int)(12e-6 * fs) + ch.outDelay;
    const int active = kWidth * pc;
    const int total = before + active + after;
    const double w = 2 * kPi / cc;
    Delay encLumaLine(ch.encLumaDelay), lumaLine(ch.lumaDelay), ghostLine(ch.ghostDelay);
    const double tr = set.tint * kPi / 180.0, ct = std::cos(tr), st = std::sin(tr);

    for (int t = 0; t < total; t++)
    {
        // console: the pixel this clock belongs to (black outside the picture), held by the DAC
        double R = 0, G = 0, B = 0;
        int ta = t - before;
        if (ta >= 0 && ta < active)
        {
            const float* px = rgb + (ta / pc) * 3;
            R = px[0]; G = px[1]; B = px[2];
        }
        // encoder
        double Y = 0.299 * R + 0.587 * G + 0.114 * B;
        double U = kUScale * (B - Y), V = kVScale * (R - Y);
        if (ch.encUV) { U = ch.encU.run(U); V = ch.encV.run(V); }
        double ph = w * (ta + lineStartPhase);
        double sn = std::sin(ph), cs = std::cos(ph);
        double C = U * sn + V * cs;
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
        if (ch.sharp > 0) yd += ch.sharp * (yd - ch.tvPeak.run(yd));
        // demodulate against the burst as received (phase and amplitude), then the TV's tint and
        // colour controls
        double phr = ph - ch.burstPhase;
        double u = ch.tvDemodU.run(2 * cd * std::sin(phr)) / ch.burstGain;
        double v = ch.tvDemodV.run(2 * cd * std::cos(phr)) / ch.burstGain;
        double uu = (u * ct - v * st) * set.colour, vv = (u * st + v * ct) * set.colour;
        // sample the decoded signal where each output sits, the luma path's delay taken out
        int to = t - ch.outDelay - before - c0;
        if (to >= 0 && to % step == 0 && to / step < W)
        {
            int o = to / step;
            double r, g, b;
            if (ch.standardAxes)
            {
                r = yd + vv / kVScale; b = yd + uu / kUScale;
                g = (yd - 0.299 * r - 0.114 * b) / 0.587;
            }
            else
            {
                // a decoder IC's own axes: each colour difference demodulated at its angle and
                // gain against B-Y
                auto at = [&](double a) { return (uu * std::cos(a) + vv * std::sin(a)) / kUScale; };
                b = yd + uu / kUScale;
                r = yd + ch.gR * at(ch.axR);
                g = yd + ch.gG * at(ch.axG);
            }
            out[o * 3 + 0] = (float)r; out[o * 3 + 1] = (float)g; out[o * 3 + 2] = (float)b;
        }
    }
}


void Signal::build(const Settings& s)
{
    // the noise level only scales the decoded noise lines: no rebuild for it
    Settings a = s, b = set;
    a.noise = b.noise = 0.f;
    set.noise = s.noise;
    if (built && a == b) return;
    set = s;
    built = true;
    gen++;
    kernel.clear();
    noiseData.clear();
    nNoise = 0;
    if (!active()) { opp = 1; nLinePhase = nOutPhase = nTaps = 1; tapL = 0; return; }

    const Timing& tm = timing(set.console);
    const int pc = tm.pixelClocks, cc = tm.carrierClocks, step = tm.outStep;
    opp = pc / step;
    off0 = (step / 2 + 0.5f) / pc;
    const int period = std::lcm(pc, cc);    // pixels and carrier line up again after this
    nOutPhase = period / step;
    const int pixPeriod = period / pc;
    const int W = kWidth * opp;

    // the carrier phases lines start at
    std::set<int> phases;
    for (uint32_t f = 0; f < (uint32_t)cc * 2; f++)
        for (int r = 0; r < 192; r++)
            phases.insert((frameClockPhase(f) + r * (tm.lineClocks % cc)) % cc);
    linePhaseClock.assign(phases.begin(), phases.end());
    linePhaseOf.assign(cc, 0);
    for (size_t i = 0; i < linePhaseClock.size(); i++) linePhaseOf[linePhaseClock[i]] = (int)i;
    nLinePhase = (int)linePhaseClock.size();

    // impulses through the chain: the response of each output to each nearby pixel
    const int span = 48;                         // pixels either side considered
    const int nAll = 2 * span + 1;
    std::vector<float> full((size_t)nLinePhase * nOutPhase * nAll * 9, 0.f);
    std::vector<float> zero(kWidth * 3, 0.f), blackOut(W * 3), resp(W * 3), in(kWidth * 3);
    const int p0base = (kWidth / 2) / pixPeriod * pixPeriod;
    for (int L = 0; L < nLinePhase; L++)
    {
        runChain(zero.data(), blackOut.data(), linePhaseClock[L], nullptr);
        if (L == 0) for (int c = 0; c < 3; c++) base[c] = blackOut[(W / 2) * 3 + c];
        for (int q = 0; q < pixPeriod; q++)
            for (int ci = 0; ci < 3; ci++)
            {
                const int p0 = p0base + q;
                std::fill(in.begin(), in.end(), 0.f);
                in[p0 * 3 + ci] = 1.f;
                runChain(in.data(), resp.data(), linePhaseClock[L], nullptr);
                for (int o = 0; o < W; o++)
                {
                    int p = (o * step + step / 2) / pc;
                    int k = p0 - p;
                    if (k < -span || k > span) continue;
                    int op = o % nOutPhase;
                    for (int co = 0; co < 3; co++)
                        full[(((size_t)L * nOutPhase + op) * nAll + (k + span)) * 9 + co * 3 + ci] =
                            resp[o * 3 + co] - blackOut[o * 3 + co];
                }
            }
    }

    // keep the taps that matter
    int lo = nAll, hi = -1;
    for (int k = 0; k < nAll; k++)
    {
        float m = 0;
        for (int L = 0; L < nLinePhase; L++)
            for (int op = 0; op < nOutPhase; op++)
                for (int j = 0; j < 9; j++)
                    m = std::max(m, std::fabs(full[(((size_t)L * nOutPhase + op) * nAll + k) * 9 + j]));
        // a tap whose every weight is under this changes no output by a visible amount: all the
        // dropped taps together stay under a quarter of an 8-bit step
        if (m > 4e-5f) { lo = std::min(lo, k); hi = std::max(hi, k); }
    }
    if (hi < lo) { lo = hi = span; }
    nTaps = hi - lo + 1;
    tapL = span - lo;
    kernel.assign((size_t)nLinePhase * nOutPhase * nTaps * 9, 0.f);
    for (int L = 0; L < nLinePhase; L++)
        for (int op = 0; op < nOutPhase; op++)
            for (int k = 0; k < nTaps; k++)
                for (int j = 0; j < 9; j++)
                    kernel[(((size_t)L * nOutPhase + op) * nTaps + k) * 9 + j] =
                        full[(((size_t)L * nOutPhase + op) * nAll + (k + lo)) * 9 + j];
    kernel4.assign((size_t)nLinePhase * nOutPhase * nTaps * 3, V4{});
    for (size_t e = 0; e < kernel.size() / 9; e++)
        for (int ci = 0; ci < 3; ci++)
            kernel4[e * 3 + ci] = V4{kernel[e * 9 + ci], kernel[e * 9 + 3 + ci], kernel[e * 9 + 6 + ci], 0.f};

    // decoded noise lines (the noise through the TV), for RF
    if (set.cable == Cable_RF)
    {
        const double fsr = tm.masterHz;
        const int total = (int)(24e-6 * fsr) + kWidth * pc + 64 + (int)(1e-6 * 8 * fsr);
        std::vector<float> nz(total + 4096);
        nNoise = kNoiseLines;
        noiseData.assign((size_t)nNoise * W * 3, 0.f);
        for (int l = 0; l < nNoise; l++)
        {
            for (size_t t = 0; t < nz.size(); t++) nz[t] = (float)(gauss((uint32_t)(l * 1000003u + t)) * kNoiseRms);
            runChain(zero.data(), resp.data(), linePhaseClock[0], nz.data());
            for (int i = 0; i < W * 3; i++) noiseData[(size_t)l * W * 3 + i] = resp[i] - blackOut[i];
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
int Signal::lineClockStep() const { return active() ? timing(set.console).lineClocks % timing(set.console).carrierClocks : 0; }
int Signal::carrierClocks() const { return active() ? timing(set.console).carrierClocks : 1; }

int Signal::linePhase(int row, uint32_t frame) const
{
    if (!active()) return 0;
    return linePhaseOf[(frameClockPhase(frame) + row * lineClockStep()) % carrierClocks()];
}

int Signal::noiseLineFor(int row, uint32_t frame) const
{
    return nNoise ? (int)(hash32(row * 7919u + frame * 104729u) % (uint32_t)nNoise) : 0;
}

void Signal::decodeLine(const float* rgb, float* out, int row, uint32_t frame) const
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
    // the row as 4-wide pixels, padded with black where the taps reach past the edges, so the
    // inner loop has no bounds checks: each tap is three 4-wide multiply-adds (kernel4: per input
    // channel, the weights to output R, G, B)
    const int pad = nTaps;
    thread_local std::vector<V4> row4;
    row4.assign(kWidth + 2 * pad, V4{});
    for (int x = 0; x < kWidth; x++) row4[pad + x] = V4{rgb[x * 3], rgb[x * 3 + 1], rgb[x * 3 + 2], 0.f};
    const V4 b4{base[0], base[1], base[2], 0.f};
    for (int o = 0; o < W; o++)
    {
        int p = (o * step + step / 2) / pc;
        const V4* kw = &kernel4[(((size_t)L * nOutPhase + o % nOutPhase) * nTaps) * 3];
        const V4* px = &row4[pad + p - tapL];
        V4 acc = b4;
        for (int k = 0; k < nTaps; k++, kw += 3, px++)
            acc += kw[0] * (*px)[0] + kw[1] * (*px)[1] + kw[2] * (*px)[2];
        out[o * 3 + 0] = acc[0]; out[o * 3 + 1] = acc[1]; out[o * 3 + 2] = acc[2];
    }
    if (nNoise && set.noise > 0)
    {
        const float* nl = &noiseData[(size_t)noiseLineFor(row, frame) * W * 3];
        for (int i = 0; i < W * 3; i++) out[i] += set.noise * nl[i];
    }
}

void Signal::simulateLine(const float* rgb, float* out, int row, uint32_t frame, bool noise) const
{
    if (!active()) { decodeLine(rgb, out, row, frame); return; }
    runChain(rgb, out, (frameClockPhase(frame) + row * lineClockStep()) % carrierClocks(), nullptr);
    if (noise && nNoise && set.noise > 0)
    {
        const float* nl = &noiseData[(size_t)noiseLineFor(row, frame) * outWidth() * 3];
        for (int i = 0; i < outWidth() * 3; i++) out[i] += set.noise * nl[i];
    }
}

} // namespace Ntsc
