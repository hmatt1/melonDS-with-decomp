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

#include <tuple>
#include <QGroupBox>
#include <QStandardItemModel>
#include "Trace.h"
#include "NDS.h"
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <vector>
#include <string>
#include <algorithm>

#include <QPointer>
#include <QProcess>
#include <QStatusBar>
#include <QApplication>
#include <QMessageBox>
#include <QMenuBar>
#include <QToolBar>
#include <QRadioButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QComboBox>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QGridLayout>
#include <QSlider>
#include <QPushButton>
#include <QActionGroup>
#include <QMimeDatabase>
#include <QFileDialog>
#include <QInputDialog>
#include <QPaintEvent>
#include <QPainter>
#include <QKeyEvent>
#include <QMimeData>
#include <QVector>
#include <QCommandLineParser>
#include <QDesktopServices>

#include "main.h"
#include "CheatsDialog.h"
#include "DateTimeDialog.h"
#include "EmuSettingsDialog.h"
#include "InputConfig/InputConfigDialog.h"
#include "VideoSettingsDialog.h"
#include "CameraSettingsDialog.h"
#include "AudioSettingsDialog.h"
#include "FirmwareSettingsDialog.h"
#include "PathSettingsDialog.h"
#include "MPSettingsDialog.h"
#include "WifiSettingsDialog.h"
#include "InterfaceSettingsDialog.h"
#include "ROMInfoDialog.h"
#include "RAMInfoDialog.h"
#include "TitleManagerDialog.h"
#include "PowerManagement/PowerManagementDialog.h"

#include "Platform.h"
#include "Config.h"
#include "version.h"
#include "Savestate.h"
#include "MPInterface.h"
#include "LANDialog.h"

//#include "main_shaders.h"

#include "EmuInstance.h"
#include "ArchiveUtil.h"
#include "CameraManager.h"
#include "Window.h"
#include "workbench/WBHost.h"
#include "workbench/WBStatusWidget.h"
#include "AboutDialog.h"

using namespace melonDS;




extern CameraManager* camManager[2];
extern bool camStarted[2];


bool MainWindowStartHidden = false;

QString NdsRomMimeType = "application/x-nintendo-ds-rom";
QStringList NdsRomExtensions { ".nds", ".srl", ".dsi", ".ids" };

QString GbaRomMimeType = "application/x-gba-rom";
QStringList GbaRomExtensions { ".gba", ".agb" };


// This list of supported archive formats is based on libarchive(3) version 3.6.2 (2022-12-09).
QStringList ArchiveMimeTypes
{
#ifdef ARCHIVE_SUPPORT_ENABLED
    "application/zip",
    "application/x-7z-compressed",
    "application/vnd.rar", // *.rar
    "application/x-tar",

    "application/x-compressed-tar", // *.tar.gz
    "application/x-xz-compressed-tar",
    "application/x-bzip-compressed-tar",
    "application/x-lz4-compressed-tar",
    "application/x-zstd-compressed-tar",

    "application/x-tarz", // *.tar.Z
    "application/x-lzip-compressed-tar",
    "application/x-lzma-compressed-tar",
    "application/x-lrzip-compressed-tar",
    "application/x-tzo", // *.tar.lzo
#endif
};

QStringList ArchiveExtensions
{
#ifdef ARCHIVE_SUPPORT_ENABLED
    ".zip", ".7z", ".rar", ".tar",

    ".tar.gz", ".tgz",
    ".tar.xz", ".txz",
    ".tar.bz2", ".tbz2",
    ".tar.lz4", ".tlz4",
    ".tar.zst", ".tzst",

    ".tar.Z", ".taz",
    ".tar.lz",
    ".tar.lzma", ".tlz",
    ".tar.lrz", ".tlrz",
    ".tar.lzo", ".tzo"
#endif
};

// AAAAAAA
static bool FileExtensionInList(const QString& filename, const QStringList& extensions, Qt::CaseSensitivity cs = Qt::CaseInsensitive)
{
    return std::any_of(extensions.cbegin(), extensions.cend(), [&](const auto& ext) {
        return filename.endsWith(ext, cs);
    });
}

static bool MimeTypeInList(const QMimeType& mimetype, const QStringList& superTypeNames)
{
    return std::any_of(superTypeNames.cbegin(), superTypeNames.cend(), [&](const auto& superTypeName) {
        return mimetype.inherits(superTypeName);
    });
}


static bool NdsRomByExtension(const QString& filename)
{
    return FileExtensionInList(filename, NdsRomExtensions);
}

static bool GbaRomByExtension(const QString& filename)
{
    return FileExtensionInList(filename, GbaRomExtensions);
}

static bool SupportedArchiveByExtension(const QString& filename)
{
    return FileExtensionInList(filename, ArchiveExtensions);
}


static bool NdsRomByMimetype(const QMimeType& mimetype)
{
    return mimetype.inherits(NdsRomMimeType);
}

static bool GbaRomByMimetype(const QMimeType& mimetype)
{
    return mimetype.inherits(GbaRomMimeType);
}

static bool SupportedArchiveByMimetype(const QMimeType& mimetype)
{
    return MimeTypeInList(mimetype, ArchiveMimeTypes);
}

static bool ZstdNdsRomByExtension(const QString& filename)
{
    return filename.endsWith(".zst", Qt::CaseInsensitive) &&
        NdsRomByExtension(filename.left(filename.size() - 4));
}

static bool ZstdGbaRomByExtension(const QString& filename)
{
    return filename.endsWith(".zst", Qt::CaseInsensitive) &&
        GbaRomByExtension(filename.left(filename.size() - 4));
}

static bool FileIsSupportedFiletype(const QString& filename, bool insideArchive = false)
{
    if (ZstdNdsRomByExtension(filename) || ZstdGbaRomByExtension(filename))
        return true;

    if (NdsRomByExtension(filename) || GbaRomByExtension(filename) || SupportedArchiveByExtension(filename))
        return true;

    const auto matchmode = insideArchive ? QMimeDatabase::MatchExtension : QMimeDatabase::MatchDefault;
    const QMimeType mimetype = QMimeDatabase().mimeTypeForFile(filename, matchmode);
    return NdsRomByMimetype(mimetype) || GbaRomByMimetype(mimetype) || SupportedArchiveByMimetype(mimetype);
}



MainWindow::MainWindow(int id, EmuInstance* inst, QWidget* parent, bool embedded) :
    QMainWindow(parent, embedded ? Qt::WindowFlags(Qt::Widget) : Qt::WindowFlags()),
    windowID(id),
    emuInstance(inst),
    globalCfg(inst->globalCfg),
    localCfg(inst->localCfg),
    windowCfg(localCfg.GetTable("Window"+std::to_string(id), "Window0")),
    emuThread(inst->getEmuThread()),
    enabledSaved(false),
    focused(true)
{
    TRACE_QT("MainWindow constructor");
    this->embedded = embedded;
    // QMainWindow always makes itself a window: an embedded one becomes a plain child widget again
    if (embedded) setWindowFlags(Qt::Widget);

    showOSD = windowCfg.GetBool("ShowOSD");

    setWindowTitle("melonDS " MELONDS_VERSION);
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);

#if QT_VERSION_MAJOR == 6 && WIN32
    // The "windows11" theme has pretty massive padding around menubar items, this makes Config and Help not fit in a window at 1x screen sizing
    // So let's reduce the padding a bit.
    if (QApplication::style()->name() == "windows11")
        setStyleSheet("QMenuBar::item { padding: 4px 8px; }");
#endif

    //hasMenu = (!parent);
    hasMenu = true;

    if (hasMenu)
    {
        QMenuBar * menubar = new QMenuBar();
        {
            QMenu * menu = menubar->addMenu("File");

            actOpenROM = menu->addAction("Open ROM...");
            connect(actOpenROM, &QAction::triggered, this, &MainWindow::onOpenFile);
            actOpenROM->setShortcut(QKeySequence(QKeySequence::StandardKey::Open));

            /*actOpenROMArchive = menu->addAction("Open ROM inside archive...");
            connect(actOpenROMArchive, &QAction::triggered, this, &MainWindow::onOpenFileArchive);
            actOpenROMArchive->setShortcut(QKeySequence(Qt::Key_O | Qt::CTRL | Qt::SHIFT));*/

            recentMenu = menu->addMenu("Open recent");
            loadRecentFilesMenu(true);

            //actBootFirmware = menu->addAction("Launch DS menu");
            actBootFirmware = menu->addAction("Boot firmware");
            connect(actBootFirmware, &QAction::triggered, this, &MainWindow::onBootFirmware);

            menu->addSeparator();

            actCurrentCart = menu->addAction("DS slot: " + emuInstance->cartLabel());
            actCurrentCart->setEnabled(false);

            actInsertCart = menu->addAction("Insert cart...");
            connect(actInsertCart, &QAction::triggered, this, &MainWindow::onInsertCart);

            actEjectCart = menu->addAction("Eject cart");
            connect(actEjectCart, &QAction::triggered, this, &MainWindow::onEjectCart);

            menu->addSeparator();

            actCurrentGBACart = menu->addAction("GBA slot: " + emuInstance->gbaCartLabel());
            actCurrentGBACart->setEnabled(false);

            actInsertGBACart = menu->addAction("Insert ROM cart...");
            connect(actInsertGBACart, &QAction::triggered, this, &MainWindow::onInsertGBACart);

            {
                QMenu * submenu = menu->addMenu("Insert add-on cart");
                QAction *act;

                int addons[] = {
                    GBAAddon_RAMExpansion,
                    GBAAddon_RumblePak,
                    GBAAddon_SolarSensorBoktai1,
                    GBAAddon_SolarSensorBoktai2,
                    GBAAddon_SolarSensorBoktai3,
                    GBAAddon_MotionPakHomebrew,
                    GBAAddon_MotionPakRetail,
                    GBAAddon_GuitarGrip,
                    -1
                };

                for (int i = 0; addons[i] != -1; i++)
                {
                    int addon = addons[i];
                    act = submenu->addAction(emuInstance->gbaAddonName(addon));
                    act->setData(QVariant(addon));
                    connect(act, &QAction::triggered, this, &MainWindow::onInsertGBAAddon);
                    actInsertGBAAddon.append(act);
                }
            }

            actEjectGBACart = menu->addAction("Eject cart");
            connect(actEjectGBACart, &QAction::triggered, this, &MainWindow::onEjectGBACart);

            menu->addSeparator();

            actImportSavefile = menu->addAction("Import savefile");
            connect(actImportSavefile, &QAction::triggered, this, &MainWindow::onImportSavefile);

            menu->addSeparator();

            {
                QMenu * submenu = menu->addMenu("Save state");

                for (int i = 1; i < 9; i++)
                {
                    actSaveState[i] = submenu->addAction(QString("%1").arg(i));
                    actSaveState[i]->setShortcut(QKeySequence(Qt::ShiftModifier | (Qt::Key_F1 + i - 1)));
                    actSaveState[i]->setData(QVariant(i));
                    connect(actSaveState[i], &QAction::triggered, this, &MainWindow::onSaveState);
                }

                actSaveState[0] = submenu->addAction("File...");
                actSaveState[0]->setShortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_F9));
                actSaveState[0]->setData(QVariant(0));
                connect(actSaveState[0], &QAction::triggered, this, &MainWindow::onSaveState);
            }
            {
                QMenu * submenu = menu->addMenu("Load state");

                for (int i = 1; i < 9; i++)
                {
                    actLoadState[i] = submenu->addAction(QString("%1").arg(i));
                    actLoadState[i]->setShortcut(QKeySequence(Qt::Key_F1 + i - 1));
                    actLoadState[i]->setData(QVariant(i));
                    connect(actLoadState[i], &QAction::triggered, this, &MainWindow::onLoadState);
                }

                actLoadState[0] = submenu->addAction("File...");
                actLoadState[0]->setShortcut(QKeySequence(Qt::Key_F9));
                actLoadState[0]->setData(QVariant(0));
                connect(actLoadState[0], &QAction::triggered, this, &MainWindow::onLoadState);
            }

            actUndoStateLoad = menu->addAction("Undo state load");
            actUndoStateLoad->setShortcut(QKeySequence(Qt::Key_F12));
            connect(actUndoStateLoad, &QAction::triggered, this, &MainWindow::onUndoStateLoad);

            menu->addSeparator();
            actOpenConfig = menu->addAction("Open melonDS directory");
            connect(actOpenConfig, &QAction::triggered, this, [&]()
            {
                QDesktopServices::openUrl(QUrl::fromLocalFile(emuDirectory));
            });

            menu->addSeparator();

            actQuit = menu->addAction("Quit");
            connect(actQuit, &QAction::triggered, this, &MainWindow::onQuit);
            actQuit->setShortcut(QKeySequence(QKeySequence::StandardKey::Quit));
        }
        {
            QMenu * menu = menubar->addMenu("System");

            actPause = menu->addAction("Pause");
            actPause->setCheckable(true);
            connect(actPause, &QAction::triggered, this, &MainWindow::onPause);

            actReset = menu->addAction("Reset");
            connect(actReset, &QAction::triggered, this, &MainWindow::onReset);

            actStop = menu->addAction("Stop");
            connect(actStop, &QAction::triggered, this, &MainWindow::onStop);

            actFrameStep = menu->addAction("Frame step");
            connect(actFrameStep, &QAction::triggered, this, &MainWindow::onFrameStep);

            menu->addSeparator();

            actPowerManagement = menu->addAction("Power management");
            connect(actPowerManagement, &QAction::triggered, this, &MainWindow::onOpenPowerManagement);

            actDateTime = menu->addAction("Date and time");
            connect(actDateTime, &QAction::triggered, this, &MainWindow::onOpenDateTime);

            menu->addSeparator();

            actEnableCheats = menu->addAction("Enable cheats");
            actEnableCheats->setCheckable(true);
            connect(actEnableCheats, &QAction::triggered, this, &MainWindow::onEnableCheats);

            //if (inst == 0)
            {
                actSetupCheats = menu->addAction("Setup cheat codes");
                actSetupCheats->setMenuRole(QAction::NoRole);
                connect(actSetupCheats, &QAction::triggered, this, &MainWindow::onSetupCheats);

                menu->addSeparator();
                actROMInfo = menu->addAction("ROM info");
                connect(actROMInfo, &QAction::triggered, this, &MainWindow::onROMInfo);

                actRAMInfo = menu->addAction("RAM search");
                connect(actRAMInfo, &QAction::triggered, this, &MainWindow::onRAMInfo);

                menu->addSeparator();
                actWorkbench = menu->addAction("Decomp workbench...");
                actWorkbench->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
                connect(actWorkbench, &QAction::triggered, this, [this]() { emuInstance->getWorkbench()->ShowWindow(); });
                // the same name and key as the workbench's Recording > Start recording
                actRecording = menu->addAction("Start recording");
                actRecording->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
                connect(actRecording, &QAction::triggered, this, [this]()
                {
                    WBHost* wb = emuInstance->getWorkbench();
                    wb->SetRecording(!wb->Status().Recording);
                });

                actTitleManager = menu->addAction("Manage DSi titles");
                connect(actTitleManager, &QAction::triggered, this, &MainWindow::onOpenTitleManager);
            }

            {
                menu->addSeparator();
                QMenu * submenu = menu->addMenu("Multiplayer");

                actMPNewInstance = submenu->addAction("Launch new instance");
                connect(actMPNewInstance, &QAction::triggered, this, &MainWindow::onMPNewInstance);

                submenu->addSeparator();

                actLANStartHost = submenu->addAction("Host LAN game");
                connect(actLANStartHost, &QAction::triggered, this, &MainWindow::onLANStartHost);

                actLANStartClient = submenu->addAction("Join LAN game");
                connect(actLANStartClient, &QAction::triggered, this, &MainWindow::onLANStartClient);

                /*submenu->addSeparator();

                actNPStartHost = submenu->addAction("NETPLAY HOST");
                connect(actNPStartHost, &QAction::triggered, this, &MainWindow::onNPStartHost);

                actNPStartClient = submenu->addAction("NETPLAY CLIENT");
                connect(actNPStartClient, &QAction::triggered, this, &MainWindow::onNPStartClient);

                actNPTest = submenu->addAction("NETPLAY GO");
                connect(actNPTest, &QAction::triggered, this, &MainWindow::onNPTest);*/
            }
        }
        {
            QMenu * menu = menubar->addMenu("View");

            {
                QMenu * submenu = menu->addMenu("Screen size");

                for (int i = 0; i < 4; i++)
                {
                    int data = i + 1;
                    actScreenSize[i] = submenu->addAction(QString("%1x").arg(data));
                    actScreenSize[i]->setData(QVariant(data));
                    actScreenSize[i]->setShortcut(QKeySequence(Qt::AltModifier | (Qt::Key_1 + i)));
                    connect(actScreenSize[i], &QAction::triggered, this, &MainWindow::onChangeScreenSize);
                }
            }
            {
                QMenu * submenu = menu->addMenu("Screen rotation");
                grpScreenRotation = new QActionGroup(submenu);

                for (int i = 0; i < screenRot_MAX; i++)
                {
                    int data = i * 90;
                    actScreenRotation[i] = submenu->addAction(QString("%1°").arg(data));
                    actScreenRotation[i]->setActionGroup(grpScreenRotation);
                    actScreenRotation[i]->setData(QVariant(i));
                    actScreenRotation[i]->setCheckable(true);
                }

                connect(grpScreenRotation, &QActionGroup::triggered, this, &MainWindow::onChangeScreenRotation);
            }
            {
                QMenu * submenu = menu->addMenu("Screen gap");
                grpScreenGap = new QActionGroup(submenu);

                const int screengap[] = {0, 1, 8, 32, 64, 90, 128};

                for (int i = 0; i < 7; i++)
                {
                    int data = screengap[i];
                    actScreenGap[i] = submenu->addAction(QString("%1 px").arg(data));
                    actScreenGap[i]->setActionGroup(grpScreenGap);
                    actScreenGap[i]->setData(QVariant(data));
                    actScreenGap[i]->setCheckable(true);
                }

                connect(grpScreenGap, &QActionGroup::triggered, this, &MainWindow::onChangeScreenGap);
            }
            {
                QMenu * submenu = menu->addMenu("Screen layout");
                grpScreenLayout = new QActionGroup(submenu);

                const char *screenlayout[] = {"Natural", "Vertical", "Horizontal", "Hybrid"};

                for (int i = 0; i < screenLayout_MAX; i++)
                {
                    actScreenLayout[i] = submenu->addAction(QString(screenlayout[i]));
                    actScreenLayout[i]->setActionGroup(grpScreenLayout);
                    actScreenLayout[i]->setData(QVariant(i));
                    actScreenLayout[i]->setCheckable(true);
                }

                connect(grpScreenLayout, &QActionGroup::triggered, this, &MainWindow::onChangeScreenLayout);

                submenu->addSeparator();

                actScreenSwap = submenu->addAction("Swap screens");
                actScreenSwap->setCheckable(true);
                connect(actScreenSwap, &QAction::triggered, this, &MainWindow::onChangeScreenSwap);
            }
            {
                QMenu * submenu = menu->addMenu("Screen sizing");
                grpScreenSizing = new QActionGroup(submenu);

                const char *screensizing[] = {"Even", "Emphasize top", "Emphasize bottom", "Auto", "Top only",
                                              "Bottom only"};

                for (int i = 0; i < screenSizing_MAX; i++)
                {
                    actScreenSizing[i] = submenu->addAction(QString(screensizing[i]));
                    actScreenSizing[i]->setActionGroup(grpScreenSizing);
                    actScreenSizing[i]->setData(QVariant(i));
                    actScreenSizing[i]->setCheckable(true);
                }

                connect(grpScreenSizing, &QActionGroup::triggered, this, &MainWindow::onChangeScreenSizing);

                submenu->addSeparator();

                actIntegerScaling = submenu->addAction("Force integer scaling");
                actIntegerScaling->setCheckable(true);
                connect(actIntegerScaling, &QAction::triggered, this, &MainWindow::onChangeIntegerScaling);
            }
            {
                QMenu * submenu = menu->addMenu("Aspect ratio");
                grpScreenAspectTop = new QActionGroup(submenu);
                grpScreenAspectBot = new QActionGroup(submenu);
                actScreenAspectTop = new QAction *[AspectRatiosNum];
                actScreenAspectBot = new QAction *[AspectRatiosNum];

                for (int i = 0; i < 2; i++)
                {
                    QActionGroup * group = grpScreenAspectTop;
                    QAction **actions = actScreenAspectTop;

                    if (i == 1)
                    {
                        group = grpScreenAspectBot;
                        submenu->addSeparator();
                        actions = actScreenAspectBot;
                    }

                    for (int j = 0; j < AspectRatiosNum; j++)
                    {
                        auto ratio = aspectRatios[j];
                        QString label = QString("%1 %2").arg(i ? "Bottom" : "Top", ratio.label);
                        actions[j] = submenu->addAction(label);
                        actions[j]->setActionGroup(group);
                        actions[j]->setData(QVariant(ratio.id));
                        actions[j]->setCheckable(true);
                    }

                    connect(group, &QActionGroup::triggered, this, &MainWindow::onChangeScreenAspect);
                }
            }

            menu->addSeparator();

            actNewWindow = menu->addAction("Open new window");
            connect(actNewWindow, &QAction::triggered, this, &MainWindow::onOpenNewWindow);

            menu->addSeparator();

            // One filter at a time, for the whole instance. What draws it follows Config > Video
            // settings: the OpenGL shaders with an OpenGL display or OpenGL 3D renderer, else the
            // software filters (the fallback).
            {
                menuScreenFilter = menu->addMenu("Screen filter");
                menuScreenFilter->setToolTipsVisible(true);
                grpScreenFilter = new QActionGroup(this);
                const char* names[screenFilter_MAX] = {"Off", "LCD", "CRT"};
                for (int i = 0; i < screenFilter_MAX; i++)
                {
                    actScreenFilter[i] = menuScreenFilter->addAction(names[i]);
                    actScreenFilter[i]->setCheckable(true);
                    actScreenFilter[i]->setData(i);
                    grpScreenFilter->addAction(actScreenFilter[i]);
                }
                connect(grpScreenFilter, &QActionGroup::triggered, this, [this](QAction* act)
                {
                    onChangeScreenFilter(act->data().toInt());
                });

                actToggleScreenFilter = menuScreenFilter->addAction("Turn filter on/off");
                actToggleScreenFilter->setShortcut(QKeySequence(Qt::AltModifier | Qt::Key_F));
                actToggleScreenFilter->setToolTip("Switches between Off and the filter used last");
                connect(actToggleScreenFilter, &QAction::triggered, this, &MainWindow::onToggleScreenFilter);

                menuScreenFilter->addSeparator();
                actLcdSettings = menuScreenFilter->addAction("LCD filter settings...");
                actCrtSettings = menuScreenFilter->addAction("CRT filter settings...");
            }

            actShowOSD = menu->addAction("Show OSD");
            actShowOSD->setCheckable(true);
            connect(actShowOSD, &QAction::triggered, this, &MainWindow::onChangeShowOSD);
        }
        {
            QMenu * menu = menubar->addMenu("Config");

            actEmuSettings = menu->addAction("Emu settings");
            connect(actEmuSettings, &QAction::triggered, this, &MainWindow::onOpenEmuSettings);

#ifdef __APPLE__
            actPreferences = menu->addAction("Preferences...");
            connect(actPreferences, &QAction::triggered, this, &MainWindow::onOpenEmuSettings);
            actPreferences->setMenuRole(QAction::PreferencesRole);
#endif

            actInputConfig = menu->addAction("Input and hotkeys");
            connect(actInputConfig, &QAction::triggered, this, &MainWindow::onOpenInputConfig);

            actVideoSettings = menu->addAction("Video settings");
            connect(actVideoSettings, &QAction::triggered, this, &MainWindow::onOpenVideoSettings);

            actCameraSettings = menu->addAction("Camera settings");
            connect(actCameraSettings, &QAction::triggered, this, &MainWindow::onOpenCameraSettings);

            actAudioSettings = menu->addAction("Audio settings");
            connect(actAudioSettings, &QAction::triggered, this, &MainWindow::onOpenAudioSettings);

            actMPSettings = menu->addAction("Multiplayer settings");
            connect(actMPSettings, &QAction::triggered, this, &MainWindow::onOpenMPSettings);

            actWifiSettings = menu->addAction("Wifi settings");
            connect(actWifiSettings, &QAction::triggered, this, &MainWindow::onOpenWifiSettings);

            actFirmwareSettings = menu->addAction("Firmware settings");
            connect(actFirmwareSettings, &QAction::triggered, this, &MainWindow::onOpenFirmwareSettings);

            actInterfaceSettings = menu->addAction("Interface settings");
            connect(actInterfaceSettings, &QAction::triggered, this, &MainWindow::onOpenInterfaceSettings);

            actPathSettings = menu->addAction("Path settings");
            connect(actPathSettings, &QAction::triggered, this, &MainWindow::onOpenPathSettings);

            menu->addSeparator();

            actLimitFramerate = menu->addAction("Limit framerate");
            actLimitFramerate->setCheckable(true);
            connect(actLimitFramerate, &QAction::triggered, this, &MainWindow::onChangeLimitFramerate);

            actAudioSync = menu->addAction("Audio sync");
            actAudioSync->setCheckable(true);
            connect(actAudioSync, &QAction::triggered, this, &MainWindow::onChangeAudioSync);
        }
        {
            QMenu * menu = menubar->addMenu("Help");
            actAbout = menu->addAction("About...");
            connect(actAbout, &QAction::triggered, this, [&]
            {
                auto dialog = AboutDialog(this);
                dialog.exec();
            });
        }

        setMenuBar(menubar);

        // screen filter settings windows; each slider writes this window's config and screen
        {
            auto localPath = [](const char* name) { return QString::fromStdString(Platform::GetLocalFilePath(name)); };

            FilterWindowSpec lcd;
            lcd.title = "LCD filter settings";
            lcd.filterName = "LCD filter";
            lcd.params = kGridParams;
            lcd.count = kNumGridParams;
            lcd.setParam = [this](int i, float v) { if (panel) panel->gridParam[i] = v; };
            lcd.setActive = [this](bool on) { onChangeScreenFilter(on ? screenFilter_LCD : screenFilter_Off); };
            lcd.presetFile = localPath("lcd-presets.json");
            lcd.legacyPresetFile = localPath("pixelgrid-presets.json");   // before the CRT filter
            lcd.note = "Imitates the DS's own screens: grid lines between pixels, the LED stripes "
                       "of each pixel and a light blur.";
            lcd.addExtras = [this](QWidget* win, QGridLayout* grid, int paramRow, int& row)
            {
                // the blur rows also get their reach, in DS pixels
                QSpinBox* reach[2] = {};
                for (int i = 0; i < kNumGridParams; i++)
                {
                    const char* key = kGridParams[i].key;
                    int axis = !strcmp(key, "BlurStrengthX") ? 0 : !strcmp(key, "BlurStrengthY") ? 1 : -1;
                    if (axis < 0) continue;
                    const char* rkey = axis ? "BlurReachY" : "BlurReachX";
                    auto* box = new QSpinBox(win);
                    box->setRange(0, 3);
                    box->setSuffix(" px");
                    box->setToolTip("How many DS pixels away, in this direction, the blur takes colour from (0 to 3).\n"
                                    "At 1 only the nearest neighbour on each side counts; at 3, three on each side.\n"
                                    "At 0 this direction isn't blurred, whatever the slider says.\n\n"
                                    "Default: 1 px");
                    box->setValue(windowCfg.GetInt(rkey));
                    connect(box, &QSpinBox::valueChanged, this, [this, axis, rkey](int v)
                    {
                        windowCfg.SetInt(rkey, v);
                        if (panel) panel->blurReach[axis] = v;
                    });
                    grid->addWidget(box, paramRow + i, 3);
                    reach[axis] = box;
                }

                // LED stripe layout of the console being imitated
                auto* layoutRow = new QHBoxLayout();
                auto* layoutLabel = new QLabel("Subpixels", win);
                layoutLabel->setToolTip("Which console's LED stripes to imitate. Each DS pixel is lit by three thin\n"
                                        "stripes, one per colour, and their order differs between models:\n\n"
                                        "DS Lite and DSi: blue, green, red, left to right, on both screens.\n"
                                        "DSi XL: blue, green, red on the top screen, but red, green, blue on the bottom.\n\n"
                                        "None turns the stripes off. Subpixel strength sets how visible they are.\n\n"
                                        "Default: DSi XL");
                layoutRow->addWidget(layoutLabel);
                auto* layoutGroup = new QButtonGroup(win);
                const char* layouts[] = {"None", "DS Lite", "DSi", "DSi XL"};
                for (int i = 0; i < 4; i++)
                {
                    auto* rb = new QRadioButton(layouts[i], win);
                    rb->setChecked(windowCfg.GetInt("SubpixelLayout") == i);
                    layoutGroup->addButton(rb, i);
                    layoutRow->addWidget(rb);
                }
                layoutRow->addStretch();
                connect(layoutGroup, &QButtonGroup::idClicked, this, [this](int id)
                {
                    windowCfg.SetInt("SubpixelLayout", id);
                    if (panel) panel->subpixelLayout = id;
                });
                grid->addLayout(layoutRow, row++, 0, 1, 4);

                auto* subBox = new QCheckBox("Subpixel sampling (needs 3x or more internal resolution)", win);
                subBox->setToolTip("Lets each LED stripe show what's actually under it, the way ClearType does with text.\n\n"
                                   "With the OpenGL renderer at 3x or more internal resolution, the 3D picture has at\n"
                                   "least three samples across each DS pixel. Each stripe then takes its own colour from\n"
                                   "the spot where that stripe sits: blue from the left third, green from the middle,\n"
                                   "red from the right (in the order chosen under Subpixels). That gives about three times\n"
                                   "the horizontal detail in 3D scenes, with faint coloured fringes on hard edges.\n\n"
                                   "Has no effect on 2D graphics, below 3x, or with Subpixels set to None.\n\n"
                                   "Default: off");
                subBox->setChecked(windowCfg.GetBool("SubpixelSample"));
                connect(subBox, &QCheckBox::toggled, this, [this](bool on)
                {
                    windowCfg.SetBool("SubpixelSample", on);
                    if (panel) panel->subpixelSample = on;
                });
                grid->addWidget(subBox, row++, 0, 1, 4);

                FilterExtras ex;
                ex.toJson = [=]()
                {
                    QJsonObject o;
                    if (reach[0]) o["BlurReachX"] = reach[0]->value();
                    if (reach[1]) o["BlurReachY"] = reach[1]->value();
                    o["SubpixelLayout"] = layoutGroup->checkedId();
                    o["SubpixelSample"] = subBox->isChecked();
                    return o;
                };
                ex.fromJson = [=](const QJsonObject& o)
                {
                    if (reach[0]) reach[0]->setValue(o.value("BlurReachX").toInt(1));
                    if (reach[1]) reach[1]->setValue(o.value("BlurReachY").toInt(1));
                    if (auto* b = layoutGroup->button(o.value("SubpixelLayout").toInt(3))) b->click();
                    subBox->setChecked(o.value("SubpixelSample").toBool(false));
                };
                return ex;
            };
            // built-in presets: the DS models' screens. What's known: the DS and DS Lite share a
            // 0.24 mm pitch (the DS's panel transflective, the Lite's transmissive); the DSi XL's
            // pixels are about 1.4x larger, and reviews describe its gaps between pixels and its
            // RGB stripes as plainly visible. No macro photo confirms any model's stripe order, so
            // those are the Subpixels setting's own (the original DS taking the DS Lite's)
            {
                auto lp = [](std::initializer_list<std::pair<const char*, double>> kv)
                {
                    QJsonObject o;
                    for (const auto& [k, v] : kv) o[k] = v;
                    return o;
                };
                lcd.builtins = {
                    {"DS (original)", lp({{"SubpixelLayout", 1}, {"GridDarkness", 0.18}, {"GridDarknessH", 0.5},
                                          {"SubpixelStrength", 0.6}, {"BlurStrengthX", 0.1}, {"BlurStrengthY", 0.05}})},
                    {"DS Lite", lp({{"SubpixelLayout", 1}, {"GridDarkness", 0.1}, {"GridDarknessH", 0.35},
                                    {"SubpixelStrength", 0.55}})},
                    {"DSi", lp({{"SubpixelLayout", 2}, {"GridDarkness", 0.1}, {"GridDarknessH", 0.35},
                                {"SubpixelStrength", 0.55}})},
                    {"DSi XL", lp({{"SubpixelLayout", 3}, {"GridDarkness", 0.3}, {"GridDarknessH", 0.55},
                                   {"SubpixelStrength", 0.8}, {"SubpixelKeep", 0.85},
                                   {"BlurStrengthX", 0.04}, {"BlurStrengthY", 0.02}})},
                    {QString(), QJsonObject()},
                    {"Grid only (no stripes)", lp({{"SubpixelLayout", 0}, {"GridDarkness", 0.25}, {"GridDarknessH", 0.5}})},
                    {"Soft and glowing", lp({{"SubpixelLayout", 0}, {"GridDarkness", 0.05}, {"GridDarknessH", 0.2},
                                             {"BlurStrengthX", 0.3}, {"BlurStrengthY", 0.2}, {"BlurHalo", 0.4}})},
                };
            }
            lcdWindow = buildFilterWindow(this, windowCfg, lcd);

            FilterWindowSpec crt;
            crt.title = "CRT filter settings";
            crt.filterName = "CRT filter";
            crt.params = kCrtParams;
            crt.count = kNumCrtParams;
            crt.setParam = [this](int i, float v) { if (panel) panel->crtParam[i] = v; };
            crt.setActive = [this](bool on) { onChangeScreenFilter(on ? screenFilter_CRT : screenFilter_Off); };
            crt.presetFile = localPath("crt-presets.json");
            crt.note = "Made for whole-number zooms (View > Screen sizing > Force integer scaling): "
                       "uneven ones give the scanlines a wavy pattern. The defaults suit 3x; at 6x or "
                       "more, a 2 px mask width keeps one triad per DS pixel.";

            // video signal: which console's encoder and which cable feed the TV. The combo boxes
            // go at the top of the "Video signal" box; the sliders that don't apply grey out
            struct SignalUi
            {
                QComboBox* console = nullptr; QComboBox* cable = nullptr; QComboBox* decoder = nullptr;
                std::function<void(int console, int select)> fillDecoders;
            };
            auto sigUi = std::make_shared<SignalUi>();
            auto updateSignalRows = [this, sigUi]()
            {
                if (!crtWindow || !sigUi->console) return;
                const int con = sigUi->console->currentData().toInt();
                const int cab = Ntsc::cableFor(con, sigUi->cable->currentIndex());
                const bool chain = con != Ntsc::Console_Off;
                // cables the console doesn't have (the NES: composite and RF only) can't be picked
                if (auto* m = qobject_cast<QStandardItemModel*>(sigUi->cable->model()))
                    for (int c = 0; c < Ntsc::Cable_Count; c++)
                        if (auto* it = m->item(c)) it->setEnabled(Ntsc::cableFor(con, c) == c);
                const bool encoded = chain && cab != Ntsc::Cable_RGB;
                const bool rf = encoded && cab == Ntsc::Cable_RF;
                sigUi->cable->setEnabled(chain);
                sigUi->decoder->setEnabled(encoded);
                crtWindow->setParamEnabled(crt_LumaBlur, !chain);
                crtWindow->setParamEnabled(crt_ChromaBlur, !chain);
                for (int p : {crt_Colour, crt_Tint, crt_Sharpness}) crtWindow->setParamEnabled(p, encoded);
                for (int p : {crt_Noise, crt_Ghost, crt_GhostDelay}) crtWindow->setParamEnabled(p, rf);
            };
            crt.sectionTop = [this, sigUi, updateSignalRows](const char* section, QGroupBox* box, QGridLayout* grid, int& row)
            {
                if (strcmp(section, "Video signal") != 0) return;
                auto* conLabel = new QLabel("Console", box);
                auto* con = new QComboBox(box);
                // each item's data is its Ntsc::Console value (the order here is only for the list)
                {
                    using namespace Ntsc;
                    const std::pair<const char*, int> items[] = {
                        {"Off (simple blur)", Console_Off}, {nullptr, 0},
                        {"NES / Famicom", Console_NES},
                        {"Mega Drive 1 / Genesis, 320 wide (H40)", Console_MD1H40},
                        {"Mega Drive 1 / Genesis, 256 wide (H32)", Console_MD1H32},
                        {"Mega Drive 2 / Genesis, 320 wide (H40)", Console_MDH40},
                        {"Mega Drive 2 / Genesis, 256 wide (H32)", Console_MDH32},
                        {"SNES / Super Famicom", Console_SNES},
                        {"PlayStation, 320 wide", Console_PS1},
                        {"PlayStation, 256 wide", Console_PS1W256},
                        {"Saturn, 352 wide", Console_Saturn352},
                        {"Nintendo 64", Console_N64}, {nullptr, 0},
                        {"Mega Drive 1 PAL, 320 wide (H40)", Console_MD1H40Pal},
                        {"Mega Drive 1 PAL, 256 wide (H32)", Console_MD1H32Pal},
                        {"Mega Drive 2 PAL, 320 wide (H40)", Console_MDH40Pal},
                        {"Mega Drive 2 PAL, 256 wide (H32)", Console_MDH32Pal},
                        {"SNES PAL", Console_SNESPal},
                        {"PlayStation PAL, 320 wide", Console_PS1Pal},
                    };
                    for (const auto& [label, value] : items)
                        if (label) con->addItem(label, value);
                        else con->insertSeparator(con->count());
                    con->setMaxVisibleItems(30);
                }
                const QString conTip =
                    "Which console's video encoder sends the picture to the TV, at that console's timing.\n\n"
                    "The DS picture is put through the signal the way that console makes it: each pixel\n"
                    "held for its clock, the colour carrier at its exact frequency and phase. Everything\n"
                    "you see (colour bleed, rainbows, dithering that blends) comes out of that signal and\n"
                    "how the TV decodes it, not from effect settings.\n\n"
                    "Mega Drive, 320 wide: 1.875 pixels per colour cycle, the same on every line, so\n"
                    "1-pixel dithering sits on the colour carrier: it blends, with still rainbows.\n"
                    "Mega Drive 1 vs 2: different encoder chips. The model 1's board filters (from\n"
                    "Sega's schematics) soften brightness and colour more.\n"
                    "Mega Drive, 256 wide and SNES: 1.5 pixels per colour cycle. The SNES shifts the\n"
                    "carrier a third of a cycle each line, so its artifacts run diagonally.\n"
                    "NES: makes the signal itself from its palette, so the picture is first shown in the\n"
                    "NES's colours (each colour as its nearest one), then sent as the NES sends them:\n"
                    "square waves with strong diagonal artifacts.\n"
                    "PlayStation, Saturn, N64: lines of the broadcast length, so the carrier flips every\n"
                    "line and artifacts flicker into a finer pattern (and a comb TV separates them well).\n"
                    "PlayStation and Saturn: the same encoder chip as the Mega Drive 2 (early\n"
                    "PlayStations). N64: the first boards' encoder, which has no filters, and the N64's\n"
                    "own way of doubling each pixel (the second half blended with the next pixel).\n"
                    "PAL consoles: the European PAL signal, with its own carrier (4.43 MHz) and V's phase\n"
                    "flipped every line; a PAL TV's delay line averages colour over two lines, so colour\n"
                    "edges are softer top to bottom and hue errors cancel out.\n\n"
                    "Off: no signal; Picture blur and Colour bleed set a simple sideways blur.\n\n"
                    "Default: Off";
                conLabel->setToolTip(conTip); con->setToolTip(conTip);
                auto* cabLabel = new QLabel("Cable", box);
                auto* cab = new QComboBox(box);
                cab->addItems({"RGB", "S-Video", "Composite", "RF"});
                const QString cabTip =
                    "How the signal gets to the TV.\n\n"
                    "RGB: no encoding at all, the sharpest picture. (Not on an NES.)\n"
                    "S-Video: brightness and colour on separate wires: colour is softer than brightness,\n"
                    "but there are no rainbows. (Not on an NES or a Mega Drive 1.)\n"
                    "Composite: both on one wire; the TV has to pull them apart, so fine detail turns into\n"
                    "colour (rainbows) and colour leaks into brightness.\n"
                    "RF: composite through an aerial lead and tuner: a little softer, and it can add\n"
                    "noise and a ghost (the RF sliders).\n\n"
                    "Default: Composite";
                cabLabel->setToolTip(cabTip); cab->setToolTip(cabTip);
                {
                    QSignalBlocker b1(con), b2(cab);
                    const int ci = con->findData(windowCfg.GetInt("CrtSignal"));
                    con->setCurrentIndex(ci < 0 ? 0 : ci);
                    cab->setCurrentIndex(std::clamp(windowCfg.GetInt("CrtCable"), 0, Ntsc::Cable_Count - 1));
                }
                auto* decLabel = new QLabel("TV decoder", box);
                auto* dec = new QComboBox(box);
                const QString decTip =
                    "How the TV turns the colour signal back into colour; the choices follow the console.\n\n"
                    "NTSC:\n"
                    "Standard: the NTSC standard's colour axes, so colours come out as they went in.\n"
                    "Sony US / Japanese: the axes of Sony's TV decoder IC from its datasheet. US sets\n"
                    "pushed red and warmed skin tones (reds stronger, yellows toward orange); the\n"
                    "Japanese setting is milder. Most TVs of the time did something like this.\n"
                    "Comb filter: separates colour from brightness using the line above (RCA's design,\n"
                    "with a delay of exactly one broadcast line), instead of a notch. Far fewer\n"
                    "rainbows on consoles whose lines are the broadcast length (PlayStation, Saturn,\n"
                    "N64); on the others the line above lands slightly to the side, so it helps less,\n"
                    "and colour edges between rows can leave dots.\n\n"
                    "PAL:\n"
                    "PAL-D, 1990s: the delay line nearly every PAL set had, as Philips' TDA4665 did it:\n"
                    "each line's colour averaged with the line above's, locked to the console's lines.\n"
                    "Colour is softer top to bottom and hue errors cancel out.\n"
                    "PAL-D, 1980s: the older glass delay line, a fixed 64 us minus a quarter cycle. These\n"
                    "consoles' lines are longer than broadcast PAL's, so the line above's colour lands a\n"
                    "little to the right (2.25 pixels on a 320-wide Mega Drive), smearing colour edges.\n"
                    "PAL-S: no delay line. Sharper colour top to bottom, but any phase error shows as\n"
                    "alternating lines of slightly different hue (Hanover bars).\n\n"
                    "Not used with RGB.\n\nDefault: Standard (NTSC), or PAL-D 1990s for a PAL console";
                decLabel->setToolTip(decTip); dec->setToolTip(decTip);
                // the decoders that fit the console, each item's data its Ntsc::Decoder value
                auto fillDecoders = [dec](int console, int select)
                {
                    QSignalBlocker b(dec);
                    dec->clear();
                    if (Ntsc::isPal(console))
                    {
                        dec->addItem("PAL-D, 1990s TV (TDA4665 delay line)", (int)Ntsc::Decoder_PalD);
                        dec->addItem("PAL-D, 1980s TV (glass delay line)", (int)Ntsc::Decoder_PalDGlass);
                        dec->addItem("PAL-S (no delay line)", (int)Ntsc::Decoder_PalS);
                    }
                    else
                    {
                        dec->addItem("Standard (NTSC)", (int)Ntsc::Decoder_Standard);
                        dec->addItem("Sony, US TV (CXA2025AS)", (int)Ntsc::Decoder_SonyUS);
                        dec->addItem("Sony, Japanese TV (CXA2025AS)", (int)Ntsc::Decoder_SonyJP);
                        dec->addItem("Comb filter TV (2-line, CCD)", (int)Ntsc::Decoder_Comb);
                    }
                    int i = dec->findData(Ntsc::decoderFor(console, select));
                    dec->setCurrentIndex(i < 0 ? 0 : i);
                };
                fillDecoders(con->currentData().toInt(), windowCfg.GetInt("CrtDecoder"));
                sigUi->fillDecoders = fillDecoders;
                auto storeDecoder = [this, dec]()
                {
                    int d = dec->currentData().toInt();
                    windowCfg.SetInt("CrtDecoder", d);
                    if (panel) panel->crtDecoder = d;
                };
                connect(dec, &QComboBox::currentIndexChanged, this, [storeDecoder](int) { storeDecoder(); });
                connect(con, &QComboBox::currentIndexChanged, this, [this, con, cab, updateSignalRows, fillDecoders, dec, storeDecoder](int idx)
                {
                    const int i = con->itemData(idx).toInt();
                    // a cable the console doesn't have (an NES on RGB) becomes the one it would use
                    if (Ntsc::cableFor(i, cab->currentIndex()) != cab->currentIndex())
                        cab->setCurrentIndex(Ntsc::cableFor(i, cab->currentIndex()));
                    windowCfg.SetInt("CrtSignal", i);
                    if (panel) panel->crtSignal = i;
                    fillDecoders(i, dec->currentData().toInt());   // keeps the choice if it fits
                    storeDecoder();
                    updateSignalRows();
                });
                connect(cab, &QComboBox::currentIndexChanged, this, [this, updateSignalRows](int i)
                {
                    windowCfg.SetInt("CrtCable", i);
                    if (panel) panel->crtCable = i;
                    updateSignalRows();
                });
                grid->addWidget(conLabel, row, 0);
                grid->addWidget(con, row++, 1, 1, 2);
                grid->addWidget(cabLabel, row, 0);
                grid->addWidget(cab, row++, 1, 1, 2);
                grid->addWidget(decLabel, row, 0);
                grid->addWidget(dec, row++, 1, 1, 2);
                sigUi->console = con;
                sigUi->cable = cab;
                sigUi->decoder = dec;
            };
            crt.addExtras = [sigUi](QWidget*, QGridLayout*, int, int&)
            {
                FilterExtras ex;
                ex.toJson = [sigUi]()
                {
                    QJsonObject o;
                    o["CrtSignal"] = sigUi->console->currentData().toInt();
                    o["CrtCable"] = sigUi->cable->currentIndex();
                    o["CrtDecoder"] = sigUi->decoder->currentData().toInt();
                    return o;
                };
                ex.fromJson = [sigUi](const QJsonObject& o)
                {
                    const int ci = sigUi->console->findData(o.value("CrtSignal").toInt(0));
                    sigUi->console->setCurrentIndex(ci < 0 ? 0 : ci);
                    sigUi->cable->setCurrentIndex(std::clamp(o.value("CrtCable").toInt(Ntsc::Cable_Composite), 0, Ntsc::Cable_Count - 1));
                    // the console's change already refilled the decoders; pick the preset's
                    const int want = Ntsc::decoderFor(sigUi->console->currentData().toInt(), o.value("CrtDecoder").toInt(Ntsc::Decoder_Standard));
                    int i = sigUi->decoder->findData(want);
                    sigUi->decoder->setCurrentIndex(i < 0 ? 0 : i);
                };
                return ex;
            };

            // built-in presets: a console's signal through a fitting TV. A key not listed keeps
            // its default (Default's tube settings suit a consumer TV at 3x)
            auto preset = [](std::initializer_list<std::pair<const char*, double>> kv)
            {
                QJsonObject o;
                for (const auto& [k, v] : kv) o[k] = v;
                return o;
            };
            // The signal part of each is a real hardware choice (console, cable, TV decoder); the
            // tube part (curvature, scanlines, mask, glow) is the look of that kind of set
            using namespace Ntsc;
            const std::pair<const char*, double> rf[] = {{"CrtNoise", 0.15}, {"CrtGhost", 0.08}, {"CrtGhostDelay", 1.2}};
            auto withRf = [&](std::initializer_list<std::pair<const char*, double>> kv)
            {
                QJsonObject o = preset(kv);
                for (const auto& [k, v] : rf) o[k] = v;
                return o;
            };
            const std::pair<QString, QJsonObject> sep{QString(), QJsonObject()};
            crt.builtins = {
                // NES: the PPU's own composite wave (RF only on the Famicom)
                {"NES - composite", preset({{"CrtSignal", Console_NES}, {"CrtCable", Cable_Composite}})},
                {"NES - RF", withRf({{"CrtSignal", Console_NES}, {"CrtCable", Cable_RF}})},
                {"Famicom - RF, Japanese TV", withRf({{"CrtSignal", Console_NES}, {"CrtCable", Cable_RF},
                                                      {"CrtDecoder", Decoder_SonyJP}})},
                sep,
                // Mega Drive 1 / Genesis: Sony's CXA1145 on Sega's board networks
                {"Mega Drive 1 / Genesis - composite", preset({{"CrtSignal", Console_MD1H40}, {"CrtCable", Cable_Composite}})},
                {"Mega Drive 1 / Genesis - RF", withRf({{"CrtSignal", Console_MD1H40}, {"CrtCable", Cable_RF}})},
                {"Mega Drive 1 (PAL) - composite", preset({{"CrtSignal", Console_MD1H40Pal}, {"CrtCable", Cable_Composite},
                                                           {"CrtDecoder", Decoder_PalD}})},
                sep,
                // Mega Drive 2 / Genesis, NTSC: Sony's CXA1645 encoder
                {"Mega Drive 2 / Genesis - composite", preset({{"CrtSignal", Console_MDH40}, {"CrtCable", Cable_Composite}})},
                {"Mega Drive 2 / Genesis - composite, US TV", preset({{"CrtSignal", Console_MDH40}, {"CrtCable", Cable_Composite},
                                                                      {"CrtDecoder", Decoder_SonyUS}})},
                {"Mega Drive 2 / Genesis - composite, 256 wide", preset({{"CrtSignal", Console_MDH32}, {"CrtCable", Cable_Composite}})},
                {"Mega Drive 2 / Genesis - S-Video (modded)", preset({{"CrtSignal", Console_MDH40}, {"CrtCable", Cable_SVideo}})},
                {"Mega Drive 2 / Genesis - RF", withRf({{"CrtSignal", Console_MDH40}, {"CrtCable", Cable_RF}})},
                {"Mega Drive 2 (Japan) - composite, Japanese TV", preset({{"CrtSignal", Console_MDH40}, {"CrtCable", Cable_Composite},
                                                                          {"CrtDecoder", Decoder_SonyJP}})},
                sep,
                // Mega Drive 2, PAL
                {"Mega Drive 2 (PAL) - composite", preset({{"CrtSignal", Console_MDH40Pal}, {"CrtCable", Cable_Composite},
                                                           {"CrtDecoder", Decoder_PalD}})},
                {"Mega Drive 2 (PAL) - composite, 1980s TV", preset({{"CrtSignal", Console_MDH40Pal}, {"CrtCable", Cable_Composite},
                                                                     {"CrtDecoder", Decoder_PalDGlass}})},
                {"Mega Drive 2 (PAL) - RF", withRf({{"CrtSignal", Console_MDH40Pal}, {"CrtCable", Cable_RF}, {"CrtDecoder", Decoder_PalD}})},
                {"Mega Drive 2 (PAL) - RGB SCART", preset({{"CrtSignal", Console_MDH40Pal}, {"CrtCable", Cable_RGB}})},
                sep,
                // SNES / Super Famicom, NTSC
                {"SNES - S-Video", preset({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_SVideo}})},
                {"SNES - composite", preset({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_Composite}})},
                {"SNES - composite, US TV", preset({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_Composite},
                                                    {"CrtDecoder", Decoder_SonyUS}})},
                {"SNES - RF", withRf({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_RF}})},
                {"Super Famicom - composite, Japanese TV", preset({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_Composite},
                                                                   {"CrtDecoder", Decoder_SonyJP}})},
                {"SNES - composite, comb filter TV", preset({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_Composite},
                                                             {"CrtDecoder", Decoder_Comb}})},
                sep,
                // SNES, PAL
                {"SNES (PAL) - composite", preset({{"CrtSignal", Console_SNESPal}, {"CrtCable", Cable_Composite},
                                                   {"CrtDecoder", Decoder_PalD}})},
                {"SNES (PAL) - composite, PAL-S TV", preset({{"CrtSignal", Console_SNESPal}, {"CrtCable", Cable_Composite},
                                                             {"CrtDecoder", Decoder_PalS}})},
                {"SNES (PAL) - RGB SCART", preset({{"CrtSignal", Console_SNESPal}, {"CrtCable", Cable_RGB}})},
                sep,
                // the 32/64-bit generation: broadcast-length lines, so a comb TV separates them well
                {"PlayStation - composite", preset({{"CrtSignal", Console_PS1}, {"CrtCable", Cable_Composite}})},
                {"PlayStation - composite, comb filter TV", preset({{"CrtSignal", Console_PS1}, {"CrtCable", Cable_Composite},
                                                                    {"CrtDecoder", Decoder_Comb}})},
                {"PlayStation - S-Video", preset({{"CrtSignal", Console_PS1}, {"CrtCable", Cable_SVideo}})},
                {"PlayStation (PAL) - composite", preset({{"CrtSignal", Console_PS1Pal}, {"CrtCable", Cable_Composite},
                                                          {"CrtDecoder", Decoder_PalD}})},
                {"Saturn - composite", preset({{"CrtSignal", Console_Saturn352}, {"CrtCable", Cable_Composite}})},
                {"Saturn - S-Video", preset({{"CrtSignal", Console_Saturn352}, {"CrtCable", Cable_SVideo}})},
                {"Nintendo 64 - composite", preset({{"CrtSignal", Console_N64}, {"CrtCable", Cable_Composite}})},
                {"Nintendo 64 - composite, comb filter TV", preset({{"CrtSignal", Console_N64}, {"CrtCable", Cable_Composite},
                                                                    {"CrtDecoder", Decoder_Comb}})},
                {"Nintendo 64 - S-Video", preset({{"CrtSignal", Console_N64}, {"CrtCable", Cable_SVideo}})},
                sep,
                // kinds of set, fed RGB (or, the portable, a weak aerial signal)
                {"Sony PVM - RGB", preset({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_RGB},
                                           {"CrtCurvature", 0.0}, {"CrtBeamMin", 0.16}, {"CrtBeamMax", 0.28},
                                           {"CrtMask", 0.7}, {"CrtGlow", 0.05}, {"CrtVignette", 0.05}, {"CrtBoost", 1.35}})},
                {"Sony BVM - RGB", preset({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_RGB},
                                           {"CrtCurvature", 0.0}, {"CrtBeamMin", 0.13}, {"CrtBeamMax", 0.22},
                                           {"CrtMask", 0.45}, {"CrtGlow", 0.02}, {"CrtVignette", 0.02}, {"CrtBoost", 1.5}})},
                {"Arcade monitor - RGB", preset({{"CrtSignal", Console_SNES}, {"CrtCable", Cable_RGB},
                                                 {"CrtCurvature", 0.4}, {"CrtBeamMin", 0.17}, {"CrtBeamMax", 0.45},
                                                 {"CrtMask", 0.85}, {"CrtGlow", 0.18}, {"CrtVignette", 0.3}, {"CrtBoost", 1.15}})},
                {"Portable TV - RF, weak signal", preset({{"CrtSignal", Console_MDH40}, {"CrtCable", Cable_RF},
                                                          {"CrtNoise", 0.4}, {"CrtGhost", 0.1}, {"CrtGhostDelay", 1.6},
                                                          {"CrtColour", 1.15}, {"CrtSharpness", 0.35},
                                                          {"CrtCurvature", 0.65}, {"CrtBeamMin", 0.24}, {"CrtBeamMax", 0.5},
                                                          {"CrtMaskPx", 2}, {"CrtGlow", 0.2}, {"CrtVignette", 0.45}, {"CrtBoost", 1.1}})},
            };
            crtWindow = buildFilterWindow(this, windowCfg, crt);
            updateSignalRows();
        }

        connect(actLcdSettings, &QAction::triggered, this, [this]()
        {
            lcdWindow->show();
            lcdWindow->raise();
            lcdWindow->activateWindow();
        });
        connect(actCrtSettings, &QAction::triggered, this, [this]()
        {
            crtWindow->show();
            crtWindow->raise();
            crtWindow->activateWindow();
        });
        lcdWindow->onClosed = [this]()
        {
            windowCfg.SetString("HelloGeometry", lcdWindow->saveGeometry().toBase64().toStdString());
            windowCfg.SetBool("HelloVisible", false);
        };
        crtWindow->onClosed = [this]()
        {
            windowCfg.SetString("CrtGeometry", crtWindow->saveGeometry().toBase64().toStdString());
            windowCfg.SetBool("CrtVisible", false);
        };

        if (localCfg.GetString("Firmware.Username") == "Arisotura")
            actMPNewInstance->setText("Fart");
    }

#ifdef Q_OS_MAC
    QPoint screenCenter = screen()->availableGeometry().center();
    QRect frameGeo = frameGeometry();
    frameGeo.moveCenter(screenCenter);
    move(frameGeo.topLeft());
#endif

    std::string geom = embedded ? std::string() : windowCfg.GetString("Geometry");
    if (!geom.empty())
    {
        QByteArray raw = QByteArray::fromStdString(geom);
        QByteArray dec = QByteArray::fromBase64(raw, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!dec.isEmpty())
            restoreGeometry(dec);
        // if the window was closed in fullscreen do not restore this
        setWindowState(windowState() & ~Qt::WindowFullScreen);
    }
    if (embedded || id != 0 || !MainWindowStartHidden) show();

    // screen filter settings windows: where they were, and open if they were open
    for (auto [fw, geomKey, visKey] : {std::tuple{lcdWindow, "HelloGeometry", "HelloVisible"},
                                       std::tuple{crtWindow, "CrtGeometry", "CrtVisible"}})
    {
        if (!fw) continue;
        std::string g = windowCfg.GetString(geomKey);
        if (!g.empty())
            fw->restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(g)));
        if (windowCfg.GetBool(visKey))
            fw->show();
    }

    panel = nullptr;
    createScreenPanel();

    // recording indicator (status bar shown while recording)
    if (WBHost* wb = emuInstance->getWorkbench())
    {
        wbStatus = new WBStatusWidget(wb, this);
        statusBar()->addWidget(wbStatus, 1);
        statusBar()->setVisible(false);
        // statusChanged comes from the emulation thread, queued: one can still be waiting when the
        // instance has deleted its host (quitting with recording on)
        QPointer<WBHost> host = wb;
        connect(wb, &WBHost::statusChanged, this, [this, host]()
        {
            if (!host) return;
            WBStatus s = host->Status();
            bool show = s.Recording;
            if (statusBar()->isVisible() != show) statusBar()->setVisible(show);
            if (hasMenu && actRecording)
            {
                actRecording->setText(s.Recording ? "Stop recording" : "Start recording");
                actRecording->setEnabled(s.Recording || (s.Available && s.EmuState != WBStatus::NoGame));
            }
            wbStatus->refresh();
        });
    }

    if (hasMenu)
    {
        actEjectCart->setEnabled(false);
        actEjectGBACart->setEnabled(false);

        if (globalCfg.GetInt("Emu.ConsoleType") == 1)
        {
            actInsertGBACart->setEnabled(false);
            for (auto act: actInsertGBAAddon)
                act->setEnabled(false);
        }

        for (int i = 0; i < 9; i++)
        {
            actSaveState[i]->setEnabled(false);
            actLoadState[i]->setEnabled(false);
        }
        actUndoStateLoad->setEnabled(false);
        actImportSavefile->setEnabled(false);

        actPause->setEnabled(false);
        actReset->setEnabled(false);
        actStop->setEnabled(false);
        actFrameStep->setEnabled(false);

        //actDateTime->setEnabled(true);
        actPowerManagement->setEnabled(false);

        actEnableCheats->setEnabled(false);
        actSetupCheats->setEnabled(false);
        actTitleManager->setEnabled(!globalCfg.GetString("DSi.NANDPath").empty());

        actEnableCheats->setChecked(localCfg.GetBool("EnableCheats"));

        actROMInfo->setEnabled(false);
        actRAMInfo->setEnabled(false);

        actScreenRotation[windowCfg.GetInt("ScreenRotation")]->setChecked(true);

        int screenGap = windowCfg.GetInt("ScreenGap");
        for (int i = 0; i < 7; i++)
        {
            if (actScreenGap[i]->data().toInt() == screenGap)
            {
                actScreenGap[i]->setChecked(true);
                break;
            }
        }

        actScreenLayout[windowCfg.GetInt("ScreenLayout")]->setChecked(true);
        actScreenSizing[windowCfg.GetInt("ScreenSizing")]->setChecked(true);
        actIntegerScaling->setChecked(windowCfg.GetBool("IntegerScaling"));

        actScreenSwap->setChecked(windowCfg.GetBool("ScreenSwap"));

        int aspectTop = windowCfg.GetInt("ScreenAspectTop");
        int aspectBot = windowCfg.GetInt("ScreenAspectBot");
        for (int i = 0; i < AspectRatiosNum; i++)
        {
            if (aspectTop == aspectRatios[i].id)
                actScreenAspectTop[i]->setChecked(true);
            if (aspectBot == aspectRatios[i].id)
                actScreenAspectBot[i]->setChecked(true);
        }

        updateScreenFilterMenu();
        actShowOSD->setChecked(showOSD);

        actLimitFramerate->setChecked(emuInstance->doLimitFPS);
        actAudioSync->setChecked(emuInstance->doAudioSync);

        if (emuInstance->instanceID > 0)
        {
            actEmuSettings->setEnabled(false);
            actVideoSettings->setEnabled(false);
            actMPSettings->setEnabled(false);
            actWifiSettings->setEnabled(false);
            actInterfaceSettings->setEnabled(false);

#ifdef __APPLE__
            actPreferences->setEnabled(false);
#endif // __APPLE__
        }

        if (emuThread->emuIsActive())
            onEmuStart();
    }

    QObject::connect(qApp, &QApplication::applicationStateChanged, this, &MainWindow::onAppStateChanged);
    onUpdateInterfaceSettings();

    updateMPInterface(MPInterface::GetType());
}

MainWindow::~MainWindow()
{
    if (hasMenu)
    {
        delete[] actScreenAspectTop;
        delete[] actScreenAspectBot;
    }
}

void MainWindow::osdAddMessage(unsigned int color, const char* msg)
{
    if (!showOSD) return;
    panel->osdAddMessage(color, msg);
}

void MainWindow::saveEnabled(bool enabled)
{
    if (enabledSaved) return;
    // an embedded window lives in the workbench, which recreates it: never reopen it on its own
    if (embedded) enabled = false;
    windowCfg.SetBool("Enabled", enabled);
    enabledSaved = true;
}

void MainWindow::saveFilterWindowState(bool closing)
{
    // the LCD window keeps the keys it had as the pixel grid window
    for (auto [fw, geomKey, visKey] : {std::tuple{lcdWindow, "HelloGeometry", "HelloVisible"},
                                       std::tuple{crtWindow, "CrtGeometry", "CrtVisible"}})
    {
        if (!fw) continue;
        windowCfg.SetString(geomKey, fw->saveGeometry().toBase64().toStdString());
        if (closing) windowCfg.SetBool(visKey, fw->isVisible());
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (emuInstance)
    {
        if (windowID == 0)
            emuInstance->saveEnabledWindows();
        else
            saveEnabled(false);
    }

    // explicitly close children windows, so the OpenGL contexts get closed properly
    auto childwins = findChildren<MainWindow *>(nullptr, Qt::FindDirectChildrenOnly);
    for (auto child : childwins)
        child->close();

    if (!emuInstance) return;

    if (!embedded)
    {
        QByteArray geom = saveGeometry();
        QByteArray enc = geom.toBase64(QByteArray::Base64Encoding);
        windowCfg.SetString("Geometry", enc.toStdString());
    }
    saveFilterWindowState(true);
    Config::Save();

    emuInstance->deleteWindow(windowID, false);

    // emuInstance may be deleted
    // prevent use after free from us
    emuInstance = nullptr;
    QMainWindow::closeEvent(event);
}

void MainWindow::createScreenPanel()
{
    TRACE_QT("MainWindow::createScreenPanel");
    auto oldpanel = panel;
    panel = nullptr;
    if (oldpanel) delete oldpanel;

    // One decision for every window of the instance: a window's OpenGL context is shared
    // with the first window's, so they must all use OpenGL or none. The OpenGL screen filters
    // need it; the software ones draw on the plain display (SoftLcdFilter, SoftCrtFilter).
    hasOGL = emuInstance->usesOpenGL();
    updateScreenFilterMenu();

    if (hasOGL)
    {
        ScreenPanelGL* panelGL = new ScreenPanelGL(this);
        panelGL->show();

        // make sure no GL context is in use by the emu thread
        // otherwise we may fail to create a shared context
        if (windowID != 0)
            emuThread->borrowGL();

        // Check that creating the context hasn't failed
        if (panelGL->createContext() == false)
        {
            Log(Platform::LogLevel::Error, "Failed to create OpenGL context, falling back to Software Renderer.\n");
            hasOGL = false;

            globalCfg.SetBool("Screen.UseGL", false);
            globalCfg.SetInt("3D.Renderer", renderer3D_Software);

            delete panelGL;
            panelGL = nullptr;
        }

        if (windowID != 0)
            emuThread->returnGL();

        panel = panelGL;
    }

    if (!hasOGL)
    {
        ScreenPanelNative* panelNative = new ScreenPanelNative(this);
        panel = panelNative;
        panel->show();
    }
    if (embedded)
    {
        // centred in a box, so a zoom (onChangeScreenSize) leaves the menus at the top
        QWidget* box = new QWidget(this);
        // black around a zoomed screen, like the gap between the screens
        box->setAttribute(Qt::WA_StyledBackground);
        box->setStyleSheet("background: black;");
        QGridLayout* g = new QGridLayout(box);
        g->setContentsMargins(0, 0, 0, 0);
        // no alignment flag: an aligned widget stops at its size hint; without one it fills up to its
        // maximum size and is centred in what is left
        g->addWidget(panel, 0, 0);
        setCentralWidget(box);
    }
    else setCentralWidget(panel);

    panel->osdSetEnabled(showOSD);

    // update(), not repaint(): a repaint per frame per window waits on the display server each time, and
    // with two windows shown (game in the workbench and the main window) that starves the GUI thread
    connect(emuThread, SIGNAL(windowUpdate()), panel, SLOT(update()));

    connect(this, SIGNAL(screenLayoutChange()), panel, SLOT(onScreenLayoutChanged()));
    emit screenLayoutChange();
}

GL::Context* MainWindow::getOGLContext()
{
    if (!hasOGL) return nullptr;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    return glpanel->getContext();
}

void MainWindow::initOpenGL()
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    return glpanel->initOpenGL();
}

void MainWindow::deinitOpenGL()
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    return glpanel->deinitOpenGL();
}

void MainWindow::setGLSwapInterval(int intv)
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    if (!glpanel) return;
    return glpanel->setSwapInterval(intv);
}

void MainWindow::makeCurrentGL()
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    if (!glpanel) return;
    return glpanel->makeCurrentGL();
}

void MainWindow::releaseGL()
{
    if (!hasOGL) return;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    if (!glpanel) return;
    return glpanel->releaseGL();
}

void MainWindow::drawScreen()
{
    if (!panel) return;
    return panel->drawScreen();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) return;

    // TODO!! REMOVE ME IN RELEASE BUILDS!!
    //if (event->key() == Qt::Key_F11) emuInstance->getNDS()->debug(0);

    emuInstance->onKeyPress(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) return;

    emuInstance->onKeyRelease(event);
}


void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;

    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.count() > 1) return; // not handling more than one file at once

    QString filename = urls.at(0).toLocalFile();

    if (FileIsSupportedFiletype(filename))
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;

    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.count() > 1) return; // not handling more than one file at once

    if (!verifySetup())
        return;

    const QStringList file = splitArchivePath(urls.at(0).toLocalFile(), false);
    if (file.isEmpty())
        return;

    const QString filename = file.last();
    const bool romInsideArchive = file.size() > 1;
    const auto matchMode = romInsideArchive ? QMimeDatabase::MatchExtension : QMimeDatabase::MatchDefault;
    const QMimeType mimetype = QMimeDatabase().mimeTypeForFile(filename, matchMode);

    bool isNdsRom = NdsRomByExtension(filename) || NdsRomByMimetype(mimetype);
    bool isGbaRom = GbaRomByExtension(filename) || GbaRomByMimetype(mimetype);
    isNdsRom |= ZstdNdsRomByExtension(filename);
    isGbaRom |= ZstdGbaRomByExtension(filename);

    QString errorstr;
    if (isNdsRom)
    {
        if (!emuThread->bootROM(file, errorstr))
        {
            QMessageBox::critical(this, "melonDS", errorstr);
            return;
        }

        const QString barredFilename = file.join('|');
        recentFileList.removeAll(barredFilename);
        recentFileList.prepend(barredFilename);
        updateRecentFilesMenu();

        updateCartInserted(false);
    }
    else if (isGbaRom)
    {
        if (!emuThread->insertCart(file, true, errorstr))
        {
            QMessageBox::critical(this, "melonDS", errorstr);
            return;
        }

        updateCartInserted(true);
    }
    else
    {
        QMessageBox::critical(this, "melonDS", "The file could not be recognized as a DS or GBA ROM.");
        return;
    }
}

void MainWindow::focusInEvent(QFocusEvent* event)
{
    onFocusIn();
}

void MainWindow::focusOutEvent(QFocusEvent* event)
{
    onFocusOut();
}

void MainWindow::onFocusIn()
{
    focused = true;
    if (emuInstance)
        emuInstance->updateAudioMuteByWindowFocus();
}

void MainWindow::onFocusOut()
{
    // focusOutEvent is called through the window close event handler
    // prevent use after free
    focused = false;
    if (emuInstance)
        emuInstance->updateAudioMuteByWindowFocus();
}

void MainWindow::onAppStateChanged(Qt::ApplicationState state)
{
    // closed: the instance and its emulation thread may be gone, while another window (the workbench)
    // still changes the application state
    if (!emuInstance) return;

    if (state == Qt::ApplicationInactive)
    {
        emuInstance->keyReleaseAll();
        if (pauseOnLostFocus && emuThread->emuIsRunning())
            emuThread->emuPause();
    }
    else if (state == Qt::ApplicationActive)
    {
        if (pauseOnLostFocus && !pausedManually)
            emuThread->emuUnpause();
    }
}

bool MainWindow::verifySetup()
{
    QString res = emuInstance->verifySetup();
    if (!res.isEmpty())
    {
         QMessageBox::critical(this, "melonDS", res);
         return false;
    }

    return true;
}

bool MainWindow::preloadROMs(QStringList file, QStringList gbafile, bool boot)
{
    if (!preloadROMsStart(file, gbafile, boot)) return false;
    return preloadROMsFinish();
}

bool MainWindow::preloadROMsStart(QStringList file, QStringList gbafile, bool boot)
{
    QString errorstr;
    preload = {};

    if (file.isEmpty() && gbafile.isEmpty() && !boot)
        return false;

    if (!verifySetup())
    {
        return false;
    }

    bool gbaloaded = false;
    if (!gbafile.isEmpty())
    {
        if (!emuThread->insertCart(gbafile, true, errorstr))
        {
            QMessageBox::critical(this, "melonDS", errorstr);
            return false;
        }

        gbaloaded = true;
    }

    preload.Active = true;
    preload.File = file;
    preload.Boot = boot;
    preload.GBALoaded = gbaloaded;
    // booting a DS ROM runs on the emulation thread while the caller goes on; preloadROMsFinish waits
    if (!file.isEmpty() && boot)
        emuThread->bootROMStart(file);
    return true;
}

bool MainWindow::preloadROMsFinish()
{
    if (!preload.Active) return false;
    const QStringList file = preload.File;
    const bool boot = preload.Boot;
    const bool gbaloaded = preload.GBALoaded;
    preload = {};
    QString errorstr;

    bool ndsloaded = false;
    if (!file.isEmpty())
    {
        if (boot)
        {
            if (!emuThread->bootROMFinish(errorstr))
            {
                QMessageBox::critical(this, "melonDS", errorstr);
                return false;
            }
        }
        else
        {
            if (!emuThread->insertCart(file, false, errorstr))
            {
                QMessageBox::critical(this, "melonDS", errorstr);
                return false;
            }
        }

        recentFileList.removeAll(file.join("|"));
        recentFileList.prepend(file.join("|"));
        updateRecentFilesMenu();
        ndsloaded = true;
    }
    else if (boot)
    {
        if (!emuThread->bootFirmware(errorstr))
        {
            QMessageBox::critical(this, "melonDS", errorstr);
            return false;
        }
    }

    updateCartInserted(false);
    if (gbaloaded)
        updateCartInserted(true);

    return true;
}

QStringList MainWindow::splitArchivePath(const QString& filename, bool useMemberSyntax)
{
    if (filename.isEmpty()) return {};

#ifdef ARCHIVE_SUPPORT_ENABLED
    if (useMemberSyntax)
    {
        const QStringList filenameParts = filename.split('|');
        if (filenameParts.size() > 2)
        {
            QMessageBox::warning(this, "melonDS", "This path contains too many '|'.");
            return {};
        }

        if (filenameParts.size() == 2)
        {
            const QString archive = filenameParts.at(0);
            if (!QFileInfo(archive).exists())
            {
                QMessageBox::warning(this, "melonDS", "This archive does not exist.");
                return {};
            }

            const QString subfile = filenameParts.at(1);
            if (!Archive::ListArchive(archive).contains(subfile))
            {
                QMessageBox::warning(this, "melonDS", "This archive does not contain the desired file.");
                return {};
            }

            return filenameParts;
        }
    }
#endif

    if (!QFileInfo(filename).exists())
    {
        QMessageBox::warning(this, "melonDS", "This ROM file does not exist.");
        return {};
    }

#ifdef ARCHIVE_SUPPORT_ENABLED
    if (SupportedArchiveByExtension(filename)
        || SupportedArchiveByMimetype(QMimeDatabase().mimeTypeForFile(filename)))
    {
        const QString subfile = pickFileFromArchive(filename);
        if (subfile.isEmpty())
            return {};

        return { filename, subfile };
    }
#endif

    return { filename };
}

QString MainWindow::pickFileFromArchive(QString archiveFileName)
{
    QVector<QString> archiveROMList = Archive::ListArchive(archiveFileName);

    if (archiveROMList.size() <= 1)
    {
        if (!archiveROMList.isEmpty() && archiveROMList.at(0) == "OK")
            QMessageBox::warning(this, "melonDS", "This archive is empty.");
        else
            QMessageBox::critical(this, "melonDS", "This archive could not be read. It may be corrupt or you don't have the permissions.");
        return QString();
    }

    archiveROMList.removeFirst();

    const auto notSupportedRom = [&](const auto& filename){
        if (NdsRomByExtension(filename) || GbaRomByExtension(filename))
            return false;
        const QMimeType mimetype = QMimeDatabase().mimeTypeForFile(filename, QMimeDatabase::MatchExtension);
        return !(NdsRomByMimetype(mimetype) || GbaRomByMimetype(mimetype));
    };

    archiveROMList.erase(std::remove_if(archiveROMList.begin(), archiveROMList.end(), notSupportedRom),
                         archiveROMList.end());

    if (archiveROMList.isEmpty())
    {
        QMessageBox::warning(this, "melonDS", "This archive does not contain any supported ROMs.");
        return QString();
    }

    if (archiveROMList.size() == 1)
        return archiveROMList.first();

    bool ok;
    const QString toLoad = QInputDialog::getItem(
        this, "melonDS",
        "This archive contains multiple files. Select which ROM you want to load.",
        archiveROMList.toList(), 0, false, &ok
    );

    if (ok) return toLoad;

    // User clicked on cancel

    return QString();
}

QStringList MainWindow::pickROM(bool gba)
{
    emuThread->emuPause();

    const QString console = gba ? "GBA" : "DS";
    const QStringList& romexts = gba ? GbaRomExtensions : NdsRomExtensions;

    QString rawROMs = romexts.join(" *");
    QString extraFilters = ";;" + console + " ROMs (*" + rawROMs;
    QString allROMs = rawROMs;

    QString zstdROMs = "*" + romexts.join(".zst *") + ".zst";
    extraFilters += ");;Zstandard-compressed " + console + " ROMs (" + zstdROMs + ")";
    allROMs += " " + zstdROMs;

#ifdef ARCHIVE_SUPPORT_ENABLED
    QString archives = "*" + ArchiveExtensions.join(" *");
    extraFilters += ";;Archives (" + archives + ")";
    allROMs += " " + archives;
#endif
    extraFilters += ";;All files (*.*)";

    const QString filename = QFileDialog::getOpenFileName(
        this, "Open " + console + " ROM",
        globalCfg.GetQString("LastROMFolder"),
        "All supported files (*" + allROMs + ")" + extraFilters
    );

    if (filename.isEmpty())
    {
        emuThread->emuUnpause();
        return {};
    }

    globalCfg.SetQString("LastROMFolder", QFileInfo(filename).dir().path());
    auto ret = splitArchivePath(filename, false);
    emuThread->emuUnpause();
    return ret;
}

void MainWindow::updateCartInserted(bool gba)
{
    bool inserted;
    QString label;
    if (gba)
    {
        inserted = emuInstance->gbaCartInserted() && (emuInstance->getConsoleType() == 0);
        label = "GBA slot: " + emuInstance->gbaCartLabel();

        emuInstance->doOnAllWindows([=](MainWindow* win)
        {
            if (!win->hasMenu) return;
            win->actCurrentGBACart->setText(label);
            win->actEjectGBACart->setEnabled(inserted);
        });
    }
    else
    {
        inserted = emuInstance->cartInserted();
        label = "DS slot: " + emuInstance->cartLabel();

        emuInstance->doOnAllWindows([=](MainWindow* win)
        {
            if (!win->hasMenu) return;
            win->actCurrentCart->setText(label);
            win->actEjectCart->setEnabled(inserted);
            win->actImportSavefile->setEnabled(inserted);
            win->actEnableCheats->setEnabled(inserted);
            win->actSetupCheats->setEnabled(inserted);
            win->actROMInfo->setEnabled(inserted);
            win->actRAMInfo->setEnabled(inserted);
        });
    }
}

void MainWindow::onOpenFile()
{
    if (!verifySetup())
        return;

    QStringList file = pickROM(false);
    if (file.isEmpty())
        return;

    QString errorstr;
    if (!emuThread->bootROM(file, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    QString filename = file.join('|');
    recentFileList.removeAll(filename);
    recentFileList.prepend(filename);
    updateRecentFilesMenu();

    updateCartInserted(false);
}

void MainWindow::onClearRecentFiles()
{
    recentFileList.clear();
    globalCfg.GetArray("RecentROM").Clear();
    updateRecentFilesMenu();
}

void MainWindow::loadRecentFilesMenu(bool loadcfg)
{
    if (loadcfg)
    {
        recentFileList.clear();

        Config::Array recentROMs = globalCfg.GetArray("RecentROM");
        int numrecent = std::min(kMaxRecentROMs, (int) recentROMs.Size());
        for (int i = 0; i < numrecent; ++i)
        {
            std::string item = recentROMs.GetString(i);
            if (!item.empty())
                recentFileList.push_back(QString::fromStdString(item));
        }
    }

    recentMenu->clear();

    for (int i = 0; i < recentFileList.size(); ++i)
    {
        if (i >= kMaxRecentROMs) break;

        QString item_full = recentFileList.at(i);
        QString item_display = item_full;
        int itemlen = item_full.length();
        const int maxlen = 100;
        if (itemlen > maxlen)
        {
            int cut_start = 0;
            while (item_full[cut_start] != '/' && item_full[cut_start] != '\\' &&
                   cut_start < itemlen)
                cut_start++;

            int cut_end = itemlen-1;
            while (((item_full[cut_end] != '/' && item_full[cut_end] != '\\') ||
                    (cut_start+4+(itemlen-cut_end) < maxlen)) &&
                   cut_end > 0)
                cut_end--;

            item_display.truncate(cut_start+1);
            item_display += "...";
            item_display += QString(item_full).remove(0, cut_end);
        }

        QAction *actRecentFile_i = recentMenu->addAction(QString("%1.  %2").arg(i+1).arg(item_display));
        actRecentFile_i->setData(item_full);
        connect(actRecentFile_i, &QAction::triggered, this, &MainWindow::onClickRecentFile);
    }

    while (recentFileList.size() > 10)
        recentFileList.removeLast();

    recentMenu->addSeparator();

    QAction *actClearRecentList = recentMenu->addAction("Clear");
    connect(actClearRecentList, &QAction::triggered, this, &MainWindow::onClearRecentFiles);

    if (recentFileList.empty())
        actClearRecentList->setEnabled(false);
}

void MainWindow::updateRecentFilesMenu()
{
    Config::Array recentroms = globalCfg.GetArray("RecentROM");
    recentroms.Clear();

    for (int i = 0; i < recentFileList.size(); ++i)
    {
        if (i >= kMaxRecentROMs) break;

        recentroms.SetQString(i, recentFileList.at(i));
    }

    Config::Save();
    loadRecentFilesMenu(false);

    emuInstance->broadcastCommand(InstCmd_UpdateRecentFiles);
}

void MainWindow::onClickRecentFile()
{
    QAction *act = (QAction *)sender();
    QString filename = act->data().toString();

    if (!verifySetup())
        return;

    const QStringList file = splitArchivePath(filename, true);
    if (file.isEmpty())
        return;

    QString errorstr;
    if (!emuThread->bootROM(file, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    recentFileList.removeAll(filename);
    recentFileList.prepend(filename);
    updateRecentFilesMenu();

    updateCartInserted(false);
}

void MainWindow::onBootFirmware()
{
    if (!verifySetup())
        return;

    QString errorstr;
    if (!emuThread->bootFirmware(errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }
}

void MainWindow::onInsertCart()
{
    QStringList file = pickROM(false);
    if (file.isEmpty())
        return;

    QString errorstr;
    if (!emuThread->insertCart(file, false, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    updateCartInserted(false);
}

void MainWindow::onEjectCart()
{
    emuThread->ejectCart(false);
    updateCartInserted(false);
}

void MainWindow::onInsertGBACart()
{
    QStringList file = pickROM(true);
    if (file.isEmpty())
        return;

    QString errorstr;
    if (!emuThread->insertCart(file, true, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    updateCartInserted(true);
}

void MainWindow::onInsertGBAAddon()
{
    QAction* act = (QAction*)sender();
    int type = act->data().toInt();

    QString errorstr;
    if (!emuThread->insertGBAAddon(type, errorstr))
    {
        QMessageBox::critical(this, "melonDS", errorstr);
        return;
    }

    updateCartInserted(true);
}

void MainWindow::onEjectGBACart()
{
    emuThread->ejectCart(true);
    updateCartInserted(true);
}

void MainWindow::onSaveState()
{
    int slot = ((QAction*)sender())->data().toInt();

    QString filename;
    if (slot > 0)
    {
        filename = QString::fromStdString(emuInstance->getSavestateName(slot));
    }
    else
    {
        // TODO: specific 'last directory' for savestate files?
        beginSettingsDialog();
        filename = QFileDialog::getSaveFileName(this,
                                                         "Save state",
                                                         globalCfg.GetQString("LastROMFolder"),
                                                         "melonDS savestates (*.mln);;Any file (*.*)");
        endSettingsDialog();
        if (filename.isEmpty())
            return;
    }

    if (emuThread->saveState(filename))
    {
        if (slot > 0) emuInstance->osdAddMessage(0, "State saved to slot %d", slot);
        else          emuInstance->osdAddMessage(0, "State saved to file");

        actLoadState[slot]->setEnabled(true);
    }
    else
    {
        emuInstance->osdAddMessage(0xFFA0A0, "State save failed");
    }
}

void MainWindow::onLoadState()
{
    int slot = ((QAction*)sender())->data().toInt();

    QString filename;
    if (slot > 0)
    {
        filename = QString::fromStdString(emuInstance->getSavestateName(slot));
    }
    else
    {
        // TODO: specific 'last directory' for savestate files?
        beginSettingsDialog();
        filename = QFileDialog::getOpenFileName(this,
                                                         "Load state",
                                                         globalCfg.GetQString("LastROMFolder"),
                                                         "melonDS savestates (*.ml*);;Any file (*.*)");
        endSettingsDialog();
        if (filename.isEmpty())
            return;
    }

    if (!Platform::FileExists(filename.toStdString()))
    {
        if (slot > 0) emuInstance->osdAddMessage(0xFFA0A0, "State slot %d is empty", slot);
        else          emuInstance->osdAddMessage(0xFFA0A0, "State file does not exist");

        return;
    }

    if (emuThread->loadState(filename))
    {
        if (slot > 0) emuInstance->osdAddMessage(0, "State loaded from slot %d", slot);
        else          emuInstance->osdAddMessage(0, "State loaded from file");

        actUndoStateLoad->setEnabled(true);
    }
    else
    {
        emuInstance->osdAddMessage(0xFFA0A0, "State load failed");
    }
}

void MainWindow::onUndoStateLoad()
{
    emuThread->undoStateLoad();

    emuInstance->osdAddMessage(0, "State load undone");
}

void MainWindow::onImportSavefile()
{
    QString path = QFileDialog::getOpenFileName(this,
                                            "Select savefile",
                                            globalCfg.GetQString("LastROMFolder"),
                                            "Savefiles (*.sav *.bin *.dsv);;Any file (*.*)");

    if (path.isEmpty())
        return;

    if (!Platform::FileExists(path.toStdString()))
    {
        QMessageBox::critical(this, "melonDS", "Could not open the given savefile.");
        return;
    }

    if (emuThread->emuIsActive())
    {
        if (QMessageBox::warning(this,
                        "melonDS",
                        "The emulation will be reset and the current savefile overwritten.",
                        QMessageBox::Ok, QMessageBox::Cancel) != QMessageBox::Ok)
        {
            return;
        }
    }

    if (!emuThread->importSavefile(path))
    {
        QMessageBox::critical(this, "melonDS", "Could not import the given savefile.");
        return;
    }
}

void MainWindow::onQuit()
{
    // embedded in the workbench: quit melonDS through the workbench, which saves its layout and takes
    // this window down first (closing only this window would bring back the hidden main window)
    if (embedded && window() != this)
    {
        // later: a key (Ctrl+Q) is still being delivered to this window, which closing deletes
        QWidget* wb = window();
        wb->setProperty("quitAll", true);
        QTimer::singleShot(0, wb, [wb]() { wb->close(); });
        return;
    }
    close();
}


void MainWindow::onPause(bool checked)
{
    if (!emuThread->emuIsActive()) return;

    if (checked)
    {
        emuThread->emuPause();
        pausedManually = true;
    }
    else
    {
        emuThread->emuUnpause();
        pausedManually = false;
    }
}

void MainWindow::onReset()
{
    if (!emuThread->emuIsActive()) return;

    emuThread->emuReset();
}

void MainWindow::onStop()
{
    if (!emuThread->emuIsActive()) return;

    emuThread->emuStop(true);
}

void MainWindow::onFrameStep()
{
    if (!emuThread->emuIsActive()) return;

    emuThread->emuFrameStep();
}

void MainWindow::onOpenDateTime()
{
    DateTimeDialog* dlg = DateTimeDialog::openDlg(this);
    connect(dlg, &DateTimeDialog::finished, this, &MainWindow::onDateTimeDialogFinished);
}

void MainWindow::onDateTimeDialogFinished(int res)
{
    if (!res) return;
    if (!emuThread->emuIsActive()) return;

    emuInstance->setDateTime();
}

void MainWindow::onOpenPowerManagement()
{
    PowerManagementDialog* dlg = PowerManagementDialog::openDlg(this);
}

void MainWindow::onEnableCheats(bool checked)
{
    localCfg.SetBool("EnableCheats", checked);
    emuThread->enableCheats(checked);

    emuInstance->doOnAllWindows([=](MainWindow* win)
    {
        win->actEnableCheats->setChecked(checked);
    }, windowID);
}

void MainWindow::onSetupCheats()
{
    beginSettingsDialog();

    CheatsDialog* dlg = CheatsDialog::openDlg(this);
    connect(dlg, &CheatsDialog::finished, this, &MainWindow::onCheatsDialogFinished);
}

void MainWindow::onCheatsDialogFinished(int res)
{
    endSettingsDialog();
}

void MainWindow::onROMInfo()
{
    auto cart = emuInstance->nds->NDSCartSlot.GetCart();
    if (cart)
        ROMInfoDialog* dlg = ROMInfoDialog::openDlg(this);
}

void MainWindow::onRAMInfo()
{
    RAMInfoDialog* dlg = RAMInfoDialog::openDlg(this);
}

void MainWindow::onOpenTitleManager()
{
    TitleManagerDialog* dlg = TitleManagerDialog::openDlg(this);
}

void MainWindow::onMPNewInstance()
{
    createEmuInstance();
}

void MainWindow::onLANStartHost()
{
    if (!lanWarning(true)) return;
    LANStartHostDialog::openDlg(this);
}

void MainWindow::onLANStartClient()
{
    if (!lanWarning(false)) return;
    LANStartClientDialog::openDlg(this);
}

void MainWindow::onNPStartHost()
{
    //Netplay::StartHost();
    //NetplayStartHostDialog::openDlg(this);
}

void MainWindow::onNPStartClient()
{
    //Netplay::StartClient();
    //NetplayStartClientDialog::openDlg(this);
}

void MainWindow::onNPTest()
{
    // HAX
    //Netplay::StartGame();
}

void MainWindow::updateMPInterface(MPInterfaceType type)
{
    if (!hasMenu) return;

    // MP interface was changed, reflect it in the UI

    bool enable = (type == MPInterface_Local);
    actMPNewInstance->setEnabled(enable);
    actLANStartHost->setEnabled(enable);
    actLANStartClient->setEnabled(enable);
    /*actNPStartHost->setEnabled(enable);
    actNPStartClient->setEnabled(enable);
    actNPTest->setEnabled(enable);*/
}

bool MainWindow::lanWarning(bool host)
{
    if (numEmuInstances() < 2)
        return true;

    QString verb = host ? "host" : "join";
    QString msg = "Multiple emulator instances are currently open.\n"
            "If you "+verb+" a LAN game now, all secondary instances will be closed.\n\n"
            "Do you wish to continue?";

    auto res = QMessageBox::warning(this, "melonDS", msg, QMessageBox::Yes|QMessageBox::No, QMessageBox::No);
    if (res == QMessageBox::No)
        return false;

    deleteAllEmuInstances(1);
    return true;
}

void MainWindow::beginSettingsDialog()
{
    if (emuThread->emuIsRunning())
    {
        emuThread->emuPause();
        settingsPauseDepth++;
    }
}

void MainWindow::endSettingsDialog()
{
    if (settingsPauseDepth > 0)
    {
        settingsPauseDepth--;
        emuThread->emuUnpause();
    }
}

void MainWindow::onOpenEmuSettings()
{
    beginSettingsDialog();

    EmuSettingsDialog* dlg = EmuSettingsDialog::openDlg(this);
    connect(dlg, &EmuSettingsDialog::finished, this, &MainWindow::onEmuSettingsDialogFinished);
}

void MainWindow::onEmuSettingsDialogFinished(int res)
{
    if (globalCfg.GetInt("Emu.ConsoleType") == 1)
    {
        actInsertGBACart->setEnabled(false);
        for (auto act : actInsertGBAAddon)
            act->setEnabled(false);
        actEjectGBACart->setEnabled(false);
    }
    else
    {
        actInsertGBACart->setEnabled(true);
        for (auto act : actInsertGBAAddon)
            act->setEnabled(true);
        actEjectGBACart->setEnabled(emuInstance->gbaCartInserted());
    }

    if (EmuSettingsDialog::needsReset)
        onReset();

    actCurrentGBACart->setText("GBA slot: " + emuInstance->gbaCartLabel());

    if (!emuThread->emuIsActive())
        actTitleManager->setEnabled(!globalCfg.GetString("DSi.NANDPath").empty());

    endSettingsDialog();
}

void MainWindow::onOpenInputConfig()
{
    beginSettingsDialog();

    InputConfigDialog* dlg = InputConfigDialog::openDlg(this);
    connect(dlg, &InputConfigDialog::finished, this, &MainWindow::onInputConfigFinished);
}

void MainWindow::onInputConfigFinished(int res)
{
    endSettingsDialog();
}

void MainWindow::onOpenVideoSettings()
{
    VideoSettingsDialog* dlg = VideoSettingsDialog::openDlg(this);
    connect(dlg, &VideoSettingsDialog::updateVideoSettings, this, &MainWindow::onUpdateVideoSettings);
}

void MainWindow::onOpenCameraSettings()
{
    beginSettingsDialog();

    camStarted[0] = camManager[0]->isStarted();
    camStarted[1] = camManager[1]->isStarted();
    if (camStarted[0]) camManager[0]->stop();
    if (camStarted[1]) camManager[1]->stop();

    CameraSettingsDialog* dlg = CameraSettingsDialog::openDlg(this);
    connect(dlg, &CameraSettingsDialog::finished, this, &MainWindow::onCameraSettingsFinished);
}

void MainWindow::onCameraSettingsFinished(int res)
{
    if (camStarted[0]) camManager[0]->start();
    if (camStarted[1]) camManager[1]->start();

    endSettingsDialog();
}

void MainWindow::onOpenAudioSettings()
{
    AudioSettingsDialog* dlg = AudioSettingsDialog::openDlg(this);
    connect(emuThread, &EmuThread::syncVolumeLevel, dlg, &AudioSettingsDialog::onSyncVolumeLevel);
    connect(emuThread, &EmuThread::windowEmuStart, dlg, &AudioSettingsDialog::onConsoleReset);
    connect(dlg, &AudioSettingsDialog::updateAudioVolume, this, &MainWindow::onUpdateAudioVolume);
    connect(dlg, &AudioSettingsDialog::updateAudioSettings, this, &MainWindow::onUpdateAudioSettings);
    connect(dlg, &AudioSettingsDialog::finished, this, &MainWindow::onAudioSettingsFinished);
}

void MainWindow::onOpenFirmwareSettings()
{
    beginSettingsDialog();

    FirmwareSettingsDialog* dlg = FirmwareSettingsDialog::openDlg(this);
    connect(dlg, &FirmwareSettingsDialog::finished, this, &MainWindow::onFirmwareSettingsFinished);
}

void MainWindow::onFirmwareSettingsFinished(int res)
{
    if (FirmwareSettingsDialog::needsReset)
        onReset();

    endSettingsDialog();
}

void MainWindow::onOpenPathSettings()
{
    beginSettingsDialog();

    PathSettingsDialog* dlg = PathSettingsDialog::openDlg(this);
    connect(dlg, &PathSettingsDialog::finished, this, &MainWindow::onPathSettingsFinished);
}

void MainWindow::onPathSettingsFinished(int res)
{
    if (PathSettingsDialog::needsReset)
        onReset();

    endSettingsDialog();
}

void MainWindow::onUpdateAudioVolume(int vol, int dsisync)
{
    emuInstance->audioVolume = vol;
    emuInstance->audioDSiVolumeSync = dsisync;
}

void MainWindow::onUpdateAudioSettings()
{
    if (!emuThread->emuIsActive()) return;
    assert(emuInstance->nds != nullptr);

    int interp = globalCfg.GetInt("Audio.Interpolation");
    emuInstance->nds->SPU.SetInterpolation(static_cast<AudioInterpolation>(interp));

    int bitdepth = globalCfg.GetInt("Audio.BitDepth");
    if (bitdepth == 0)
        emuInstance->nds->SPU.SetDegrade10Bit(emuInstance->nds->ConsoleType == 0);
    else
        emuInstance->nds->SPU.SetDegrade10Bit(bitdepth == 1);
}

void MainWindow::onAudioSettingsFinished(int res)
{
    emuInstance->audioUpdateSettings();
}

void MainWindow::onOpenMPSettings()
{
    beginSettingsDialog();

    MPSettingsDialog* dlg = MPSettingsDialog::openDlg(this);
    connect(dlg, &MPSettingsDialog::finished, this, &MainWindow::onMPSettingsFinished);
}

void MainWindow::onMPSettingsFinished(int res)
{
    emuInstance->mpAudioMode = globalCfg.GetInt("MP.AudioMode");
    emuInstance->updateAudioMuteByWindowFocus();
    MPInterface::Get().SetRecvTimeout(globalCfg.GetInt("MP.RecvTimeout"));

    endSettingsDialog();
}

void MainWindow::onOpenWifiSettings()
{
    beginSettingsDialog();

    WifiSettingsDialog* dlg = WifiSettingsDialog::openDlg(this);
    connect(dlg, &WifiSettingsDialog::finished, this, &MainWindow::onWifiSettingsFinished);
}

void MainWindow::onWifiSettingsFinished(int res)
{
    if (WifiSettingsDialog::needsReset)
        onReset();

    endSettingsDialog();
}

void MainWindow::onOpenInterfaceSettings()
{
    beginSettingsDialog();
    InterfaceSettingsDialog* dlg = InterfaceSettingsDialog::openDlg(this);
    connect(dlg, &InterfaceSettingsDialog::finished, this, &MainWindow::onInterfaceSettingsFinished);
    connect(dlg, &InterfaceSettingsDialog::updateInterfaceSettings, this, &MainWindow::onUpdateInterfaceSettings);
}

void MainWindow::onUpdateInterfaceSettings()
{
    pauseOnLostFocus = globalCfg.GetBool("PauseLostFocus");
    emuInstance->targetFPS = globalCfg.GetDouble("TargetFPS");
    emuInstance->fastForwardFPS = globalCfg.GetDouble("FastForwardFPS");
    emuInstance->slowmoFPS = globalCfg.GetDouble("SlowmoFPS");
    panel->setMouseHide(globalCfg.GetBool("Mouse.Hide"),
                        globalCfg.GetInt("Mouse.HideSeconds")*1000);
}

void MainWindow::onInterfaceSettingsFinished(int res)
{
    endSettingsDialog();
}

void MainWindow::onChangeScreenSize()
{
    int factor = ((QAction*)sender())->data().toInt();
    QSize diff = size() - panel->size();
    // embedded in the workbench, the workbench panel sets the window's size: cap the screen at the
    // zoom instead, centred under the menus (a smaller scale still when there is no room)
    if (embedded) panel->setMaximumSize(panel->screenGetMinSize(factor));
    else resize(panel->screenGetMinSize(factor) + diff);
}

void MainWindow::onChangeScreenRotation(QAction* act)
{
    int rot = act->data().toInt();
    windowCfg.SetInt("ScreenRotation", rot);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenGap(QAction* act)
{
    int gap = act->data().toInt();
    windowCfg.SetInt("ScreenGap", gap);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenLayout(QAction* act)
{
    int layout = act->data().toInt();
    windowCfg.SetInt("ScreenLayout", layout);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenSwap(bool checked)
{
    windowCfg.SetBool("ScreenSwap", checked);

    // Swap between top and bottom screen when displaying one screen.
    int sizing = windowCfg.GetInt("ScreenSizing");
    if (sizing == screenSizing_TopOnly)
    {
        // Bottom Screen.
        sizing = screenSizing_BotOnly;
        actScreenSizing[screenSizing_TopOnly]->setChecked(false);
        actScreenSizing[sizing]->setChecked(true);
    }
    else if (sizing == screenSizing_BotOnly)
    {
        // Top Screen.
        sizing = screenSizing_TopOnly;
        actScreenSizing[screenSizing_BotOnly]->setChecked(false);
        actScreenSizing[sizing]->setChecked(true);
    }
    windowCfg.SetInt("ScreenSizing", sizing);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenSizing(QAction* act)
{
    int sizing = act->data().toInt();
    windowCfg.SetInt("ScreenSizing", sizing);

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenAspect(QAction* act)
{
    int aspect = act->data().toInt();
    QActionGroup* group = act->actionGroup();

    if (group == grpScreenAspectTop)
    {
        windowCfg.SetInt("ScreenAspectTop", aspect);
    }
    else
    {
        windowCfg.SetInt("ScreenAspectBot", aspect);
    }

    emit screenLayoutChange();
}

void MainWindow::onChangeIntegerScaling(bool checked)
{
    windowCfg.SetBool("IntegerScaling", checked);

    emit screenLayoutChange();
}

void MainWindow::onOpenNewWindow()
{
    emuInstance->createWindow();
}

void MainWindow::updateScreenFilterMenu()
{
    const int mode = screenFilterSetting(emuInstance->getLocalConfig());
    if (lcdWindow) lcdWindow->setActive(mode == screenFilter_LCD);
    if (crtWindow) crtWindow->setActive(mode == screenFilter_CRT);
    const int threads = softFilterThreads(emuInstance->getLocalConfig());
    if (lcdWindow) lcdWindow->setDrawnBy(hasOGL, threads);
    if (crtWindow) crtWindow->setDrawnBy(hasOGL, threads);
    if (!actScreenFilter[mode]) return;
    actScreenFilter[mode]->setChecked(true);
    const char* by = hasOGL ? "Drawn by an OpenGL shader (Config > Video settings uses OpenGL)"
                            : "Drawn in software on the CPU (Config > Video settings uses software)";
    actScreenFilter[screenFilter_Off]->setToolTip("The plain picture");
    actScreenFilter[screenFilter_LCD]->setToolTip(QString("Imitates the DS's own LCD screens.\n") + by);
    actScreenFilter[screenFilter_CRT]->setToolTip(QString("Imitates an old CRT television.\n") + by);
}

void MainWindow::onChangeScreenFilter(int mode)
{
    // one setting for all windows of the instance; each screen keeps its implementation
    mode = std::clamp(mode, 0, (int)screenFilter_MAX - 1);
    Config::Table& local = emuInstance->getLocalConfig();
    // the filter Alt+F brings back: the one being turned off, or the one being turned on
    int cur = screenFilterSetting(local);
    if (cur != screenFilter_Off) local.SetInt("ScreenFilterLast", cur);
    if (mode != screenFilter_Off) local.SetInt("ScreenFilterLast", mode);
    local.SetInt("ScreenFilterMode", mode);
    Config::Save();
    for (int i = 0; i < kMaxWindows; i++)
        if (auto* w = emuInstance->getWindow(i))
        {
            if (w->panel) w->panel->setScreenFilter(mode);
            w->updateScreenFilterMenu();
        }
}

void MainWindow::onToggleScreenFilter()
{
    Config::Table& local = emuInstance->getLocalConfig();
    int last = local.GetInt("ScreenFilterLast");
    if (last <= screenFilter_Off || last >= screenFilter_MAX) last = screenFilter_LCD;
    onChangeScreenFilter(screenFilterSetting(local) == screenFilter_Off ? last : (int)screenFilter_Off);
}

void MainWindow::onChangeScreenFiltering(bool checked)
{
    windowCfg.SetBool("ScreenFilter", checked);

    //emit screenLayoutChange();
    panel->setFilter(checked);
}

void MainWindow::onChangeShowOSD(bool checked)
{
    showOSD = checked;
    panel->osdSetEnabled(showOSD);
    windowCfg.SetBool("ShowOSD", showOSD);
}

void MainWindow::onChangeLimitFramerate(bool checked)
{
    emuInstance->doLimitFPS = checked;
    globalCfg.SetBool("LimitFPS", emuInstance->doLimitFPS);
}

void MainWindow::onChangeAudioSync(bool checked)
{
    emuInstance->doAudioSync = checked;
    globalCfg.SetBool("AudioSync", emuInstance->doAudioSync);
}


void MainWindow::onTitleUpdate(QString title)
{
    if (!emuInstance) return;

    int numinst = numEmuInstances();
    int numwin = emuInstance->getNumWindows();
    if ((numinst > 1) && (numwin > 1))
    {
        // add player/window prefix
        QString prefix = QString("[p%1:w%2] ").arg(emuInstance->instanceID+1).arg(windowID+1);
        title = prefix + title;
    }
    else if (numinst > 1)
    {
        // add player prefix
        QString prefix = QString("[p%1] ").arg(emuInstance->instanceID+1);
        title = prefix + title;
    }
    else if (numwin > 1)
    {
        // add window prefix
        QString prefix = QString("[w%1] ").arg(windowID+1);
        title = prefix + title;
    }

    if (WBHost* wb = emuInstance->getWorkbench())
    {
        QString p = wb->TitlePrefix();
        if (!p.isEmpty() && !title.startsWith(p)) title = p + title;
    }

    setWindowTitle(title);
}

void MainWindow::onWorkbenchBooted()
{
    updateCartInserted(false);
}

void MainWindow::toggleFullscreen()
{
    if (!isFullScreen())
    {
        showFullScreen();
        if (hasMenu)
        {
            menuBar()->setFixedHeight(0); // Don't use hide() as menubar actions stop working
            if (toolbar) toolbar->hide();
        }
    }
    else
    {
        showNormal();
        if (hasMenu)
        {
            int menuBarHeight = menuBar()->sizeHint().height();
            menuBar()->setFixedHeight(menuBarHeight);
            if (toolbar) toolbar->show();
        }
    }
}

void MainWindow::onFullscreenToggled()
{
    toggleFullscreen();
}

void MainWindow::onScreenEmphasisToggled()
{
    int currentSizing = windowCfg.GetInt("ScreenSizing");
    if (currentSizing == screenSizing_EmphTop)
    {
        currentSizing = screenSizing_EmphBot;
    }
    else if (currentSizing == screenSizing_EmphBot)
    {
        currentSizing = screenSizing_EmphTop;
    }
    else
    {
        // For any other sizing mode, switch to EmphTop as a sensible default
        currentSizing = screenSizing_EmphTop;
    }
    windowCfg.SetInt("ScreenSizing", currentSizing);
    actScreenSizing[currentSizing]->setChecked(true);

    emit screenLayoutChange();
}

void MainWindow::onEmuStart()
{
    if (!hasMenu) return;

    for (int i = 1; i < 9; i++)
    {
        actSaveState[i]->setEnabled(true);
        actLoadState[i]->setEnabled(emuInstance->savestateExists(i));
    }
    actSaveState[0]->setEnabled(true);
    actLoadState[0]->setEnabled(true);
    actUndoStateLoad->setEnabled(false);

    actPause->setEnabled(true);
    actPause->setChecked(false);
    actReset->setEnabled(true);
    actStop->setEnabled(true);
    actFrameStep->setEnabled(true);

    //actDateTime->setEnabled(false);
    actPowerManagement->setEnabled(true);

    actTitleManager->setEnabled(false);
}

void MainWindow::onEmuStop()
{
    if (!hasMenu) return;

    for (int i = 0; i < 9; i++)
    {
        actSaveState[i]->setEnabled(false);
        actLoadState[i]->setEnabled(false);
    }
    actUndoStateLoad->setEnabled(false);

    actPause->setEnabled(false);
    actReset->setEnabled(false);
    actStop->setEnabled(false);
    actFrameStep->setEnabled(false);

    //actDateTime->setEnabled(true);
    actPowerManagement->setEnabled(false);

    actTitleManager->setEnabled(!globalCfg.GetString("DSi.NANDPath").empty());
}

void MainWindow::onEmuPause(bool pause)
{
    if (!hasMenu) return;

    actPause->setChecked(pause);
}

void MainWindow::onEmuReset()
{
    if (!hasMenu) return;

    actUndoStateLoad->setEnabled(false);
}

void MainWindow::onUpdateVideoSettings(bool glchange)
{
    if (!emuInstance) return;

    // if we have a parent window: pass the message over to the parent
    // the topmost parent takes care of updating all the windows
    MainWindow* parentwin = (MainWindow*)parentWidget();
    if (parentwin)
        return parentwin->onUpdateVideoSettings(glchange);

    auto childwins = findChildren<MainWindow *>(nullptr);

    bool hadOGL = hasOGL;
    if (glchange)
    {
        beginSettingsDialog();
        if (hadOGL)
        {
            emuThread->deinitContext(windowID);
            for (auto child: childwins)
            {
                auto thread = child->getEmuInstance()->getEmuThread();
                thread->deinitContext(child->windowID);
            }
        }

        createScreenPanel();
        for (auto child: childwins)
        {
            child->createScreenPanel();
        }
    }

    emuThread->updateVideoSettings();
    for (auto child: childwins)
    {
        // child windows may belong to a different instance
        // in that case we need to signal their thread appropriately
        auto thread = child->getEmuInstance()->getEmuThread();
        if (child->getWindowID() == 0)
            thread->updateVideoSettings();
    }

    if (glchange)
    {
        if (hasOGL) 
        {
            emuThread->initContext(windowID);
            for (auto child: childwins)
            {
                auto thread = child->getEmuInstance()->getEmuThread();
                thread->initContext(child->windowID);
            }
        }
    }

    if (glchange)
    {
        endSettingsDialog();
    }
}
