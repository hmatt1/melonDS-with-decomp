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

#include <string.h>

#include <optional>
#include <thread>
#include <cmath>

#include <QPaintEvent>
#include <QPainter>

#include <QDateTime>

#include "Trace.h"
#include "OpenGLSupport.h"
#include "duckstation/gl/context.h"

#include "main.h"
#include "EmuInstance.h"

#include "NDS.h"
#include "GPU.h"
#include "GPU3D_Soft.h"
#include "GPU3D_OpenGL.h"
#include "Platform.h"
#include "Config.h"

#include "main_shaders.h"
#include "OSD_shaders.h"
#include "font.h"
#include "version.h"

using namespace melonDS;

#if !defined(_WIN32) && !defined(APPLE)
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
using namespace QNativeInterface;
#else
#include <qpa/qplatformnativeinterface.h>
#endif
#endif


const u32 kOSDMargin = 6;
const int kLogoWidth = 192;


ScreenPanel::ScreenPanel(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_AcceptTouchEvents);

    QWidget* w = parent;
    for (;;)
    {
        mainWindow = qobject_cast<MainWindow*>(w);
        if (mainWindow) break;
        w = w->parentWidget();
        if (!w) break;
    }

    emuInstance = mainWindow->getEmuInstance();

    mouseHide = false;
    mouseHideDelay = 0;

    QTimer* mouseTimer = setupMouseTimer();
    connect(mouseTimer, &QTimer::timeout, [=] { if (mouseHide) setCursor(Qt::BlankCursor);});

    osdEnabled = false;
    osdID = 1;
    
    loadConfig();
    setFilter(false);   // blur is the screen filters' own, in DS pixels
    setScreenFilter(screenFilterSetting(emuInstance->getLocalConfig()));
    for (int i = 0; i < kNumGridParams; i++)
        gridParam[i] = (float)mainWindow->getWindowConfig().GetDouble(kGridParams[i].key);
    for (int i = 0; i < kNumCrtParams; i++)
        crtParam[i] = (float)mainWindow->getWindowConfig().GetDouble(kCrtParams[i].key);
    crtSignal = mainWindow->getWindowConfig().GetInt("CrtSignal");
    crtCable = mainWindow->getWindowConfig().GetInt("CrtCable");
    crtDecoder = mainWindow->getWindowConfig().GetInt("CrtDecoder");
    subpixelLayout = mainWindow->getWindowConfig().GetInt("SubpixelLayout");
    subpixelSample = mainWindow->getWindowConfig().GetBool("SubpixelSample");
    blurReach[0] = mainWindow->getWindowConfig().GetInt("BlurReachX");
    blurReach[1] = mainWindow->getWindowConfig().GetInt("BlurReachY");

    splashLogo = QPixmap(":/melon-logo");

    strncpy(splashText[0].text, "File->Open ROM...", 256);
    splashText[0].id = 0x80000000;
    splashText[0].color = 0;
    splashText[0].rendered = false;
    splashText[0].rainbowstart = -1;

    strncpy(splashText[1].text, "to get started", 256);
    splashText[1].id = 0x80000001;
    splashText[1].color = 0;
    splashText[1].rendered = false;
    splashText[1].rainbowstart = -1;

    std::string url = MELONDS_URL;
    int urlpos = url.find("://");
    urlpos = (urlpos == std::string::npos) ? 0 : urlpos+3;
    strncpy(splashText[2].text, url.c_str() + urlpos, 256);
    splashText[2].id = 0x80000002;
    splashText[2].color = 0;
    splashText[2].rendered = false;
    splashText[2].rainbowstart = -1;
}

ScreenPanel::~ScreenPanel()
{
    mouseTimer->stop();
    delete mouseTimer;
}

void ScreenPanel::loadConfig()
{
    auto& cfg = mainWindow->getWindowConfig();
    
    screenRotation = cfg.GetInt("ScreenRotation");
    screenGap = cfg.GetInt("ScreenGap");
    screenLayout = cfg.GetInt("ScreenLayout");
    screenSwap = cfg.GetBool("ScreenSwap");
    screenSizing = cfg.GetInt("ScreenSizing");
    integerScaling = cfg.GetBool("IntegerScaling");
    screenAspectTop = cfg.GetInt("ScreenAspectTop");
    screenAspectBot = cfg.GetInt("ScreenAspectBot");
}

void ScreenPanel::setFilter(bool filter)
{
    this->filter = filter;
}

void ScreenPanel::setScreenFilter(int mode)
{
    this->screenFilter = std::clamp(mode, 0, (int)screenFilter_MAX - 1);
}

Ntsc::Settings ScreenPanel::signalSettings() const
{
    Ntsc::Settings s;
    s.console = std::clamp((int)crtSignal, 0, (int)Ntsc::Console_Count - 1);
    s.cable = std::clamp((int)crtCable, 0, (int)Ntsc::Cable_Count - 1);
    s.decoder = std::clamp((int)crtDecoder, 0, (int)Ntsc::Decoder_Count - 1);
    s.colour = crtParam[crt_Colour]; s.tint = crtParam[crt_Tint]; s.sharpness = crtParam[crt_Sharpness];
    s.noise = crtParam[crt_Noise]; s.ghost = crtParam[crt_Ghost]; s.ghostDelay = crtParam[crt_GhostDelay];
    return s;
}

int softFilterThreads(Config::Table& localCfg)
{
    // PixelGridThreads, or 0 for half the logical cores, 2 to 4 (the emulator itself needs the rest)
    int threads = localCfg.GetInt("PixelGridThreads");
    if (threads <= 0) threads = std::clamp((int)std::thread::hardware_concurrency() / 2, 2, 4);
    return threads;
}

int screenFilterSetting(Config::Table& localCfg)
{
    int mode = localCfg.GetInt("ScreenFilterMode");
    if (mode < 0 || mode >= screenFilter_MAX)   // never chosen: follow the old on/off switch
        mode = localCfg.GetBool("PixelGrid") ? screenFilter_LCD : screenFilter_Off;
    return mode;
}

void ScreenPanel::setMouseHide(bool enable, int delay)
{
    mouseHide = enable;
    mouseHideDelay = delay;

    mouseTimer->setInterval(mouseHideDelay);
}

void ScreenPanel::setupScreenLayout()
{
    int w = width();
    int h = height();

    int sizing = screenSizing;
    if (sizing == screenSizing_Auto) sizing = autoScreenSizing;

    float aspectTop, aspectBot;

    for (auto ratio : aspectRatios)
    {
        if (ratio.id == screenAspectTop)
            aspectTop = ratio.ratio;
        if (ratio.id == screenAspectBot)
            aspectBot = ratio.ratio;
    }

    if (aspectTop == 0)
        aspectTop = ((float) w / h) / (4.f / 3.f);

    if (aspectBot == 0)
        aspectBot = ((float) w / h) / (4.f / 3.f);

    layout.Setup(w, h,
                static_cast<ScreenLayoutType>(screenLayout),
                static_cast<ScreenRotation>(screenRotation),
                static_cast<ScreenSizing>(sizing),
                screenGap,
                integerScaling != 0,
                screenSwap != 0,
                aspectTop,
                aspectBot);

    numScreens = layout.GetScreenTransforms(screenMatrix[0], screenKind);

    calcSplashLayout();
}

QSize ScreenPanel::screenGetMinSize(int factor = 1)
{
    bool isHori = (screenRotation == screenRot_90Deg
        || screenRotation == screenRot_270Deg);
    int gap = screenGap * factor;

    int w = 256 * factor;
    int h = 192 * factor;

    if (screenSizing == screenSizing_TopOnly
        || screenSizing == screenSizing_BotOnly)
    {
        return QSize(w, h);
    }

    if (screenLayout == screenLayout_Natural)
    {
        if (isHori)
            return QSize(h+gap+h, w);
        else
            return QSize(w, h+gap+h);
    }
    else if (screenLayout == screenLayout_Vertical)
    {
        if (isHori)
            return QSize(h, w+gap+w);
        else
            return QSize(w, h+gap+h);
    }
    else if (screenLayout == screenLayout_Horizontal)
    {
        if (isHori)
            return QSize(h+gap+h, w);
        else
            return QSize(w+gap+w, h);
    }
    else // hybrid
    {
        if (isHori)
            return QSize(h+gap+h, 3*w + (int)ceil((4*gap) / 3.0));
        else
            return QSize(3*w + (int)ceil((4*gap) / 3.0), h+gap+h);
    }
}

void ScreenPanel::onScreenLayoutChanged()
{
    loadConfig();

    setMinimumSize(screenGetMinSize());
    setupScreenLayout();
}

void ScreenPanel::onAutoScreenSizingChanged(int sizing)
{
    autoScreenSizing = sizing;
    if (screenSizing != screenSizing_Auto) return;

    setupScreenLayout();
}

void ScreenPanel::resizeEvent(QResizeEvent* event)
{
    setupScreenLayout();
    QWidget::resizeEvent(event);
}

void ScreenPanel::mousePressEvent(QMouseEvent* event)
{
    event->accept();
    if (!emuInstance->emuIsActive()) { touching = false; return; }
    if (event->button() != Qt::LeftButton) return;

    int x = event->pos().x();
    int y = event->pos().y();

    if (layout.GetTouchCoords(x, y, false))
    {
        touching = true;
        emuInstance->touchScreen(x, y);
    }
}

void ScreenPanel::mouseReleaseEvent(QMouseEvent* event)
{
    event->accept();
    if (!emuInstance->emuIsActive()) { touching = false; return; }
    if (event->button() != Qt::LeftButton) return;

    if (touching)
    {
        touching = false;
        emuInstance->releaseScreen();
    }
}

void ScreenPanel::mouseMoveEvent(QMouseEvent* event)
{
    event->accept();

    showCursor();

    if (!emuInstance->emuIsActive()) return;
    //if (!(event->buttons() & Qt::LeftButton)) return;
    if (!touching) return;

    int x = event->pos().x();
    int y = event->pos().y();

    if (layout.GetTouchCoords(x, y, true))
    {
        emuInstance->touchScreen(x, y);
    }
}

void ScreenPanel::tabletEvent(QTabletEvent* event)
{
    event->accept();
    if (!emuInstance->emuIsActive()) { touching = false; return; }

    switch(event->type())
    {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
        {
#if QT_VERSION_MAJOR == 6
            int x = event->position().x();
            int y = event->position().y();
#else
            int x = event->x();
            int y = event->y();
#endif

            if (layout.GetTouchCoords(x, y, event->type()==QEvent::TabletMove))
            {
                touching = true;
                emuInstance->touchScreen(x, y);
            }
        }
        break;
    case QEvent::TabletRelease:
        if (touching)
        {
            emuInstance->releaseScreen();
            touching = false;
        }
        break;
    default:
        break;
    }
}

void ScreenPanel::touchEvent(QTouchEvent* event)
{
#if QT_VERSION_MAJOR == 6
    if (event->device()->type() == QInputDevice::DeviceType::TouchPad)
        return;
#endif

    event->accept();
    if (!emuInstance->emuIsActive()) { touching = false; return; }

    switch(event->type())
    {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
#if QT_VERSION_MAJOR == 6
        if (event->points().length() > 0)
        {
            QPointF lastPosition = event->points().first().lastPosition();
#else
        if (event->touchPoints().length() > 0)
        {
            QPointF lastPosition = event->touchPoints().first().lastPos();
#endif
            int x = (int)lastPosition.x();
            int y = (int)lastPosition.y();

            if (layout.GetTouchCoords(x, y, event->type()==QEvent::TouchUpdate))
            {
                touching = true;
                emuInstance->touchScreen(x, y);
            }
        }
        break;
    case QEvent::TouchEnd:
        if (touching)
        {
            emuInstance->releaseScreen();
            touching = false;
        }
        break;
    default:
        break;
    }
}

bool ScreenPanel::event(QEvent* event)
{
    if (event->type() == QEvent::TouchBegin
        || event->type() == QEvent::TouchEnd
        || event->type() == QEvent::TouchUpdate)
    {
        touchEvent((QTouchEvent*)event);
        return true;
    }
    else if (event->type() == QEvent::FocusIn)
        mainWindow->onFocusIn();
    else if (event->type() == QEvent::FocusOut)
        mainWindow->onFocusOut();

    return QWidget::event(event);
}

void ScreenPanel::showCursor()
{
    mainWindow->panel->setCursor(Qt::ArrowCursor);
    mouseTimer->start();
}

QTimer* ScreenPanel::setupMouseTimer()
{
    mouseTimer = new QTimer();
    mouseTimer->setSingleShot(true);
    mouseTimer->setInterval(mouseHideDelay);
    mouseTimer->start();

    return mouseTimer;
}

int ScreenPanel::osdFindBreakPoint(const char* text, int i)
{
    // i = character that went out of bounds

    for (int j = i; j >= 0; j--)
    {
        if (text[j] == ' ')
            return j;
    }

    return i;
}

void ScreenPanel::osdLayoutText(const char* text, int* width, int* height, int* breaks)
{
    int w = 0;
    int h = 14;
    int totalw = 0;
    int maxw = ((QWidget*)this)->width() - (kOSDMargin*2);
    int lastbreak = -1;
    int numbrk = 0;
    u16* ptr;

    memset(breaks, 0, sizeof(int)*64);

    for (int i = 0; text[i] != '\0'; )
	{
	    int glyphsize;
		if (text[i] == ' ')
		{
			glyphsize = 6;
		}
		else
        {
            u32 ch = text[i];
            if (ch < 0x10 || ch > 0x7E) ch = 0x7F;

            ptr = &::font[(ch-0x10) << 4];
            glyphsize = ptr[0];
            if (!glyphsize) glyphsize = 6;
            else            glyphsize += 2; // space around the character
        }

		w += glyphsize;
		if (w > maxw)
        {
            // wrap shit as needed
            if (text[i] == ' ')
            {
                if (numbrk >= 64) break;
                breaks[numbrk++] = i;
                i++;
            }
            else
            {
                int brk = osdFindBreakPoint(text, i);
                if (brk != lastbreak) i = brk;

                if (numbrk >= 64) break;
                breaks[numbrk++] = i;

                lastbreak = brk;
            }

            w = 0;
            h += 14;
        }
        else
            i++;

        if (w > totalw) totalw = w;
    }

    *width = totalw;
    *height = h;
}

unsigned int ScreenPanel::osdRainbowColor(int inc)
{
    // inspired from Acmlmboard

    if      (inc < 100) return 0xFFFF9B9B + (inc << 8);
    else if (inc < 200) return 0xFFFFFF9B - ((inc-100) << 16);
    else if (inc < 300) return 0xFF9BFF9B + (inc-200);
    else if (inc < 400) return 0xFF9BFFFF - ((inc-300) << 8);
    else if (inc < 500) return 0xFF9B9BFF + ((inc-400) << 16);
    else                return 0xFFFF9BFF - (inc-500);
}

void ScreenPanel::osdRenderItem(OSDItem* item)
{
    int w, h;
    int breaks[64];

    char* text = item->text;
    u32 color = item->color;

    bool rainbow = (color == 0);
    u32 rainbowinc;
    if (item->rainbowstart == -1)
    {
        u32 ticks = (u32) QDateTime::currentMSecsSinceEpoch();
        rainbowinc = ((text[0] * 17) + (ticks * 13)) % 600;
    }
    else
        rainbowinc = (u32)item->rainbowstart;

    color |= 0xFF000000;
    const u32 shadow = 0xE0000000;

    osdLayoutText(text, &w, &h, breaks);

    item->bitmap = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    u32* bitmap = (u32*)item->bitmap.bits();
    memset(bitmap, 0, w*h*sizeof(u32));

    int x = 0, y = 1;
    u32 maxw = ((QWidget*)this)->width() - (kOSDMargin*2);
    int curline = 0;
    u16* ptr;

    for (int i = 0; text[i] != '\0'; )
	{
	    int glyphsize;
		if (text[i] == ' ')
		{
			x += 6;
		}
		else
        {
            u32 ch = text[i];
            if (ch < 0x10 || ch > 0x7E) ch = 0x7F;

            ptr = &::font[(ch-0x10) << 4];
            int glyphsize = ptr[0];
            if (!glyphsize) x += 6;
            else
            {
                x++;

                if (rainbow)
                {
                    color = osdRainbowColor(rainbowinc);
                    rainbowinc = (rainbowinc + 30) % 600;
                }

                // draw character
                for (int cy = 0; cy < 12; cy++)
                {
                    u16 val = ptr[4+cy];

                    for (int cx = 0; cx < glyphsize; cx++)
                    {
                        if (val & (1<<cx))
                            bitmap[((y+cy) * w) + x+cx] = color;
                    }
                }

                x += glyphsize;
                x++;
            }
        }

		i++;
		if (breaks[curline] && i >= breaks[curline])
        {
            i = breaks[curline++];
            if (text[i] == ' ') i++;

            x = 0;
            y += 14;
        }
    }

    // shadow
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            u32 val;

            val = bitmap[(y * w) + x];
            if ((val >> 24) == 0xFF) continue;

            if (x > 0)   val  = bitmap[(y * w) + x-1];
            if (x < w-1) val |= bitmap[(y * w) + x+1];
            if (y > 0)
            {
                if (x > 0)   val |= bitmap[((y-1) * w) + x-1];
                val |= bitmap[((y-1) * w) + x];
                if (x < w-1) val |= bitmap[((y-1) * w) + x+1];
            }
            if (y < h-1)
            {
                if (x > 0)   val |= bitmap[((y+1) * w) + x-1];
                val |= bitmap[((y+1) * w) + x];
                if (x < w-1) val |= bitmap[((y+1) * w) + x+1];
            }

            if ((val >> 24) == 0xFF)
                bitmap[(y * w) + x] = shadow;
        }
    }

    item->rainbowend = (int)rainbowinc;
}

void ScreenPanel::osdDeleteItem(OSDItem* item)
{
}

void ScreenPanel::osdSetEnabled(bool enabled)
{
    osdMutex.lock();
    osdEnabled = enabled;
    osdMutex.unlock();
}

void ScreenPanel::osdAddMessage(unsigned int color, const char* text)
{
    if (!osdEnabled) return;

    osdMutex.lock();

    OSDItem item;

    item.id = (osdID++) & 0x7FFFFFFF;
    item.timestamp = QDateTime::currentMSecsSinceEpoch();
    strncpy(item.text, text, 255); item.text[255] = '\0';
    item.color = color;
    item.rendered = false;
    item.rainbowstart = -1;

    osdItems.push_back(item);

    osdMutex.unlock();
}

void ScreenPanel::osdUpdate()
{
    osdMutex.lock();

    qint64 tick_now = QDateTime::currentMSecsSinceEpoch();
    qint64 tick_min = tick_now - 2500;

    for (auto it = osdItems.begin(); it != osdItems.end(); )
    {
        OSDItem& item = *it;

        if ((!osdEnabled) || (item.timestamp < tick_min))
        {
            osdDeleteItem(&item);
            it = osdItems.erase(it);
            continue;
        }

        if (!item.rendered)
        {
            osdRenderItem(&item);
            item.rendered = true;
        }

        it++;
    }

    // render splashscreen text items if needed

    int rainbowinc = -1;
    bool needrecalc = false;

    for (int i = 0; i < 3; i++)
    {
        if (!splashText[i].rendered)
        {
            splashText[i].rainbowstart = rainbowinc;
            osdRenderItem(&splashText[i]);
            splashText[i].rendered = true;
            rainbowinc = splashText[i].rainbowend;
            needrecalc = true;
        }
    }

    osdMutex.unlock();

    if (needrecalc)
        calcSplashLayout();
}

void ScreenPanel::calcSplashLayout()
{
    if (!splashText[0].rendered)
        return;

    osdMutex.lock();

    int w = width();
    int h = height();

    int xlogo = (w - kLogoWidth) / 2;
    int ylogo = (h - kLogoWidth) / 2;

    // top text
    int totalwidth = splashText[0].bitmap.width() + 6 + splashText[1].bitmap.width();
    if (totalwidth >= w)
    {
        // stacked vertically
        splashPos[0].setX((width() - splashText[0].bitmap.width()) / 2);
        splashPos[1].setX((width() - splashText[1].bitmap.width()) / 2);

        int basey = ylogo / 2;
        splashPos[0].setY(basey - splashText[0].bitmap.height() - 1);
        splashPos[1].setY(basey + 1);
    }
    else
    {
        // horizontal
        splashPos[0].setX((w - totalwidth) / 2);
        splashPos[1].setX(splashPos[0].x() + splashText[0].bitmap.width() + 6);

        int basey = (ylogo - splashText[0].bitmap.height()) / 2;
        splashPos[0].setY(basey);
        splashPos[1].setY(basey);
    }

    // bottom text
    splashPos[2].setX((w - splashText[2].bitmap.width()) / 2);
    splashPos[2].setY(ylogo + kLogoWidth + ((ylogo - splashText[2].bitmap.height()) / 2));

    // logo
    splashPos[3].setX(xlogo);
    splashPos[3].setY(ylogo);

    osdMutex.unlock();
}



ScreenPanelNative::ScreenPanelNative(QWidget* parent) : ScreenPanel(parent)
{
    hasBuffers = false;

    screen[0] = QImage(256, 192, QImage::Format_RGB32);
    screen[1] = QImage(256, 192, QImage::Format_RGB32);

    screenTrans[0].reset();
    screenTrans[1].reset();

    // the plain display draws the pixel grid with the software filter (made when first needed)
}

ScreenPanelNative::~ScreenPanelNative()
{
}

void ScreenPanelNative::setupScreenLayout()
{
    ScreenPanel::setupScreenLayout();

    for (int i = 0; i < numScreens; i++)
    {
        float* mtx = screenMatrix[i];
        screenTrans[i].setMatrix(mtx[0], mtx[1], 0.f,
                                 mtx[2], mtx[3], 0.f,
                                 mtx[4], mtx[5], 1.f);
    }
}

void ScreenPanelNative::drawScreen()
{
    auto emuThread = emuInstance->getEmuThread();
    if (!emuThread->emuIsActive())
    {
        hasBuffers = false;
        return;
    }

    auto nds = emuInstance->getNDS();
    assert(nds != nullptr);

    bufferLock.lock();
    hasBuffers = nds->GPU.GetFramebuffers(&topBuffer, &bottomBuffer);
    bufferLock.unlock();
}

void ScreenPanelNative::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);

    // fill background
    painter.fillRect(event->rect(), QColor::fromRgb(0, 0, 0));

    auto emuThread = emuInstance->getEmuThread();
    
    if (emuThread->emuIsActive())
    {
        emuInstance->renderLock.lock();

        bufferLock.lock();
        if (hasBuffers)
        {
            memcpy(screen[0].scanLine(0), topBuffer, 256 * 192 * 4);
            memcpy(screen[1].scanLine(0), bottomBuffer, 256 * 192 * 4);
        }
        bufferLock.unlock();
        emuInstance->renderLock.unlock();

        QRect screenrc(0, 0, 256, 192);

        static_assert(kNumGridParams == 9 && kNumCrtParams == crt_Count, "the soft filter params below are filled by index");
        const int mode = screenFilter;     // Off: the plain picture
        const bool useSoft = mode != screenFilter_Off;
        if (useSoft)
        {
            // filter at the whole zoom closest to the screen's size in device pixels, so the
            // result is drawn 1:1 (grid lines, scanlines and mask stripes land on whole pixels,
            // as with the shaders)
            const QTransform& t = screenTrans[0];
            float scale = std::sqrt(float(t.m11() * t.m11() + t.m12() * t.m12())) * devicePixelRatioF();
            int n = std::clamp((int)std::lround(scale), 1, 16);
            for (int k = 0; k < 2; k++)
                if (filtered[k].width() != 256 * n)
                    filtered[k] = QImage(256 * n, 192 * n, QImage::Format_RGB32);
            // only the screens the layout shows
            unsigned layers = 0;
            for (int i = 0; i < numScreens; i++) layers |= 1u << screenKind[i];
            const uint32_t* src[2] = {(const uint32_t*)screen[0].constBits(), (const uint32_t*)screen[1].constBits()};
            uint32_t* dst[2] = {(uint32_t*)filtered[0].bits(), (uint32_t*)filtered[1].bits()};
            const int threads = softFilterThreads(emuInstance->getLocalConfig());

            if (mode == screenFilter_LCD)
            {
                if (!lcdFilter) lcdFilter = std::make_unique<SoftLcdFilter>(threads);
                SoftFilterParams p;
                p.grid = true;
                p.gridDark = gridParam[0]; p.gridDarkH = gridParam[1]; p.gridWidth = gridParam[2]; p.gridSoft = gridParam[3];
                p.subStrength = gridParam[4]; p.subKeep = gridParam[5];
                p.blurX = gridParam[6]; p.blurY = gridParam[7]; p.halo = gridParam[8];
                p.blurReachX = blurReach[0]; p.blurReachY = blurReach[1];
                p.subpixel = subpixelLayout;
                lcdFilter->process(src, dst, (int)filtered[0].bytesPerLine(), n, p, layers);
            }
            else
            {
                if (!crtFilter) crtFilter = std::make_unique<SoftCrtFilter>(threads);
                SoftCrtParams p;
                p.curvature = crtParam[crt_Curvature]; p.lumaBlur = crtParam[crt_LumaBlur]; p.chromaBlur = crtParam[crt_ChromaBlur];
                p.beamMin = crtParam[crt_BeamMin]; p.beamMax = crtParam[crt_BeamMax];
                p.mask = crtParam[crt_Mask]; p.maskPx = crtParam[crt_MaskPx];
                p.glow = crtParam[crt_Glow]; p.vignette = crtParam[crt_Vignette]; p.boost = crtParam[crt_Boost];
                p.signal = signalSettings();
                p.frame = emuInstance->getNDS() ? emuInstance->getNDS()->NumFrames : 0;
                crtFilter->process(src, dst, (int)filtered[0].bytesPerLine(), n, p, layers);
            }
        }

        for (int i = 0; i < numScreens; i++)
        {
            painter.setTransform(screenTrans[i]);
            painter.drawImage(screenrc, useSoft ? filtered[screenKind[i]] : screen[screenKind[i]]);
        }
#ifdef MELONDS_TRACE_QT
        if (hasBuffers && mainWindow->isVisible() && !shownTraced)
        {
            shownTraced = true;
            if (mainWindow->isEmbedded()) TRACE_QT_MESSAGE("first frame shown (workbench game screen)");
            else TRACE_QT_MESSAGE("first frame shown (main window)");
        }
#endif
    }

    osdUpdate();

    if (!emuThread->emuIsActive())
    {
        // splashscreen
        osdMutex.lock();

        painter.drawPixmap(QRect(splashPos[3], QSize(kLogoWidth, kLogoWidth)), splashLogo);

        for (int i = 0; i < 3; i++)
            painter.drawImage(splashPos[i], splashText[i].bitmap);

        osdMutex.unlock();
    }

    if (osdEnabled)
    {
        osdMutex.lock();

        u32 y = kOSDMargin;

        painter.resetTransform();

        for (auto it = osdItems.begin(); it != osdItems.end(); )
        {
            OSDItem& item = *it;

            painter.drawImage(kOSDMargin, y, item.bitmap);

            y += item.bitmap.height();
            it++;
        }

        osdMutex.unlock();
    }
}



ScreenPanelGL::ScreenPanelGL(QWidget* parent) : ScreenPanel(parent)
{
    setAutoFillBackground(false);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_KeyCompression, false);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(screenGetMinSize());

    glInited = false;
}

ScreenPanelGL::~ScreenPanelGL()
{}

bool ScreenPanelGL::createContext()
{
    std::optional<WindowInfo> windowinfo = getWindowInfo();

    // if our parent window is parented to another window, we will
    // share our OpenGL context with that window
    MainWindow* ourwin = (MainWindow*)parentWidget();
    // share with the instance's main window: a secondary window's parent widget is usually that window,
    // but an embedded one (the decomp workbench's game screen) sits in another widget
    MainWindow* parentwin = emuInstance->getMainWindow();
    //if (parentwin)
    if (ourwin->getWindowID() != 0)
    {
        if (windowinfo.has_value())
            if ((glContext = parentwin->getOGLContext()->CreateSharedContext(*windowinfo)))
                glContext->DoneCurrent();
    }
    else
    {
        std::array<GL::Context::Version, 2> versionsToTry = {
                GL::Context::Version{GL::Context::Profile::Core, 4, 3},
                GL::Context::Version{GL::Context::Profile::Core, 3, 2}};
        if (windowinfo.has_value())
            if ((glContext = GL::Context::Create(*windowinfo, versionsToTry)))
                glContext->DoneCurrent();
    }

    return glContext != nullptr;
}

void ScreenPanelGL::setSwapInterval(int intv)
{
    if (!glContext) return;

    glContext->SetSwapInterval(intv);
}

void ScreenPanelGL::initOpenGL()
{
    if (!glContext) return;
    if (glInited) return;

    glContext->MakeCurrent();

    OpenGL::CompileVertexFragmentProgram(screenShaderProgram,
                                         kScreenVS, kScreenFS,
                                         "ScreenShader",
                                         {{"vPosition", 0}, {"vTexcoord", 1}},
                                         {{"oColor", 0}});

    glUseProgram(screenShaderProgram);
    glUniform1i(glGetUniformLocation(screenShaderProgram, "TopScreenTex"), 0);
    glUniform1i(glGetUniformLocation(screenShaderProgram, "BottomScreenTex"), 1);

    screenShaderScreenSizeULoc = glGetUniformLocation(screenShaderProgram, "uScreenSize");
    screenShaderGridULoc = glGetUniformLocation(screenShaderProgram, "uGrid");
    screenShaderSubpixelULoc = glGetUniformLocation(screenShaderProgram, "uSubpixel");
    screenShaderSubSampleULoc = glGetUniformLocation(screenShaderProgram, "uSubSample");
    screenShaderBlurReachULoc = glGetUniformLocation(screenShaderProgram, "uBlurReach");
    for (int i = 0; i < kNumGridParams; i++)
        screenShaderGridParamULoc[i] = glGetUniformLocation(screenShaderProgram, kGridParams[i].uniform);
    screenShaderTransformULoc = glGetUniformLocation(screenShaderProgram, "uTransform");

    // CRT filter: prepass at DS resolution, then the main pass with the same vertex shader
    OpenGL::CompileVertexFragmentProgram(crtPrepassProgram,
                                         kCrtPrepassVS, kCrtPrepassFS,
                                         "CrtPrepassShader",
                                         {},
                                         {{"oYiq", 0}, {"oGlow", 1}});
    glUseProgram(crtPrepassProgram);
    glUniform1i(glGetUniformLocation(crtPrepassProgram, "ScreenTex"), 0);
    crtPrepassLayerULoc = glGetUniformLocation(crtPrepassProgram, "uLayer");

    OpenGL::CompileVertexFragmentProgram(crtProgram,
                                         kScreenVS, kCrtFS,
                                         "CrtShader",
                                         {{"vPosition", 0}, {"vTexcoord", 1}},
                                         {{"oColor", 0}});
    glUseProgram(crtProgram);
    glUniform1i(glGetUniformLocation(crtProgram, "YiqTex"), 1);
    glUniform1i(glGetUniformLocation(crtProgram, "GlowTex"), 2);
    crtScreenSizeULoc = glGetUniformLocation(crtProgram, "uScreenSize");
    crtTransformULoc = glGetUniformLocation(crtProgram, "uTransform");
    for (int i = 0; i < kNumCrtParams; i++)   // the signal chain's settings have no uniform
        crtParamULoc[i] = kCrtParams[i].uniform ? glGetUniformLocation(crtProgram, kCrtParams[i].uniform) : -1;

    // the prepass targets: one layer per screen. The signal is read texel by texel (nearest);
    // the glow is read bilinearly between DS pixel centres
    GLuint crtTex[2];
    glGenTextures(2, crtTex);
    crtYiqTexture = crtTex[0];
    crtGlowTexture = crtTex[1];
    for (int i = 0; i < 2; i++)
    {
        glBindTexture(GL_TEXTURE_2D_ARRAY, crtTex[i]);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, i ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, i ? GL_LINEAR : GL_NEAREST);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA16F, 256, 192, 2, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    glGenFramebuffers(1, &crtFramebuffer);

    // video signal chain: the decode pass, and the main pass's uniforms for reading its lines
    OpenGL::CompileVertexFragmentProgram(crtDecodeProgram,
                                         kCrtPrepassVS, kCrtDecodeFS,
                                         "CrtDecodeShader",
                                         {},
                                         {{"oColor", 0}});
    glUseProgram(crtDecodeProgram);
    glUniform1i(glGetUniformLocation(crtDecodeProgram, "ScreenTex"), 0);
    glUniform1i(glGetUniformLocation(crtDecodeProgram, "KernelTex"), 4);
    glUniform1i(glGetUniformLocation(crtDecodeProgram, "NoiseTex"), 5);
    {
        auto u = [this](const char* n) { return glGetUniformLocation(crtDecodeProgram, n); };
        crtDecLoc = {u("uLayer"), u("uPixelClocks"), u("uOutStep"), u("uOutPhases"), u("uTaps"), u("uTapLeft"),
                     u("uCarrier"), u("uLineStep"), u("uFramePhase"), u("uLinePhaseOf"), u("uBias"), u("uNoise"),
                     u("uNoiseLines"), u("uFrame"), u("uFrameSwitch"), u("uRows"), u("uPal")};
    }
    glUseProgram(crtProgram);
    glUniform1i(glGetUniformLocation(crtProgram, "DecodedTex"), 3);
    crtSignalULoc = glGetUniformLocation(crtProgram, "uSignal");
    crtSigPixelClocksULoc = glGetUniformLocation(crtProgram, "uSigPixelClocks");
    crtSigStepULoc = glGetUniformLocation(crtProgram, "uSigStep");
    crtSigHalfStepULoc = glGetUniformLocation(crtProgram, "uSigHalfStep");
    crtSigWidthULoc = glGetUniformLocation(crtProgram, "uSigWidth");
    crtSigTexWidthULoc = glGetUniformLocation(crtProgram, "uSigTexWidth");

    GLuint decTex[3];
    glGenTextures(3, decTex);
    crtDecodedTexture = decTex[0]; crtKernelTexture = decTex[1]; crtNoiseTexture = decTex[2];
    glBindTexture(GL_TEXTURE_2D_ARRAY, crtDecodedTexture);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA16F, kDecodedWidth, 192, 2, 0, GL_RGBA, GL_FLOAT, nullptr);
    for (GLuint t : {crtKernelTexture, crtNoiseTexture})
    {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glGenFramebuffers(1, &crtDecodeFramebuffer);
    crtChainValid = false;

    const float vertices[] =
    {
        0.f,   0.f,    0.f, 0.f, 0.f,
        0.f,   192.f,  0.f, 1.f, 0.f,
        256.f, 192.f,  1.f, 1.f, 0.f,
        0.f,   0.f,    0.f, 0.f, 0.f,
        256.f, 192.f,  1.f, 1.f, 0.f,
        256.f, 0.f,    1.f, 0.f, 0.f,

        0.f,   0.f,    0.f, 0.f, 1.f,
        0.f,   192.f,  0.f, 1.f, 1.f,
        256.f, 192.f,  1.f, 1.f, 1.f,
        0.f,   0.f,    0.f, 0.f, 1.f,
        256.f, 192.f,  1.f, 1.f, 1.f,
        256.f, 0.f,    1.f, 0.f, 1.f
    };

    glGenBuffers(1, &screenVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, screenVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &screenVertexArray);
    glBindVertexArray(screenVertexArray);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5*4, (void*)(0));
    glEnableVertexAttribArray(1); // texcoord
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5*4, (void*)(2*4));

    glGenTextures(1, &screenTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, screenTexture);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, 256, 192, 2, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);


    OpenGL::CompileVertexFragmentProgram(osdShader,
                                         kScreenVS_OSD, kScreenFS_OSD,
                                         "OSDShader",
                                         {{"vPosition", 0}},
                                         {{"oColor", 0}});

    glUseProgram(osdShader);
    glUniform1i(glGetUniformLocation(osdShader, "OSDTex"), 0);

    osdScreenSizeULoc = glGetUniformLocation(osdShader, "uScreenSize");
    osdPosULoc = glGetUniformLocation(osdShader, "uOSDPos");
    osdSizeULoc = glGetUniformLocation(osdShader, "uOSDSize");
    osdScaleFactorULoc = glGetUniformLocation(osdShader, "uScaleFactor");
    osdTexScaleULoc = glGetUniformLocation(osdShader, "uTexScale");

    const float osdvertices[6*2] =
    {
        0, 0,
        1, 1,
        1, 0,
        0, 0,
        0, 1,
        1, 1
    };

    glGenBuffers(1, &osdVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, osdVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(osdvertices), osdvertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &osdVertexArray);
    glBindVertexArray(osdVertexArray);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)(0));

    // splash logo texture
    QImage logo = splashLogo.scaled(kLogoWidth*2, kLogoWidth*2).toImage();
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo.width(), logo.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, logo.bits());
    logoTexture = tex;

    transferLayout();
    glInited = true;
}

void ScreenPanelGL::deinitOpenGL()
{
    if (!glContext) return;
    if (!glInited) return;

    glContext->MakeCurrent();

    glDeleteTextures(1, &screenTexture);

    glDeleteVertexArrays(1, &screenVertexArray);
    glDeleteBuffers(1, &screenVertexBuffer);

    glDeleteProgram(screenShaderProgram);

    glDeleteFramebuffers(1, &crtFramebuffer);
    glDeleteTextures(1, &crtYiqTexture);
    glDeleteTextures(1, &crtGlowTexture);
    glDeleteProgram(crtPrepassProgram);
    glDeleteProgram(crtProgram);
    glDeleteProgram(crtDecodeProgram);
    glDeleteFramebuffers(1, &crtDecodeFramebuffer);
    glDeleteTextures(1, &crtDecodedTexture);
    glDeleteTextures(1, &crtKernelTexture);
    glDeleteTextures(1, &crtNoiseTexture);
    crtChainValid = false;

    for (const auto& [key, tex] : osdTextures)
    {
        glDeleteTextures(1, &tex);
    }
    osdTextures.clear();

    glDeleteVertexArrays(1, &osdVertexArray);
    glDeleteBuffers(1, &osdVertexBuffer);

    glDeleteTextures(1, &logoTexture);

    glDeleteProgram(osdShader);


    glContext->DoneCurrent();

    lastScreenWidth = lastScreenHeight = -1;
    glInited = false;
}

void ScreenPanelGL::makeCurrentGL()
{
    if (!glContext) return;

    glContext->MakeCurrent();
}

void ScreenPanelGL::releaseGL()
{
    if (!glContext) return;

    glContext->DoneCurrent();
}

void ScreenPanelGL::osdRenderItem(OSDItem* item)
{
    ScreenPanel::osdRenderItem(item);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, item->bitmap.width(), item->bitmap.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, item->bitmap.bits());

    osdTextures[item->id] = tex;
}

void ScreenPanelGL::osdDeleteItem(OSDItem* item)
{
    if (osdTextures.count(item->id))
    {
        GLuint tex = osdTextures[item->id];
        glDeleteTextures(1, &tex);
        osdTextures.erase(item->id);
    }

    ScreenPanel::osdDeleteItem(item);
}

// CRT filter prepass: per shown screen, the YIQ signal and the glow at DS resolution, into a layer
// of crtYiqTexture / crtGlowTexture. The screen texture is bound on unit 0 and a vertex array is
// bound (the prepass draws one triangle without vertex data). Leaves the prepass framebuffer bound.
void ScreenPanelGL::drawCrtPrepass(unsigned layers)
{
    glBindFramebuffer(GL_FRAMEBUFFER, crtFramebuffer);
    glViewport(0, 0, 256, 192);
    glUseProgram(crtPrepassProgram);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    // the OpenGL renderer shares this context and leaves per-buffer write masks set (buffer 1 often
    // masked off), which would keep the glow from being written: open both, then put them back
    GLboolean mask[2][4];
    for (int b = 0; b < 2; b++) glGetBooleani_v(GL_COLOR_WRITEMASK, b, mask[b]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (int l = 0; l < 2; l++)
    {
        if (!(layers & (1u << l))) continue;
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, crtYiqTexture, 0, l);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, crtGlowTexture, 0, l);
        glUniform1f(crtPrepassLayerULoc, (float)l);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    for (int b = 0; b < 2; b++) glColorMaski(b, mask[b][0], mask[b][1], mask[b][2], mask[b][3]);
}

// a texture unit's binding, put back when this goes out of scope (the OpenGL renderer shares the
// context and doesn't always rebind what it reads)
struct GLBindingKeeper
{
    GLenum unit, target;
    GLint prev = 0;
    GLBindingKeeper(GLenum u, GLenum t, GLenum query) : unit(u), target(t)
    {
        glActiveTexture(unit);
        glGetIntegerv(query, &prev);
    }
    ~GLBindingKeeper()
    {
        glActiveTexture(unit);
        glBindTexture(target, (GLuint)prev);
        glActiveTexture(GL_TEXTURE0);
    }
};

// The signal chain's kernel and decoded noise, as textures; again only when the settings changed
void ScreenPanelGL::uploadCrtChain()
{
    if (crtChainValid && crtChainUploaded == crtChain.generation()) return;
    crtChainUploaded = crtChain.generation();
    crtChainValid = true;

    // kernel: one RGBA texel per (tap, output channel) holding its weights for input R, G, B
    const int taps = crtChain.taps(), rows = crtChain.linePhases() * crtChain.outPhases() * crtChain.rows();
    const std::vector<float>& wt = crtChain.weights();
    std::vector<float> tex((size_t)taps * 3 * rows * 4, 0.f);
    for (int r = 0; r < rows; r++)
        for (int k = 0; k < taps; k++)
            for (int co = 0; co < 3; co++)
                for (int ci = 0; ci < 3; ci++)
                    tex[(((size_t)r * taps + k) * 3 + co) * 4 + ci] = wt[((size_t)r * taps + k) * 9 + co * 3 + ci];
    {
        GLBindingKeeper keep(GL_TEXTURE4, GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D);
        glBindTexture(GL_TEXTURE_2D, crtKernelTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, taps * 3, rows, 0, GL_RGBA, GL_FLOAT, tex.data());
    }

    // decoded noise lines (RF), or one empty line
    const int nl = std::max(crtChain.noiseLines(), 1), ow = crtChain.outWidth();
    std::vector<float> nz((size_t)ow * nl * 4, 0.f);
    if (crtChain.noiseLines())
        for (size_t i = 0; i < (size_t)ow * nl; i++)
            for (int c = 0; c < 3; c++) nz[i * 4 + c] = crtChain.noise()[i * 3 + c];
    {
        GLBindingKeeper keep(GL_TEXTURE5, GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D);
        glBindTexture(GL_TEXTURE_2D, crtNoiseTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, ow, nl, 0, GL_RGBA, GL_FLOAT, nz.data());
    }
}

// The signal chain's decode pass: each shown screen's lines as the TV decodes them, into a layer of
// crtDecodedTexture. The screen texture is bound on unit 0 and a vertex array is bound.
void ScreenPanelGL::drawCrtDecode(unsigned layers, uint32_t frame)
{
    GLBindingKeeper keepK(GL_TEXTURE4, GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D);
    GLBindingKeeper keepN(GL_TEXTURE5, GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, crtKernelTexture);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, crtNoiseTexture);
    glActiveTexture(GL_TEXTURE0);

    const Ntsc::Timing& tm = Ntsc::timing(crtChain.settings().console);
    glBindFramebuffer(GL_FRAMEBUFFER, crtDecodeFramebuffer);
    glViewport(0, 0, crtChain.outWidth(), 192);
    glUseProgram(crtDecodeProgram);
    const GLenum buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buf);
    glUniform1i(crtDecLoc.pixelClocks, tm.pixelClocks);
    glUniform1i(crtDecLoc.outStep, tm.outStep);
    glUniform1i(crtDecLoc.outPhases, crtChain.outPhases());
    glUniform1i(crtDecLoc.taps, crtChain.taps());
    glUniform1i(crtDecLoc.tapLeft, crtChain.tapLeft());
    glUniform1i(crtDecLoc.carrier, crtChain.carrierClocks());
    glUniform1i(crtDecLoc.lineStep, crtChain.lineClockStep());
    glUniform1i(crtDecLoc.framePhase, crtChain.frameClockPhase(frame));
    glUniform1i(crtDecLoc.frameSwitch, crtChain.frameSwitch(frame));
    glUniform1i(crtDecLoc.rows, crtChain.rows());
    glUniform1i(crtDecLoc.pal, Ntsc::isPal(crtChain.settings().console) ? 1 : 0);
    GLint phaseOf[64] = {};
    for (int c = 0; c < 2 * crtChain.carrierClocks() && c < 64; c++) phaseOf[c] = crtChain.linePhaseOfClock(c);
    glUniform1iv(crtDecLoc.linePhaseOf, 64, phaseOf);
    glUniform3fv(crtDecLoc.bias, 1, crtChain.bias());
    glUniform1f(crtDecLoc.noise, crtChain.settings().noise);
    glUniform1i(crtDecLoc.noiseLines, crtChain.settings().noise > 0 ? crtChain.noiseLines() : 0);
    glUniform1ui(crtDecLoc.frame, frame);

    // the renderer's write masks, opened for this pass and put back after (see drawCrtPrepass)
    GLboolean mask[2][4];
    for (int b = 0; b < 2; b++) glGetBooleani_v(GL_COLOR_WRITEMASK, b, mask[b]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (int l = 0; l < 2; l++)
    {
        if (!(layers & (1u << l))) continue;
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, crtDecodedTexture, 0, l);
        glUniform1f(crtDecLoc.layer, (float)l);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    for (int b = 0; b < 2; b++) glColorMaski(b, mask[b][0], mask[b][1], mask[b][2], mask[b][3]);
}

void ScreenPanelGL::drawScreen()
{
    if (!glContext) return;

    auto emuThread = emuInstance->getEmuThread();

    glContext->MakeCurrent();

    int w = windowInfo.surface_width;
    int h = windowInfo.surface_height;
    float factor = windowInfo.surface_scale;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(0, 0, w, h);

    if (emuThread->emuIsActive())
    {
        auto nds = emuInstance->getNDS();

        void* topbuf; void* bottombuf;
        if (nds->GPU.GetFramebuffers(&topbuf, &bottombuf))
        {
            // if we're doing a regular render, use the provided framebuffers
            // otherwise, GetFramebuffers() will set up the required state

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, screenTexture);

            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 256, 192, 1, GL_BGRA,
                            GL_UNSIGNED_BYTE, topbuf);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 256, 192, 1, GL_BGRA,
                            GL_UNSIGNED_BYTE, bottombuf);
        }
        else
        {
            GLuint texid = *(GLuint*)topbuf;

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, texid);
        }

        screenSettingsLock.lock();

        GLint filter = this->filter ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, filter);

        glBindBuffer(GL_ARRAY_BUFFER, screenVertexBuffer);
        glBindVertexArray(screenVertexArray);

        GLint transformLoc;
        const int mode = screenFilter;   // once: the GUI thread may change it meanwhile
        if (mode == screenFilter_CRT)
        {
            unsigned layers = 0;   // only the screens the layout shows
            for (int i = 0; i < numScreens; i++) layers |= 1u << screenKind[i];
            drawCrtPrepass(layers);
            // the video signal chain, when a console is chosen: decode the shown screens' lines
            crtChain.build(signalSettings());   // cheap when nothing changed
            const bool chain = crtChain.active();
            if (chain)
            {
                uploadCrtChain();
                drawCrtDecode(layers, nds->NumFrames);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, w, h);

            glUseProgram(crtProgram);
            glUniform2f(crtScreenSizeULoc, w / factor, h / factor);
            for (int i = 0; i < kNumCrtParams; i++)
                glUniform1f(crtParamULoc[i], crtParam[i]);
            glUniform1i(crtSignalULoc, chain ? 1 : 0);
            if (chain)
            {
                const Ntsc::Timing& tm = Ntsc::timing(crtChain.settings().console);
                glUniform1f(crtSigPixelClocksULoc, (float)tm.pixelClocks);
                glUniform1f(crtSigStepULoc, (float)tm.outStep);
                glUniform1f(crtSigHalfStepULoc, (float)(tm.outStep / 2));
                glUniform1f(crtSigWidthULoc, (float)crtChain.outWidth());
                glUniform1f(crtSigTexWidthULoc, (float)kDecodedWidth);
            }
            // the OpenGL renderer shares this context, and its final pass reads array textures
            // on units 1 and 2 that it doesn't always rebind: put back what was there after drawing
            // (unit 3, the decoded lines, likewise)
            for (int u = 0; u < 3; u++)
            {
                glActiveTexture(GL_TEXTURE1 + u);
                glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &crtSavedBinding[u]);
                glBindTexture(GL_TEXTURE_2D_ARRAY, u == 0 ? crtYiqTexture : u == 1 ? crtGlowTexture : crtDecodedTexture);
            }
            glActiveTexture(GL_TEXTURE0);
            transformLoc = crtTransformULoc;
        }
        else
        {
            // LCD filter, or Off (the shader then shows the plain picture)
            glUseProgram(screenShaderProgram);
            glUniform2f(screenShaderScreenSizeULoc, w / factor, h / factor);
            glUniform1i(screenShaderGridULoc, mode == screenFilter_LCD ? 1 : 0);
            for (int i = 0; i < kNumGridParams; i++)
                glUniform1f(screenShaderGridParamULoc[i], gridParam[i]);
            glUniform1i(screenShaderSubpixelULoc, subpixelLayout);
            glUniform1i(screenShaderSubSampleULoc, subpixelSample ? 1 : 0);
            glUniform2i(screenShaderBlurReachULoc, blurReach[0], blurReach[1]);
            transformLoc = screenShaderTransformULoc;
        }

        for (int i = 0; i < numScreens; i++)
        {
            glUniformMatrix2x3fv(transformLoc, 1, GL_TRUE, screenMatrix[i]);
            glDrawArrays(GL_TRIANGLES, screenKind[i] == 0 ? 0 : 2 * 3, 2 * 3);
        }
        if (mode == screenFilter_CRT)
        {
            for (int u = 0; u < 3; u++)
            {
                glActiveTexture(GL_TEXTURE1 + u);
                glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)crtSavedBinding[u]);
            }
            glActiveTexture(GL_TEXTURE0);
        }

        screenSettingsLock.unlock();
    }

    osdUpdate();

    if (!emuThread->emuIsActive())
    {
        // splashscreen
        osdMutex.lock();

        glUseProgram(osdShader);

        glUniform2f(osdScreenSizeULoc, w, h);
        glUniform1f(osdScaleFactorULoc, factor);
        glUniform1f(osdTexScaleULoc, 2.0);

        glBindBuffer(GL_ARRAY_BUFFER, osdVertexBuffer);
        glBindVertexArray(osdVertexArray);

        glActiveTexture(GL_TEXTURE0);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glBindTexture(GL_TEXTURE_2D, logoTexture);
        glUniform2i(osdPosULoc, splashPos[3].x(), splashPos[3].y());
        glUniform2i(osdSizeULoc, kLogoWidth, kLogoWidth);
        glDrawArrays(GL_TRIANGLES, 0, 2*3);

        glUniform1f(osdTexScaleULoc, 1.0);

        for (int i = 0; i < 3; i++)
        {
            OSDItem& item = splashText[i];

            if (!osdTextures.count(item.id))
                continue;

            glBindTexture(GL_TEXTURE_2D, osdTextures[item.id]);
            glUniform2i(osdPosULoc, splashPos[i].x(), splashPos[i].y());
            glUniform2i(osdSizeULoc, item.bitmap.width(), item.bitmap.height());
            glDrawArrays(GL_TRIANGLES, 0, 2*3);
        }

        glDisable(GL_BLEND);
        glUseProgram(0);

        osdMutex.unlock();
    }

    if (osdEnabled)
    {
        osdMutex.lock();

        u32 y = kOSDMargin;

        glUseProgram(osdShader);

        glUniform2f(osdScreenSizeULoc, w, h);
        glUniform1f(osdScaleFactorULoc, factor);
        glUniform1f(osdTexScaleULoc, 1.0);

        glBindBuffer(GL_ARRAY_BUFFER, osdVertexBuffer);
        glBindVertexArray(osdVertexArray);

        glActiveTexture(GL_TEXTURE0);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        for (auto it = osdItems.begin(); it != osdItems.end(); )
        {
            OSDItem& item = *it;

            if (!osdTextures.count(item.id))
                continue;

            glBindTexture(GL_TEXTURE_2D, osdTextures[item.id]);
            glUniform2i(osdPosULoc, kOSDMargin, y);
            glUniform2i(osdSizeULoc, item.bitmap.width(), item.bitmap.height());
            glDrawArrays(GL_TRIANGLES, 0, 2*3);

            y += item.bitmap.height();
            it++;
        }

        glDisable(GL_BLEND);
        glUseProgram(0);

        osdMutex.unlock();
    }

    glContext->SwapBuffers();
#ifdef MELONDS_TRACE_QT
    // landmarks: the first frame on a window that is on screen (the hidden main window of -wo doesn't count)
    if (emuThread->emuIsActive() && mainWindow->isVisible() && !shownTraced)
    {
        shownTraced = true;
        if (mainWindow->isEmbedded()) TRACE_QT_MESSAGE("first frame shown (workbench game screen)");
        else TRACE_QT_MESSAGE("first frame shown (main window)");
    }
#endif
}

qreal ScreenPanelGL::devicePixelRatioFromScreen() const
{
    const QScreen* screen_for_ratio = window()->windowHandle()->screen();
    if (!screen_for_ratio)
        screen_for_ratio = QGuiApplication::primaryScreen();

    return screen_for_ratio ? screen_for_ratio->devicePixelRatio() : static_cast<qreal>(1);
}

int ScreenPanelGL::scaledWindowWidth() const
{
  return std::max(static_cast<int>(std::ceil(static_cast<qreal>(width()) * devicePixelRatioFromScreen())), 1);
}

int ScreenPanelGL::scaledWindowHeight() const
{
  return std::max(static_cast<int>(std::ceil(static_cast<qreal>(height()) * devicePixelRatioFromScreen())), 1);
}

std::optional<WindowInfo> ScreenPanelGL::getWindowInfo()
{
    WindowInfo wi;

    // Windows and Apple are easy here since there's no display connection.
    #if defined(_WIN32)
    wi.type = WindowInfo::Type::Win32;
    wi.window_handle = reinterpret_cast<void*>(winId());
    #elif defined(__APPLE__)
    wi.type = WindowInfo::Type::MacOS;
    wi.window_handle = reinterpret_cast<void*>(winId());
    #else
    const QString platform_name = QGuiApplication::platformName();

    #if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (platform_name == QStringLiteral("xcb"))
    {
        wi.type = WindowInfo::Type::X11;
        const QX11Application* x11 = qApp->nativeInterface<QX11Application>();
        wi.display_connection = x11->display();
        wi.window_handle = reinterpret_cast<void*>(winId());
    }
    #if defined(WAYLAND_ENABLED)
    else if (platform_name == QStringLiteral("wayland"))
    {
        wi.type = WindowInfo::Type::Wayland;
        const QWaylandApplication* wl = qApp->nativeInterface<QWaylandApplication>();
        wi.display_connection = wl->display();
        wi.window_handle = reinterpret_cast<void*>(winId());
    }
    #endif
    #else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    if (platform_name == QStringLiteral("xcb"))
    {
        wi.type = WindowInfo::Type::X11;
        wi.display_connection = pni->nativeResourceForWindow("display", windowHandle());
        wi.window_handle = reinterpret_cast<void*>(winId());
    }
    else if (platform_name == QStringLiteral("wayland"))
    {
        wi.type = WindowInfo::Type::Wayland;
        QWindow* handle = windowHandle();
        if (handle == nullptr)
            return std::nullopt;

        wi.display_connection = pni->nativeResourceForWindow("display", handle);
        wi.window_handle = pni->nativeResourceForWindow("surface", handle);
    }
    #endif
    else
    {
        Platform::Log(Platform::LogLevel::Error, "Unknown PNI platform %s\n", platform_name.toStdString().c_str());
        return std::nullopt;
    }
    #endif

    wi.surface_width = static_cast<u32>(scaledWindowWidth());
    wi.surface_height = static_cast<u32>(scaledWindowHeight());
    wi.surface_scale = static_cast<float>(devicePixelRatioFromScreen());

    return wi;
}


QPaintEngine* ScreenPanelGL::paintEngine() const
{
  return nullptr;
}

void ScreenPanelGL::setupScreenLayout()
{
    ScreenPanel::setupScreenLayout();
    transferLayout();
}

void ScreenPanelGL::transferLayout()
{
    std::optional<WindowInfo> windowInfo = getWindowInfo();
    if (windowInfo.has_value())
    {
        screenSettingsLock.lock();

        if (lastScreenWidth != windowInfo->surface_width || lastScreenHeight != windowInfo->surface_height)
        {
            if (glContext)
                glContext->ResizeSurface(windowInfo->surface_width, windowInfo->surface_height);
            lastScreenWidth = windowInfo->surface_width;
            lastScreenHeight = windowInfo->surface_height;
        }

        this->windowInfo = *windowInfo;

        screenSettingsLock.unlock();
    }
}
