/*
    NTSC video signal chain for the CRT filter: the DS picture as a console's video encoder would
    send it and an NTSC TV would decode it.

    The chain is simulated at the console's master clock, so every timing relation is exact and
    in whole clocks: pixels are held for the pixel divider's clocks (the DAC step), the colour
    carrier runs at master / carrier divider, and each line starts at the carrier phase its
    length in clocks gives it. Encoder and TV filters are causal (analog-style) filters, so colour
    lags luma the way it does on real hardware.

    Everything from DS pixel to decoded R'G'B' is linear and periodic in the carrier phase, so the
    decoded output at any sample is an exact weighted sum of nearby pixels. build() simulates
    impulses through the chain once per setting and keeps those weights (the kernel); decodeLine()
    applies them. simulateLine() runs the chain sample by sample, and is the reference the kernel
    is tested against. RF noise is not part of the kernel: it is decoded noise, simulated once
    into a set of lines and added per line and frame.
*/

#ifndef NTSCSIGNAL_H
#define NTSCSIGNAL_H

#include <cstdint>
#include <vector>

namespace Ntsc
{

// four floats, added and multiplied lane by lane (SSE / NEON with GCC and Clang)
#if defined(__GNUC__) || defined(__clang__)
typedef float V4 __attribute__((vector_size(16)));
#else
struct V4
{
    float v[4];
    float& operator[](int i) { return v[i]; }
    float operator[](int i) const { return v[i]; }
    V4& operator+=(const V4& o) { for (int i = 0; i < 4; i++) v[i] += o.v[i]; return *this; }
    friend V4 operator*(const V4& a, float s) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] * s; return r; }
    friend V4 operator+(const V4& a, const V4& b) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i]; return r; }
};
#endif

enum Console { Console_Off = 0, Console_MDH40, Console_MDH32, Console_SNES, Console_Count };
enum Cable { Cable_RGB = 0, Cable_SVideo, Cable_Composite, Cable_RF, Cable_Count };
// how the TV turns chroma back into colour differences: the NTSC standard's axes, or the
// "red push" axes of a Sony TV decoder IC (CXA2025AS, US and Japanese settings)
enum Decoder { Decoder_Standard = 0, Decoder_SonyUS, Decoder_SonyJP, Decoder_Count };

struct Settings
{
    int console = Console_Off;
    int cable = Cable_Composite;
    int decoder = Decoder_Standard;
    float colour = 1.f;       // TV colour (saturation), 1 = as sent
    float tint = 0.f;         // TV tint, degrees
    float sharpness = 0.f;    // TV luma peaking, 0 = none, 1 = strong
    float noise = 0.f;        // RF: noise, 0 = none, 1 = strong (see kNoiseRms)
    float ghost = 0.f;        // RF: ghost (reflection) amplitude, 0..0.5 of the signal
    float ghostDelay = 1.f;   // RF: ghost delay, microseconds

    bool operator==(const Settings& o) const
    {
        return console == o.console && cable == o.cable && decoder == o.decoder && colour == o.colour && tint == o.tint &&
               sharpness == o.sharpness && noise == o.noise && ghost == o.ghost && ghostDelay == o.ghostDelay;
    }
};

// One console's video timing, in master clocks
struct Timing
{
    const char* name;
    double masterHz;
    int pixelClocks;        // master clocks per DS pixel
    int carrierClocks;      // master clocks per colour carrier cycle
    int lineClocks;         // master clocks per line (the visible part of every line)
    int outStep;            // master clocks between decoded samples (divides pixelClocks)
    long long frameClocks[2];   // master clocks per frame, even and odd frames
};
const Timing& timing(int console);

class Signal
{
public:
    static constexpr int kWidth = 256;

    // (re)builds the kernel for these settings; cheap when nothing changed
    void build(const Settings& s);
    const Settings& settings() const { return set; }
    unsigned generation() const { return gen; }   // goes up whenever the kernel or noise lines change
    bool active() const { return set.console != Console_Off; }

    int outPerPixel() const { return opp; }          // decoded samples per DS pixel
    int outWidth() const { return kWidth * opp; }
    // where decoded sample 0 sits, in DS pixels from the left edge of pixel 0 (its clock's centre)
    float outOffset() const { return off0; }

    // the carrier phase a line starts at, as an index into the kernel's line phases
    int linePhase(int row, uint32_t frame) const;
    // the same in parts, for the GPU: a line starts at (frameClockPhase(frame) + row * lineClockStep())
    // mod carrierClocks() clocks into the carrier, and linePhaseOfClock() maps that to an index
    int frameClockPhase(uint32_t frame) const;
    int lineClockStep() const;
    int carrierClocks() const;
    int linePhaseOfClock(int clock) const { return linePhaseOf.empty() ? 0 : linePhaseOf[clock]; }

    // rgb: kWidth gamma R'G'B' triples (0..1); out: outWidth() decoded R'G'B' triples, not clipped
    void decodeLine(const float* rgb, float* out, int row, uint32_t frame) const;
    // the chain sample by sample (the reference for decodeLine); noise as decodeLine adds it
    void simulateLine(const float* rgb, float* out, int row, uint32_t frame, bool noise) const;

    // kernel layout, for the GPU: weights[((phase * outPhases + o) * taps + k) * 9 + 3 * outChannel + inChannel]
    // applied to pixel (pixel of the output) + k - tapLeft
    int linePhases() const { return nLinePhase; }
    int outPhases() const { return nOutPhase; }
    int taps() const { return nTaps; }
    int tapLeft() const { return tapL; }
    const std::vector<float>& weights() const { return kernel; }
    const float* bias() const { return base; }            // decoded black, per channel (about 0)
    // decoded noise lines (at noise = 1), noiseLines() of outWidth() triples each
    int noiseLines() const { return nNoise; }
    const std::vector<float>& noise() const { return noiseData; }
    int noiseLineFor(int row, uint32_t frame) const;

private:
    struct Chain;
    void runChain(const float* rgb, float* out, int lineStartPhase, const float* noiseIn) const;

    Settings set;
    bool built = false;
    unsigned gen = 0;
    int opp = 1, nLinePhase = 1, nOutPhase = 1, nTaps = 1, tapL = 0;
    float off0 = 0.5f;
    float base[3] = {0, 0, 0};
    std::vector<int> linePhaseOf;      // carrier phase (clocks, mod carrier) -> line phase index
    std::vector<int> linePhaseClock;   // line phase index -> carrier phase in clocks
    std::vector<float> kernel;
    std::vector<V4> kernel4;           // the same, per (tap, input channel): weights to output R, G, B
    int nNoise = 0;
    std::vector<float> noiseData;
};

} // namespace Ntsc

#endif
