/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef SCREEN_H
#define SCREEN_H

#include <atomic>
#include <optional>
#include <deque>
#include <map>

#include <QWidget>
#include <memory>
#include "SoftLcdFilter.h"
#include "SoftCrtFilter.h"
#include "NtscSignal.h"
#include <QImage>
#include <QMutex>
#include <QScreen>
#include <QCloseEvent>
#include <QTimer>

#include "glad/glad.h"
#include "ScreenLayout.h"
#include "duckstation/gl/context.h"


class MainWindow;
class EmuInstance;


const struct { int id; float ratio; const char* label; } aspectRatios[] =
{
    { 0, 1,                       "4:3 (native)" },
    { 4, (5.f  / 3) / (4.f / 3), "5:3 (3DS)"},
    { 1, (16.f / 9) / (4.f / 3),  "16:9" },
    { 2, (21.f / 9) / (4.f / 3),  "21:9" },
    { 3, 0,                       "window" }
};
constexpr int AspectRatiosNum = sizeof(aspectRatios) / sizeof(aspectRatios[0]);


// View > Screen filter: one per instance (all its windows); each window keeps its own settings
enum { screenFilter_Off = 0, screenFilter_LCD = 1, screenFilter_CRT = 2, screenFilter_MAX };
namespace Config { class Table; }
// the instance's screen filter; before there was a choice of filter, PixelGrid said LCD or off
int screenFilterSetting(Config::Table& localCfg);


// a screen filter's slider: name, config key (per window), shader uniform, range, default, unit
// shown after the value, tooltip, whole numbers only
struct GridParamDef { const char* name; const char* key; const char* uniform; float min, max, def; const char* unit; const char* tip; bool integer = false; const char* section = nullptr; };
// steps a parameter's slider has: whole numbers for integer ones
inline int gridParamSteps(const struct GridParamDef& d) { return d.integer ? (int)(d.max - d.min) : 1000; }

// LCD filter (kScreenFS, SoftLcdFilter)
static const GridParamDef kGridParams[] =
{
    {"Vertical lines",    "GridDarkness", "uGridDark",  0.0f, 1.0f, 0.10f, "", "How dark the vertical grid lines between pixels are. 0 means no line at all,\n1 means a black line.\n\nThe line sits on the right edge of each DS pixel, inside it. With subpixels on,\nthe line covers the right-hand LED stripe more than the others, so the other\nstripes are dimmed to match and the line stays neutral instead of tinting the picture."}, // darkness: 0 = none, 1 = black
    {"Horizontal lines",  "GridDarknessH", "uGridDarkH", 0.0f, 1.0f, 0.50f, "", "How dark the horizontal grid lines between pixel rows are. 0 means no line at all,\n1 means a black line.\n\nThe line sits along the bottom edge of each DS pixel, inside it. On a real DSi XL\nthe gap between LED rows is thin, so a low value (around 0.1 to 0.2) at 4x or more\nlooks closest. It crosses all three LED stripes equally, so it never tints the picture."}, // darkness: 0 = none, 1 = black
    {"Line width",        "GridWidthPx",  "uGridWidth", 0.0f, 4.0f, 1.0f, "px", "Width of the grid lines, in whole screen pixels. The lines are drawn on the\nscreen's own pixels, so a width in between would only look uneven; at 1 each\nline is exactly one screen pixel wide at any zoom.\n\n0 hides the lines whatever the darkness sliders say.", true},
    {"Softness",          "GridSoft",     "uGridSoft",  0.0f, 4.0f, 3.0f, "px", "Belongs to the grid lines only. It sets how gradually each dark line fades\ninto the pixel next to it, measured in screen pixels.\n\nAt 0 a line has a hard edge. Higher values give a soft ramp, so lines look\nsmooth at zooms where they'd otherwise fall unevenly on the screen's pixels.\nIt never touches the picture itself."},  // edge antialiasing, output pixels
    {"Subpixel strength", "SubpixelStrength", "uSubStrength", 0.0f, 1.0f, 0.6f, "", "How strongly each DS pixel is split into the coloured LED stripes of a real screen.\n\nEach DS pixel becomes three columns, one per LED colour, in the order chosen under\nSubpixels. At 0 there are no stripes. At 1 each column shows only its own colour,\nwhich makes the picture about a third as bright; that is expected. The stripes are\napplied in linear light and balanced at every zoom, so they don't tint the picture."}, // 0 = off, 1 = each stripe only its colour
    {"Keep brightness",   "SubpixelKeep", "uSubKeep",   0.0f, 1.0f, 0.9f, "", "How much of the light the subpixel stripes take away is given back.\n\nAt 0 the stripes simply darken the picture (at full strength to a third).\nAt 1 the picture keeps its full brightness: the lit stripe is made brighter,\nand whatever doesn't fit in it spills over evenly onto the pixel's other\nstripes, so nothing clips and no colour shifts. Bright areas then show the\nstripes less, much like a real backlit screen. Values in between give part\nof the light back. Does nothing with Subpixels set to None."},
    {"Horizontal blur",   "BlurStrengthX", "uBlurX",    0.0f, 1.0f, 0.08f, "", "Acts on the picture. Each DS pixel borrows colour from its neighbours to the left\nand right: the box next to the slider sets how many DS pixels away it looks, and the\nslider sets how much those neighbours count against the pixel's own colour.\n\nIt's a plain average of the stored colours, so light and dark pull equally. Text\ngets softer and loses some contrast, but stays the same thickness. It's applied\nfirst, before the stripes and grid lines are drawn on top. At 1 the pixel and all\nits neighbours within reach count equally (a full box blur)."}, // weight of the neighbours within reach
    {"Vertical blur",     "BlurStrengthY", "uBlurY",    0.0f, 1.0f, 0.04f, "", "Acts on the picture. Each DS pixel borrows colour from its neighbours above and\nbelow: the box next to the slider sets how many DS pixels away it looks, and the\nslider sets how much those neighbours count against the pixel's own colour.\n\nIt's a plain average of the stored colours, so light and dark pull equally. Text\ngets softer and loses some contrast, but stays the same thickness. It's applied\nfirst, before the stripes and grid lines are drawn on top. At 1 the pixel and all\nits neighbours within reach count equally (a full box blur)."},
    {"Halo",              "BlurHalo",     "uHalo",      0.0f, 1.0f, 0.0f, "", "Changes how the blur averages. At 0 it's the plain average described for the blur.\nAt 1 it averages as light, so bright pixels add much more than dark ones take away.\n\nLight details then spread into their dark surroundings: white text on dark gets\nthicker and glows, while dark text on light stays almost the same. Values in between\nmix the two. Halo only works through the blur, so with both blur strengths at 0\nit does nothing."},  // 0 = plain blur, 1 = blur as light (bright spreads)
};
static constexpr int kNumGridParams = sizeof(kGridParams) / sizeof(kGridParams[0]);

// CRT filter (kCrtPrepassFS + kCrtFS, SoftCrtFilter), in the order the settings window shows them.
// The video signal ones without a uniform go to the signal chain (NtscSignal), not the shader.
enum CrtParam
{
    crt_Curvature, crt_BeamMin, crt_BeamMax, crt_Mask, crt_MaskPx, crt_Glow, crt_Vignette, crt_Boost,
    crt_LumaBlur, crt_ChromaBlur, crt_Colour, crt_Tint, crt_Sharpness, crt_Noise, crt_Ghost, crt_GhostDelay,
    crt_Count
};
static const GridParamDef kCrtParams[] =
{
    {"Curvature",           "CrtCurvature",  "uCrtWarp",       0.0f,  1.0f,  0.25f, "", "How much the picture bulges, like the curved glass of an old tube.\n\n0 is a flat screen. Anything outside the curved edge is black.\nThe curve is a little stronger top to bottom than side to side, as on a real set.", false, "Tube"},
    {"Scanline width, dark",   "CrtBeamMin", "uCrtBeamMin",    0.10f, 0.5f,  0.21f, "rows", "How thick each scanline is in dark parts of the picture, in DS rows.\n\nThinner lines leave wider black gaps between rows, so the scanlines show more.\nThicker lines fill the gaps in. Each DS row is drawn as one beam: at 3x zoom that\nis a bright screen row with a dimmer one either side, and more zoom gives the\nbeams a smoother shape.", false, "Tube"},
    {"Scanline width, bright", "CrtBeamMax", "uCrtBeamMax",    0.10f, 0.8f,  0.40f, "rows", "How thick each scanline is in bright parts of the picture, in DS rows.\n\nOn a real tube a brighter beam is wider, so bright areas fill the gaps between\nrows while dark areas keep visible scanlines. Keep it at or above the dark width\nfor that look; set both the same for even scanlines everywhere.", false, "Tube"},
    {"Mask strength",       "CrtMask",       "uCrtMask",       0.0f,  1.0f,  1.0f,  "", "How strongly the red, green and blue phosphor stripes of the tube show.\n\nThe stripes run top to bottom, like an aperture-grille tube. At 0 there are no\nstripes. At 1 each stripe is brightened in its own colour and the others dimmed,\nwhich darkens the picture a little overall; Brightness makes up for it.", false, "Tube"},
    {"Mask width",          "CrtMaskPx",     "uCrtMaskPx",     1.0f,  4.0f,  1.0f,  "px", "Width of each phosphor stripe, in screen pixels. The stripes are laid on the\nscreen's own pixels, so they stay the same size whatever the zoom.\n\nA red, green and blue stripe make one triad, three times this wide. At 3x zoom,\n1 puts exactly one triad on each DS pixel; at 6x, 2 does. Wider than that, the\ntriads span several DS pixels and show as coloured speckle.", true, "Tube"},
    {"Glow",                "CrtGlow",       "uCrtGlow",       0.0f,  1.0f,  0.15f, "", "A soft halo around bright parts of the picture, as light scatters in the glass\nof a tube. It adds light; it never darkens.\n\n0 turns it off. Higher values make bright areas bloom into their surroundings,\nwhich also washes out the bright, busy screens many DS games have.", false, "Tube"},
    {"Edge darkening",      "CrtVignette",   "uCrtVignette",   0.0f,  1.0f,  0.22f, "", "How much darker the corners and edges are than the centre, as on a real tube.\n\n0 keeps the brightness even across the screen.", false, "Tube"},
    {"Brightness",          "CrtBoost",      "uCrtBoost",      0.5f,  2.5f,  1.25f, "x", "Overall brightness. The scanline gaps and the phosphor mask both take light away,\nso the picture is brightened to make up for it. The default keeps mid-tones\nabout as bright as without the filter.\n\nHigher and bright colours clip to flat white (a white screen always does, unless\nthis is below about 0.85); lower and the picture looks dim.", false, "Tube"},
    {"Picture blur",        "CrtLumaBlur",   "uCrtLumaBlur",   0.05f, 1.0f,  0.45f, "DS px", "How much the brightness of the picture is smeared sideways, the way a TV signal\nsoftens it. Only sideways: rows stay sharp top to bottom.\n\nLower is sharper. Around 0.45, 1-pixel checkerboards and dithering melt into an\neven shade, as they did on a real TV.\n\nOnly with Console set to Off: with a console, the signal itself sets the sharpness.", false, "Video signal"},
    {"Colour bleed",        "CrtChromaBlur", "uCrtChromaBlur", 0.05f, 3.0f,  1.2f,  "DS px", "How far colour runs sideways into its neighbours, separately from brightness.\nTV signals carry colour with less detail than brightness, so colour edges are\nsofter than the picture itself.\n\nLower keeps colours crisp; higher lets them bleed further.\n\nOnly with Console set to Off: with a console, the signal itself sets the bleed.", false, "Video signal"},
    {"Colour",              "CrtColour",     nullptr,          0.0f,  2.0f,  1.0f,  "x", "The TV's colour control: how strongly colour shows, from black and white at 0.\n\n1 shows the colour as the console sent it. Used with a console's signal; with\nthe signal off, colour is as the DS made it.", false, "Video signal"},
    {"Tint",                "CrtTint",       nullptr,         -30.0f, 30.0f, 0.0f,  "deg", "The TV's tint (hue) control, in degrees. It turns every colour around the\ncolour wheel: negative toward purple, positive toward green, as on an NTSC set.\n\n0 is correct. Used with a console's signal.", false, "Video signal"},
    {"Sharpness",           "CrtSharpness",  nullptr,          0.0f,  1.0f,  0.0f,  "", "The TV's sharpness control: it boosts fine detail in brightness (peaking), which\nalso gives edges a light halo. 0 is off. Used with a console's signal.", false, "Video signal"},
    {"RF noise",            "CrtNoise",      nullptr,          0.0f,  1.0f,  0.0f,  "", "Snow from a weak RF signal: random noise added to the signal before the TV, so it\nshows in brightness and as colour speckle, different every frame.\n\nOnly with the RF cable.", false, "Video signal"},
    {"RF ghost",            "CrtGhost",      nullptr,          0.0f,  0.5f,  0.0f,  "", "A ghost: a weaker copy of the signal arriving a little late, as from a reflection\nin the aerial lead. This is its strength, against the picture.\n\nOnly with the RF cable.", false, "Video signal"},
    {"Ghost delay",         "CrtGhostDelay", nullptr,          0.2f,  4.0f,  1.0f,  "us", "How late the ghost arrives, in microseconds: about 2.4 DS pixels each at the\nMega Drive's 320-wide timing. Only with the RF cable and a ghost.", false, "Video signal"},
};
static constexpr int kNumCrtParams = sizeof(kCrtParams) / sizeof(kCrtParams[0]);


class ScreenPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenPanel(QWidget* parent);
    virtual ~ScreenPanel();

    void setFilter(bool filter);
    void setScreenFilter(int mode);   // screenFilter_Off, _LCD or _CRT

    void setMouseHide(bool enable, int delay);

    QTimer* setupMouseTimer();
    void updateMouseTimer();
    QTimer* mouseTimer;
    QSize screenGetMinSize(int factor);

    void osdSetEnabled(bool enabled);
    void osdAddMessage(unsigned int color, const char* msg);

    virtual void drawScreen() {}// = 0;

private slots:
    void onScreenLayoutChanged();
    void onAutoScreenSizingChanged(int sizing);

protected:
    bool shownTraced = false;   // Trace.h: the "first frame shown" marker was sent
    MainWindow* mainWindow;
    EmuInstance* emuInstance;

    bool filter;
    int screenFilter = screenFilter_LCD;
public:
    std::atomic<float> gridParam[kNumGridParams];   // LCD filter, kGridParams order
    std::atomic<float> crtParam[kNumCrtParams];     // CRT filter, kCrtParams order
    std::atomic<int> crtSignal{0};                  // video signal: Ntsc::Console (0 = off)
    std::atomic<int> crtCable{Ntsc::Cable_Composite};
    std::atomic<int> crtDecoder{Ntsc::Decoder_Standard};
    Ntsc::Settings signalSettings() const;
    std::atomic<bool> subpixelSample{false};  // each stripe reads the upscaled picture where it sits
    std::atomic<int> subpixelLayout{0};
    std::atomic<int> blurReach[2] = {1, 1};   // DS pixels each way, horizontal and vertical (0 to 3)   // 0 none, 1 DS Lite, 2 DSi, 3 DSi XL
protected:

    int screenRotation;
    int screenGap;
    int screenLayout;
    bool screenSwap;
    int screenSizing;
    bool integerScaling;
    int screenAspectTop, screenAspectBot;

    int autoScreenSizing;

    ScreenLayout layout;
    float screenMatrix[kMaxScreenTransforms][6];
    int screenKind[kMaxScreenTransforms];
    int numScreens;

    bool touching = false;

    bool mouseHide;
    int mouseHideDelay;

    struct OSDItem
    {
        unsigned int id;
        qint64 timestamp;

        char text[256];
        unsigned int color;

        bool rendered;
        QImage bitmap;

        int rainbowstart;
        int rainbowend;
    };

    QMutex osdMutex;
    bool osdEnabled;
    unsigned int osdID;
    std::deque<OSDItem> osdItems;

    QPixmap splashLogo;
    OSDItem splashText[3];
    QPoint splashPos[4];

    void loadConfig();

    virtual void setupScreenLayout();

    void resizeEvent(QResizeEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

    void tabletEvent(QTabletEvent* event) override;
    void touchEvent(QTouchEvent* event);
    bool event(QEvent* event) override;

    void showCursor();

    int osdFindBreakPoint(const char* text, int i);
    void osdLayoutText(const char* text, int* width, int* height, int* breaks);
    unsigned int osdRainbowColor(int inc);

    virtual void osdRenderItem(OSDItem* item);
    virtual void osdDeleteItem(OSDItem* item);

    void osdUpdate();

    void calcSplashLayout();
};


class ScreenPanelNative : public ScreenPanel
{
    Q_OBJECT

public:
    explicit ScreenPanelNative(QWidget* parent);
    virtual ~ScreenPanelNative();

    void drawScreen() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void setupScreenLayout() override;

    QMutex bufferLock;
    bool hasBuffers;
    void* topBuffer;
    void* bottomBuffer;

    QImage screen[2];
    // software screen filters: the shaders' look without OpenGL
    std::unique_ptr<SoftLcdFilter> lcdFilter;
    std::unique_ptr<SoftCrtFilter> crtFilter;
    QImage filtered[2];
    QTransform screenTrans[kMaxScreenTransforms];
};




class ScreenPanelGL : public ScreenPanel
{
    Q_OBJECT

public:
    explicit ScreenPanelGL(QWidget* parent);
    virtual ~ScreenPanelGL();

    std::optional<WindowInfo> getWindowInfo();

    bool createContext();

    void setSwapInterval(int intv);

    void initOpenGL();
    void deinitOpenGL();
    void makeCurrentGL();
    void releaseGL();

    void drawScreen() override;

    GL::Context* getContext() { return glContext.get(); }

    void transferLayout();
protected:

    qreal devicePixelRatioFromScreen() const;
    int scaledWindowWidth() const;
    int scaledWindowHeight() const;

    QPaintEngine* paintEngine() const override;

private:
    void setupScreenLayout() override;

    std::unique_ptr<GL::Context> glContext;
    bool glInited;

    GLuint screenVertexBuffer, screenVertexArray;
    GLuint screenTexture;
    GLuint screenShaderProgram;
    GLint screenShaderTransformULoc, screenShaderScreenSizeULoc, screenShaderGridULoc, screenShaderGridParamULoc[kNumGridParams], screenShaderSubpixelULoc, screenShaderBlurReachULoc, screenShaderSubSampleULoc;

    // CRT filter: a prepass at DS resolution into two texture arrays (one layer per screen), then
    // the main pass reading them
    GLuint crtPrepassProgram = 0, crtProgram = 0;
    GLuint crtFramebuffer = 0, crtYiqTexture = 0, crtGlowTexture = 0;
    GLint crtPrepassLayerULoc = -1;
    GLint crtSavedBinding[3] = {0, 0, 0};   // units 1 to 3, put back after the CRT draw
    GLint crtScreenSizeULoc = -1, crtTransformULoc = -1, crtParamULoc[kNumCrtParams];
    void drawCrtPrepass(unsigned layers);

    // CRT video signal chain: the kernel (weights) and decoded noise as textures, and a decode
    // pass writing each shown screen's decoded lines, which the CRT pass then reads
    static constexpr int kDecodedWidth = 1280;   // the widest decoded line (Mega Drive H32: 5 per pixel)
    Ntsc::Signal crtChain;
    unsigned crtChainUploaded = 0;
    bool crtChainValid = false;
    GLuint crtDecodeProgram = 0, crtDecodeFramebuffer = 0, crtDecodedTexture = 0, crtKernelTexture = 0, crtNoiseTexture = 0;
    struct { GLint layer, pixelClocks, outStep, outPhases, taps, tapLeft, carrier, lineStep, framePhase, linePhaseOf, bias, noise, noiseLines, frame; } crtDecLoc{};
    GLint crtSignalULoc = -1, crtSigPixelClocksULoc = -1, crtSigStepULoc = -1, crtSigHalfStepULoc = -1, crtSigWidthULoc = -1, crtSigTexWidthULoc = -1;
    void uploadCrtChain();
    void drawCrtDecode(unsigned layers, uint32_t frame);

    QMutex screenSettingsLock;
    WindowInfo windowInfo;

    int lastScreenWidth = -1, lastScreenHeight = -1;

    GLuint osdShader;
    GLint osdScreenSizeULoc, osdPosULoc, osdSizeULoc;
    GLint osdScaleFactorULoc;
    GLint osdTexScaleULoc;
    GLuint osdVertexArray;
    GLuint osdVertexBuffer;
    std::map<unsigned int, GLuint> osdTextures;

    GLuint logoTexture;

    void osdRenderItem(OSDItem* item) override;
    void osdDeleteItem(OSDItem* item) override;
};

#endif // SCREEN_H

