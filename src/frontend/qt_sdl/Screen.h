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
#include "SoftPixelFilter.h"
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


class ScreenPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenPanel(QWidget* parent);
    virtual ~ScreenPanel();

    void setFilter(bool filter);
    void setPixelGrid(bool enable);

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
    bool pixelGrid = true;
public:
    std::atomic<float> gridParam[9];
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
    // software pixel grid (View > Software pixel grid): the shader's look without OpenGL
    std::unique_ptr<SoftPixelFilter> softFilter;
    QImage filtered[2];
    QTransform screenTrans[kMaxScreenTransforms];
};


struct GridParamDef { const char* name; const char* key; const char* uniform; float min, max, def; const char* tip; bool integer = false; };
// steps a parameter's slider has: whole numbers for integer ones
inline int gridParamSteps(const struct GridParamDef& d) { return d.integer ? (int)(d.max - d.min) : 1000; }
static const GridParamDef kGridParams[] =
{
    {"Vertical lines",    "GridDarkness", "uGridDark",  0.0f, 1.0f, 0.10f, "How dark the vertical grid lines between pixels are. 0 means no line at all,\n1 means a black line.\n\nThe line sits on the right edge of each DS pixel, inside it. With subpixels on,\nthe line covers the right-hand LED stripe more than the others, so the other\nstripes are dimmed to match and the line stays neutral instead of tinting the picture."}, // darkness: 0 = none, 1 = black
    {"Horizontal lines",  "GridDarknessH", "uGridDarkH", 0.0f, 1.0f, 0.50f, "How dark the horizontal grid lines between pixel rows are. 0 means no line at all,\n1 means a black line.\n\nThe line sits along the bottom edge of each DS pixel, inside it. On a real DSi XL\nthe gap between LED rows is thin, so a low value (around 0.1 to 0.2) at 4x or more\nlooks closest. It crosses all three LED stripes equally, so it never tints the picture."}, // darkness: 0 = none, 1 = black
    {"Line width (px)",   "GridWidthPx",  "uGridWidth", 0.0f, 4.0f, 1.0f, "Width of the grid lines, in whole screen pixels. The lines are drawn on the\nscreen's own pixels, so a width in between would only look uneven; at 1 each\nline is exactly one screen pixel wide at any zoom.\n\n0 hides the lines whatever the darkness sliders say.", true},
    {"Softness (px)",     "GridSoft",     "uGridSoft",  0.0f, 4.0f, 3.0f, "Belongs to the grid lines only. It sets how gradually each dark line fades\ninto the pixel next to it, measured in screen pixels.\n\nAt 0 a line has a hard edge. Higher values give a soft ramp, so lines look\nsmooth at zooms where they'd otherwise fall unevenly on the screen's pixels.\nIt never touches the picture itself."},  // edge antialiasing, output pixels
    {"Subpixel strength", "SubpixelStrength", "uSubStrength", 0.0f, 1.0f, 0.6f, "How strongly each DS pixel is split into the coloured LED stripes of a real screen.\n\nEach DS pixel becomes three columns, one per LED colour, in the order chosen under\nSubpixels. At 0 there are no stripes. At 1 each column shows only its own colour,\nwhich makes the picture about a third as bright; that is expected. The stripes are\napplied in linear light and balanced at every zoom, so they don't tint the picture."}, // 0 = off, 1 = each stripe only its colour
    {"Keep brightness",   "SubpixelKeep", "uSubKeep",   0.0f, 1.0f, 0.9f, "How much of the light the subpixel stripes take away is given back.\n\nAt 0 the stripes simply darken the picture (at full strength to a third).\nAt 1 the picture keeps its full brightness: the lit stripe is made brighter,\nand whatever doesn't fit in it spills over evenly onto the pixel's other\nstripes, so nothing clips and no colour shifts. Bright areas then show the\nstripes less, much like a real backlit screen. Values in between give part\nof the light back. Does nothing with Subpixels set to None."},
    {"Horizontal blur",   "BlurStrengthX", "uBlurX",    0.0f, 1.0f, 0.08f, "Acts on the picture. Each DS pixel borrows colour from its neighbours to the left\nand right: the box next to the slider sets how many DS pixels away it looks, and the\nslider sets how much those neighbours count against the pixel's own colour.\n\nIt's a plain average of the stored colours, so light and dark pull equally. Text\ngets softer and loses some contrast, but stays the same thickness. It's applied\nfirst, before the stripes and grid lines are drawn on top. At 1 the pixel and all\nits neighbours within reach count equally (a full box blur)."}, // weight of the neighbours within reach
    {"Vertical blur",     "BlurStrengthY", "uBlurY",    0.0f, 1.0f, 0.04f, "Acts on the picture. Each DS pixel borrows colour from its neighbours above and\nbelow: the box next to the slider sets how many DS pixels away it looks, and the\nslider sets how much those neighbours count against the pixel's own colour.\n\nIt's a plain average of the stored colours, so light and dark pull equally. Text\ngets softer and loses some contrast, but stays the same thickness. It's applied\nfirst, before the stripes and grid lines are drawn on top. At 1 the pixel and all\nits neighbours within reach count equally (a full box blur)."},
    {"Halo",              "BlurHalo",     "uHalo",      0.0f, 1.0f, 0.0f, "Changes how the blur averages. At 0 it's the plain average described for the blur.\nAt 1 it averages as light, so bright pixels add much more than dark ones take away.\n\nLight details then spread into their dark surroundings: white text on dark gets\nthicker and glows, while dark text on light stays almost the same. Values in between\nmix the two. Halo only works through the blur, so with both blur strengths at 0\nit does nothing."},  // 0 = plain blur, 1 = blur as light (bright spreads)
};
static constexpr int kNumGridParams = sizeof(kGridParams) / sizeof(kGridParams[0]);

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
    GLint screenShaderTransformULoc, screenShaderScreenSizeULoc, screenShaderGridULoc, screenShaderGridParamULoc[9], screenShaderSubpixelULoc, screenShaderBlurReachULoc, screenShaderSubSampleULoc;

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

