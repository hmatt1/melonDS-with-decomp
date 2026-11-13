/*
    Video signal chain for the CRT filter: the DS picture as a console's video encoder would send
    it and a TV would decode it, NTSC or PAL.

    The chain is simulated at the console's master clock (or a whole multiple of it), so every
    timing relation is exact: pixels are held for the pixel divider's clocks (the DAC step), the
    colour carrier makes a fixed number of cycles per fixed number of clocks, and each line starts
    at the carrier phase its length in clocks gives it. Encoder and TV filters are causal
    (analog-style) filters, so colour lags luma the way it does on real hardware. PAL flips V's
    phase every line (the PAL switch), and a PAL-D TV mixes each line's chroma with the previous
    line's through its delay line, as an NTSC comb-filter TV separates chroma with the previous
    line: with either, an output depends on two DS rows.

    Everything from DS pixel to decoded R'G'B' is linear and periodic in the carrier phase, so the
    decoded output at any sample is an exact weighted sum of nearby pixels. (The NES makes its wave
    from palette entries, not R'G'B': each pixel is shown as its nearest entry, and the sum is over
    what each nearby pixel's entry adds, which is just as exact.) build() simulates
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

// (new ones are added at the end, so saved settings keep their meaning; the settings window lists
// them in its own order)
enum Console { Console_Off = 0, Console_MDH40, Console_MDH32, Console_SNES,
               Console_MDH40Pal, Console_MDH32Pal, Console_SNESPal,
               Console_NES, Console_MD1H40, Console_MD1H32, Console_MD1H40Pal, Console_MD1H32Pal,
               Console_N64, Console_PS1, Console_PS1W256, Console_PS1Pal, Console_Saturn352,
               Console_Count };
// what makes a console's signal: an RGB encoder IC (with its board's filters), or (NES) a PPU that
// generates the composite waveform itself from palette indices
enum Source { Source_CXA1645 = 0, Source_SEnc, Source_NES, Source_CXA1145, Source_BA7242 };
enum Cable { Cable_RGB = 0, Cable_SVideo, Cable_Composite, Cable_RF, Cable_Count };
// how the TV turns chroma back into colour differences. NTSC: the standard's axes, or the "red
// push" axes of a Sony TV decoder IC (CXA2025AS, US and Japanese settings). PAL: with a delay line
// (PAL-D): the 1990s kind on demodulated colour, locked to the incoming line (Philips TDA4661 /
// TDA4665), or the 1980s glass one on the chroma signal itself (a fixed 63.943 us); or without one
// (PAL-S)
// NTSC also: a 2-line comb filter (Decoder_Comb), see Chain
enum Decoder { Decoder_Standard = 0, Decoder_SonyUS, Decoder_SonyJP, Decoder_PalD, Decoder_PalS, Decoder_PalDGlass,
               Decoder_Comb, Decoder_Count };
bool isPal(int console);
// the cable a console can really use: the NES makes only composite (and RF from it), and the Mega
// Drive 1 has no S-Video
int cableFor(int console, int cable);
// the decoder a console's TV uses: a PAL decoder for PAL consoles, an NTSC one otherwise
int decoderFor(int console, int decoder);

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
    int carrierClocks;      // the carrier makes carrierCycles cycles in this many master clocks
    int lineClocks;         // master clocks per line (the visible part of every line)
    int outStep;            // master clocks between decoded samples (divides pixelClocks)
    long long frameClocks[2];   // master clocks per frame, even and odd frames
    int carrierCycles;      // see carrierClocks (1 when the carrier is master / divider)
    int linesPerFrame[2];   // lines per frame, even and odd frames (for PAL's line switch)
    int sub;                // simulation clocks per master clock with the glass delay line (so it is whole)
    bool pal;
    int source;             // Source
    int lineAlt = 0;        // 1: lines alternate lineClocks and lineClocks + 1 (PS1 NTSC), from even global lines
    double fsc() const { return masterHz * carrierCycles / carrierClocks; }
    // master clocks from the start of a frame to the start of its row
    long long rowClocks(int row, uint32_t frame) const
    {
        return (long long)row * lineClocks + (lineAlt ? (row + (int)(frame & 1)) / 2 : 0);
    }
};
const Timing& timing(int console);

// NES: the 64 palette entries as a standard TV decodes them (R'G'B', 0..1), and the entry each
// colour is shown as (the nearest one): nesIndexLut[(r5 << 10) | (g5 << 5) | b5] for 5-bit R'G'B'
const float* nesPalette();
const uint8_t* nesIndexLut();
inline int nesIndexOf(float r, float g, float b)
{
    auto q = [](float v) { return v <= 0.f ? 0 : v >= 1.f ? 31 : (int)(v * 31.f + 0.5f); };
    return nesIndexLut()[(q(r) << 10) | (q(g) << 5) | q(b)];
}

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

    // a line's state (the carrier phase it starts at, and for PAL the line switch), as an index
    // into the kernel's line phases. A frame's pattern is set by frameClockPhase, frameSwitch and
    // (lines of alternating length) frame & 1
    int linePhase(int row, uint32_t frame) const;
    int frameClockPhase(uint32_t frame) const;
    int frameSwitch(uint32_t frame) const;

    // rgb: kWidth gamma R'G'B' triples (0..1) of this row; prev: the row above (null for none:
    // black), used with a PAL-D TV; out: outWidth() decoded R'G'B' triples, not clipped
    void decodeLine(const float* rgb, const float* prev, float* out, int row, uint32_t frame) const;
    // the chain sample by sample (the reference for decodeLine); noise as decodeLine adds it
    void simulateLine(const float* rgb, const float* prev, float* out, int row, uint32_t frame, bool noise) const;

    // kernel layout, for the GPU:
    // weights[(((phase * outPhases + o) * rows + r) * taps + k) * 9 + 3 * outChannel + inChannel]
    // applied to pixel (pixel of the output) + k - tapLeft of the row r rows up.
    // NES (indexed()): weights[(((phase * outPhases + o) * rows + r) * taps + k) * 64 * 3 + 3 * index + outChannel],
    // the decoded R'G'B' a pixel of that palette entry adds
    bool indexed() const { return active() && timing(set.console).source == Source_NES; }
    int rows() const { return nRows; }
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
    friend const float* nesPalette();
    struct Chain;
    void buildChain();
    void finalize();
    void outputMatrix(float m[3][4]) const;
    // rgb / prev: R'G'B' rows, or (NES) palette indices as floats, one per pixel
    void runChain(const float* rgb, const float* prev, float* out, int lineStartPhase, int lineSwitch,
                  const float* noiseIn) const;

    Settings set;
    bool built = false;
    unsigned gen = 0;
    int opp = 1, nLinePhase = 1, nOutPhase = 1, nTaps = 1, tapL = 0, nRows = 1;
    float off0 = 0.5f;
    float base[3] = {0, 0, 0};
    std::vector<int> linePhaseOf;      // 2 * carrier phase (clocks, mod carrier) + switch -> line phase index
    std::vector<int> linePhaseClock;   // line phase index -> carrier phase in clocks
    std::vector<int> linePhaseSwitch;  // line phase index -> PAL switch (0 or 1)
    int nIn = 3;                        // inputs per pixel: R, G, B, or (NES) the 64 palette entries
    std::vector<float> rawKernel;       // per tap: the four TV signals (luma, its high-pass, U, V) x inputs
    std::vector<V4> kernelIdx;          // NES: per (tap, palette entry), the decoded R'G'B' it adds
    float rawBase[4] = {0, 0, 0, 0};
    std::vector<float> rawNoise;        // noise lines as the four TV signals
    std::vector<float> kernel;
    std::vector<V4> kernel4;           // the same, per (tap, input channel): weights to output R, G, B
    int nNoise = 0;
    std::vector<float> noiseData;
};

} // namespace Ntsc

#endif
