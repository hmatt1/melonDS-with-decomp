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

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <string>

#include <QApplication>
#include <QStyle>
#include <QMessageBox>
#include <QMenuBar>
#include <QFileDialog>
#include <QInputDialog>
#include <QPainter>
#include <QKeyEvent>
#include <QMimeData>
#include <QVector>
#include <QCommandLineParser>
#include <QStandardPaths>
#ifndef _WIN32
#include <QGuiApplication>
#include <QSocketNotifier>
#include <unistd.h>
#include <sys/socket.h>
#include <csignal>
#endif

#include <SDL2/SDL.h>

#include "OpenGLSupport.h"
#include "duckstation/gl/context.h"

#include "main.h"
#include "Trace.h"
#ifdef MELONDS_TRACE_QT
#include <QMetaEnum>
#endif
#include "version.h"

#include "Config.h"

#include "EmuInstance.h"
#include "workbench/WBHost.h"
#include "ArchiveUtil.h"
#include "CameraManager.h"
#include "MPInterface.h"
#include "Net.h"

#include "CLI.h"

#include "Net_PCap.h"
#include "Net_Slirp.h"

using namespace melonDS;

QString* systemThemeName;


QString emuDirectory;

const int kMaxEmuInstances = 16;
EmuInstance* emuInstances[kMaxEmuInstances];

CameraManager* camManager[2];
bool camStarted[2];

std::optional<LibPCap> pcap;
Net net;


QElapsedTimer sysTimer;


void NetInit()
{
    Config::Table cfg = Config::GetGlobalTable();
    if (cfg.GetBool("LAN.DirectMode"))
    {
        if (!pcap)
            pcap = LibPCap::New();

        if (pcap)
        {
            std::string devicename = cfg.GetString("LAN.Device");
            std::unique_ptr<Net_PCap> netPcap = pcap->Open(devicename, [](const u8* data, int len) {
                net.RXEnqueue(data, len);
            });

            if (netPcap)
            {
                net.SetDriver(std::move(netPcap));
            }
        }
    }
    else
    {
        net.SetDriver(std::make_unique<Net_Slirp>([](const u8* data, int len) {
            net.RXEnqueue(data, len);
        }));
    }
}


bool createEmuInstance()
{
    int id = -1;
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (!emuInstances[i])
        {
            id = i;
            break;
        }
    }

    if (id == -1)
        return false;

    auto inst = new EmuInstance(id);
    emuInstances[id] = inst;

    return true;
}

void deleteEmuInstance(int id)
{
    auto inst = emuInstances[id];
    if (!inst) return;

    delete inst;
    emuInstances[id] = nullptr;
}

void deleteAllEmuInstances(int first)
{
    for (int i = first; i < kMaxEmuInstances; i++)
        deleteEmuInstance(i);
}

int numEmuInstances()
{
    int ret = 0;

    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (emuInstances[i])
            ret++;
    }

    return ret;
}


void broadcastInstanceCommand(int cmd, QVariant& param, int sourceinst)
{
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (i == sourceinst) continue;
        if (!emuInstances[i]) continue;

        emuInstances[i]->handleCommand(cmd, param);
    }
}


void pathInit()
{
    // First, check for the portable directory next to the executable.
    QString appdirpath = QCoreApplication::applicationDirPath();
    QString portablepath = appdirpath + QDir::separator() + "portable";

#if defined(__APPLE__)
    // On Apple platforms we may need to navigate outside an app bundle.
    // The executable directory would be "melonDS.app/Contents/MacOS", so we need to go a total of three steps up.
    QDir bundledir(appdirpath);
    if (bundledir.cd("..") && bundledir.cd("..") && bundledir.dirName().endsWith(".app") && bundledir.cd(".."))
    {
        portablepath = bundledir.absolutePath() + QDir::separator() + "portable";
    }
#endif

    QDir portabledir(portablepath);
    if (portabledir.exists())
    {
        emuDirectory = portabledir.absolutePath();
    }
    else
    {
        // If no overrides are specified, use the default path.
#if defined(__WIN32__) && defined(WIN32_PORTABLE)
        emuDirectory = appdirpath;
#else
        QString confdir;
        QDir config(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
        config.mkdir("melonDS");
        confdir = config.absolutePath() + QDir::separator() + "melonDS";
        emuDirectory = confdir;
#endif
    }
}


void setMPInterface(MPInterfaceType type)
{
    // switch to the requested MP interface
    MPInterface::Set(type);

    // set receive timeout
    // TODO: different settings per interface?
    MPInterface::Get().SetRecvTimeout(Config::GetGlobalTable().GetInt("MP.RecvTimeout"));

    // update UI appropriately
    // TODO: decide how to deal with multi-window when it becomes a thing
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        EmuInstance* inst = emuInstances[i];
        if (!inst) continue;

        MainWindow* win = inst->getMainWindow();
        if (win) win->updateMPInterface(type);
    }
}



MelonApplication::MelonApplication(int& argc, char** argv)
    : QApplication(argc, argv)
{
#if !defined(Q_OS_APPLE)
    setWindowIcon(QIcon(":/melon-icon"));
    #if defined(Q_OS_UNIX)
        setDesktopFileName(QString("net.kuribo64.melonDS"));
    #endif
#endif
}

#ifdef MELONDS_TRACE_QT
// every Qt event as a zone, "<event type>: <receiver class>": paints, layouts, timers and the gaps between
bool MelonApplication::notify(QObject* receiver, QEvent* event)
{
    static const QMetaEnum types = QMetaEnum::fromType<QEvent::Type>();
    const char* type = types.valueToKey(event->type());
    char name[128];
    if (type) snprintf(name, sizeof(name), "%s: %s", type, receiver->metaObject()->className());
    else snprintf(name, sizeof(name), "QEvent %d: %s", (int)event->type(), receiver->metaObject()->className());
    TRACE_QT_NAMED(name);
    return QApplication::notify(receiver, event);
}
#endif

// TODO: ROM loading should be moved to EmuInstance
// especially so the preloading below and in main() can be done in a nicer fashion

bool MelonApplication::event(QEvent *event)
{
    if (event->type() == QEvent::FileOpen)
    {
        EmuInstance* inst = emuInstances[0];
        MainWindow* win = inst->getMainWindow();
        QFileOpenEvent *openEvent = static_cast<QFileOpenEvent*>(event);

        const QStringList file = win->splitArchivePath(openEvent->file(), true);
        win->preloadROMs(file, {}, true);
    }

    return QApplication::event(event);
}

#ifndef _WIN32
// The handler only writes a byte; the event loop reads it and quits. Calling qApp->quit() in the handler
// closed every window right where the signal landed, even in the middle of painting a game screen
static int signalPipe[2] = {-1, -1};

static void signalHandler(int signal)
{
    std::signal(signal, SIG_DFL);
    char b = 1;
    if (write(signalPipe[1], &b, 1) < 0) {}
}
#endif

int main(int argc, char** argv)
{
    TRACE_QT_STEP("startup: before QApplication");
    sysTimer.start();
    srand(time(nullptr));

    for (int i = 0; i < kMaxEmuInstances; i++)
        emuInstances[i] = nullptr;

    qputenv("QT_SCALE_FACTOR", "1");

#if defined(__linux__)
    // Wayland doesn't let apps position their own windows, so saved window
    // positions are ignored. Default to X11 (XWayland) unless the user chose.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif

#if defined(_WIN32)
#if QT_VERSION_MAJOR == 6
    // Allow using the system dark theme palette on Windows
    qputenv("QT_QPA_PLATFORM", "windows:darkmode=2");
#endif

    // argc and argv are passed as UTF8 by SDL's WinMain function
    // QT checks for the original value in local encoding though
    // to see whether it is unmodified to activate its hack that
    // retrieves the unicode value via CommandLineToArgvW.
    argc = __argc;
    argv = __argv;

    // Check whether we are already attached to an output stream.
    HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!outputHandle || (outputHandle == INVALID_HANDLE_VALUE))
    {
        // If started from terminal, attach and output logs to it.
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            freopen("CONOUT$", "a", stdout);
            freopen("CONOUT$", "a", stderr);
        }
        else
        {
            // Otherwise, discard log output.
            freopen("NUL:", "w", stdout);
            freopen("NUL:", "w", stderr);
        }
    }
#else
    if (pipe(signalPipe) == 0)
    {
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
    }
#endif

    printf("melonDS " MELONDS_VERSION "\n");
    printf(MELONDS_URL "\n");

    // easter egg - not worth checking other cases for something so dumb
    if (argc != 0 && (!strcasecmp(argv[0], "derpDS") || !strcasecmp(argv[0], "./derpDS")))
        printf("did you just call me a derp???\n");

    TRACE_QT_STEP("startup: QApplication");
    MelonApplication melon(argc, argv);
#ifndef _WIN32
    QSocketNotifier* quitOnSignal = signalPipe[0] >= 0 ? new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &melon) : nullptr;
    if (quitOnSignal) QObject::connect(quitOnSignal, &QSocketNotifier::activated, &melon, []() { qApp->quit(); });
#endif
    TRACE_QT_STEP("startup: paths and arguments");
    pathInit();

    CLI::CommandLineOptions* options = CLI::ManageArgs(melon);

    // http://stackoverflow.com/questions/14543333/joystick-wont-work-using-sdl
    TRACE_QT_STEP("startup: SDL");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_SetHint(SDL_HINT_APP_NAME, "melonDS");

    TRACE_SDL_STARTUP_MESSAGE("SDL init begins");
    // No SDL_INIT_HAPTIC: nothing uses SDL_Haptic, and rumble goes through SDL_GameControllerRumble.
    if ([] { TRACE_SDL_STARTUP("SDL_Init(AUDIO)"); return SDL_Init(SDL_INIT_AUDIO); }() < 0)
    {
        const char* err = SDL_GetError();
        QString errorStr = "Failed to initialize SDL. This could indicate an issue with your audio driver.\n\nThe error was: ";
        errorStr += err;

        QMessageBox::critical(nullptr, "melonDS", errorStr);
        return 1;
    }

    { TRACE_SDL_STARTUP("SDL_InitSubSystem(VIDEO)"); SDL_InitSubSystem(SDL_INIT_VIDEO); }
    { TRACE_SDL_STARTUP("SDL screensaver calls"); SDL_EnableScreenSaver(); SDL_DisableScreenSaver(); }

    // last SDL_Init on this thread: SDL_Init calls must not overlap the worker's
    EmuInstance::joystickInitStart();

    TRACE_QT_STEP("startup: config load");
    if (!Config::Load())
        QMessageBox::critical(nullptr,
                              "melonDS",
                              "Unable to write to config.\nPlease check the write permissions of the folder you placed melonDS in.");

    camStarted[0] = false;
    camStarted[1] = false;
    TRACE_QT_STEP("startup: cameras");
    camManager[0] = new CameraManager(0, 640, 480, true);
    camManager[1] = new CameraManager(1, 640, 480, true);

    systemThemeName = new QString(QApplication::style()->objectName());

    {
        Config::Table cfg = Config::GetGlobalTable();
        QString uitheme = cfg.GetQString("UITheme");
        if (!uitheme.isEmpty())
        {
            QApplication::setStyle(uitheme);
        }
    }

    // fix for Wayland OpenGL glitches
    QGuiApplication::setAttribute(Qt::AA_NativeWindows, false);
    QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);

    // default MP interface type is local MP
    // this will be changed if a LAN or netplay session is initiated
    setMPInterface(MPInterface_Local);

    TRACE_QT_STEP("startup: network");
    NetInit();

    TRACE_QT_STEP("startup: emu instance and main window");
    MainWindowStartHidden = options->workbenchOnly;
    createEmuInstance();

    TRACE_QT_STEP("startup: workbench");
    {
        // decomp workbench: settings and command line
        WBHost* wb = emuInstances[0]->getWorkbench();
        if (options->knowledgeStore) wb->Settings().KnowledgeStoreOverride = *options->knowledgeStore;
        wb->Settings().WorkbenchOnly = options->workbenchOnly;
        bool api = options->api || wb->Settings().ApiEnabled;
        if (api)
            wb->StartApi(options->apiPort ? options->apiPort : wb->Settings().ApiPort,
                         options->apiMemoryWrite || wb->Settings().ApiMemoryWrite);
        // Recording, write history and data origin are on from boot. --record keeps its old meaning
        // (recording only). --no-record turns all three off; the other --no-* flags turn one off.
        if (!options->noRecord)
        {
            if (options->record)
                wb->RecordOnBoot(options->writeHistory && !options->noWriteHistory, options->dataOrigin && !options->noDataOrigin);
            else
                wb->RecordOnBoot(!options->noWriteHistory, !options->noDataOrigin);
        }
    }

    TRACE_QT_STEP("startup: ROM preload start");
    {
        MainWindow* win = emuInstances[0]->getMainWindow();
        bool memberSyntaxUsed = false;
        const auto prepareRomPath = [&](const std::optional<QString> &romPath,
                                        const std::optional<QString> &romArchivePath) -> QStringList
        {
            if (!romPath.has_value())
                return {};

            if (romArchivePath.has_value())
                return {*romPath, *romArchivePath};

            const QStringList path = win->splitArchivePath(*romPath, true);
            if (path.size() > 1) memberSyntaxUsed = true;
            return path;
        };

        const QStringList dsfile = prepareRomPath(options->dsRomPath, options->dsRomArchivePath);
        const QStringList gbafile = prepareRomPath(options->gbaRomPath, options->gbaRomArchivePath);

        if (memberSyntaxUsed) printf("Warning: use the a.zip|b.nds format at your own risk!\n");

        // the ROM boots on the emulation thread while the workbench window is built here, then we wait
        // for it: the workbench settings it reads (record at boot) are set above
        const bool preloading = win->preloadROMsStart(dsfile, gbafile, options->boot);

        TRACE_QT_STEP("startup: workbench window");
        if (options->workbench) emuInstances[0]->getWorkbench()->ShowWindow();

        TRACE_QT_STEP("startup: ROM preload finish");
        if (preloading) win->preloadROMsFinish();

        if (options->fullscreen)
            win->toggleFullscreen();
    }

    TRACE_QT_STEP_END();
    TRACE_QT_MESSAGE("event loop starts");
    int ret = melon.exec();
    TRACE_QT_STEP("shutdown");

    delete options;

    // if we get here, all the existing emu instances should have been deleted already
    // but with this we make extra sure they are all deleted
    deleteAllEmuInstances();

    delete camManager[0];
    delete camManager[1];

    Config::Save();

    EmuInstance::joystickInitWait();
    { TRACE_SDL_STARTUP("SDL_Quit"); SDL_Quit(); }
    TRACE_QT_STEP_END();
    return ret;
}
