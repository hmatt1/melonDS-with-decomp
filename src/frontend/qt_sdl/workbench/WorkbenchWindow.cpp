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

#include <memory>
#include <QAbstractButton>
#include <QChildEvent>
#include <QCloseEvent>
#include <QFile>
#include <QPixmap>
#include <QPointer>
#include <QFileSystemWatcher>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QUrl>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSplitter>
#include <QToolButton>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QFormLayout>
#include <DockManager.h>
#include <DockWidget.h>
#include <DockAreaWidget.h>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QToolBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabBar>

#include "WorkbenchWindow.h"
#include "Panels.h"
#include "Dialogs.h"
#include "WBColors.h"
#include "WBHost.h"
#include "WBQueries.h"
#include "ApiServer.h"
#include "Config.h"
#include "EmuInstance.h"
#include "Window.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QClipboard>
#include <QKeyEvent>

using namespace melonDS;
using namespace melonDS::WB;

WBPanel::WBPanel(WorkbenchWindow* win, QWidget* parent) : QWidget(parent), W(win), H(win->Host())
{
}

bool WBPanel::IsShown() const
{
    return isVisible() && !visibleRegion().isEmpty();
}

void WorkbenchWindow::ApplyDarkTheme()
{
    QPalette pal;
    const QColor window = WBColor::Window();
    const QColor base = WBColor::Base();
    const QColor text = WBColor::Text();
    const QColor button = WBColor::Off();
    const QColor hi = WBColor::Select();
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, base);
    pal.setColor(QPalette::AlternateBase, button);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, button);
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::BrightText, text);
    pal.setColor(QPalette::Highlight, hi);
    pal.setColor(QPalette::HighlightedText, text);
    pal.setColor(QPalette::ToolTipBase, button);
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::Mid, WBColor::Border());
    pal.setColor(QPalette::Dark, WBColor::Dark());
    pal.setColor(QPalette::Shadow, WBColor::Dark());
    pal.setColor(QPalette::Light, WBColor::Border());
    pal.setColor(QPalette::Link, WBColor::Blue());
    pal.setColor(QPalette::LinkVisited, WBColor::Teal());
    pal.setColor(QPalette::Accent, WBColor::Teal());
    pal.setColor(QPalette::PlaceholderText, WBColor::Muted());
    pal.setColor(QPalette::Disabled, QPalette::WindowText, WBColor::Muted());
    pal.setColor(QPalette::Disabled, QPalette::Text, WBColor::Muted());
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, WBColor::Muted());
    setPalette(pal);
    for (QAbstractButton* b : findChildren<QAbstractButton*>())
        StampButton(b);
}

void WorkbenchWindow::StampButton(QAbstractButton* b)
{
    if (!b) return;
    if (GameWin && GameWin->isAncestorOf(b)) return;
    if (b->window() != this && !isAncestorOf(b)) return;
    // The fill is the Dracula button color. The label is the light foreground, including on a gray face.
    QPalette p = palette();
    p.setColor(QPalette::Button, WBColor::Select());
    p.setColor(QPalette::ButtonText, WBColor::Text());
    p.setColor(QPalette::WindowText, WBColor::Text());
    p.setColor(QPalette::Disabled, QPalette::Button, WBColor::Off());
    p.setColor(QPalette::Disabled, QPalette::ButtonText, WBColor::Muted());
    p.setColor(QPalette::Disabled, QPalette::WindowText, WBColor::Muted());
    b->setPalette(p);
}

void WorkbenchWindow::ApplyChrome()
{
    const QString sheet = WBColor::WidgetStyleSheet();
    setStyleSheet(sheet);
    if (!DockMgr) return;
    // ADS installs its own sheet once; ours is appended so a reload does not stack copies
    static QString adsBase;
    static bool saved = false;
    if (!saved)
    {
        adsBase = DockMgr->styleSheet();
        saved = true;
    }
    DockMgr->setStyleSheet(adsBase + "\n" + sheet);
}

void WorkbenchWindow::ReloadColors()
{
    if (!WBColor::Load()) return;
    ApplyDarkTheme();
    ApplyChrome();
    for (QWidget* w : findChildren<QWidget*>()) w->update();
}

WorkbenchWindow::WorkbenchWindow(WBHost* host) : QMainWindow(nullptr), H(host)
{
    WBColor::Load();
    ApplyDarkTheme();
    setWindowTitle("melonDS Decomp Workbench");
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1500, 950);

    // every panel, the disassembly included, can dock, tab, float, auto-hide to a side bar or close
    ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    // full tab titles; a crowded tab bar scrolls, and its menu button lists every tab
    ads::CDockManager::setConfigFlag(ads::CDockManager::DisableTabTextEliding, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DoubleClickUndocksWidget, false);
    // plain XML, so a restored layout can be checked for panels it doesn't know (RestoreLayout)
    ads::CDockManager::setConfigFlag(ads::CDockManager::XmlCompressionEnabled, false);
    ads::CDockManager::setAutoHideConfigFlags(ads::CDockManager::DefaultAutoHideConfig);
    ads::CDockManager::setAutoHideConfigFlag(ads::CDockManager::AutoHideCloseOnOutsideMouseClick, true);
    DockMgr = new ads::CDockManager(this);
    ApplyChrome();
    {
        auto* watch = new QFileSystemWatcher(this);
        connect(watch, &QFileSystemWatcher::fileChanged, this, [this, watch]()
        {
            ReloadColors();
            const QString path = WBColor::FilePath();
            if (!watch->files().contains(path) && QFile::exists(path)) watch->addPath(path);
        });
        if (QFile::exists(WBColor::FilePath())) watch->addPath(WBColor::FilePath());
    }

    BuildPanels();
    DescribePanels();
    BuildMenus();
    BuildStatusBar();
    DefaultLayout();

    Config::Table cfg = Config::GetGlobalTable();
    QByteArray geom = QByteArray::fromBase64(cfg.GetQString("Workbench.WindowGeometry").toLatin1());
    if (!geom.isEmpty()) restoreGeometry(geom);
    // workspaces are set up when the window is first shown (FinishLayout): only then are the default
    // layout's splitter sizes real

    connect(H, &WBHost::statusChanged, this, [this]()
    {
        UpdateStatus();
        // a cancelled or abandoned run leaves the previous summary
        if (AnalysisLive && !H->AnalysisRunning())
        {
            AnalysisLive = false;
            ShowAnalysisLine(AnalysisSummary, true, false);
        }
    });
    connect(H, &WBHost::breakChanged, this, [this](bool inBreak)
    {
        UpdateStatus();
        if (inBreak)
        {
            WBStatus s = H->Status();
            if (s.BreakInfo.Cpu != (u32)CurCpu) SetCpu((int)s.BreakInfo.Cpu);
            if (LockPC) GoTo(s.BreakInfo.PC, false);
        }
        for (WBPanel* p : Panels) p->Refresh();
    });
    connect(H, &WBHost::knowledgeChanged, this, [this]()
    {
        for (WBPanel* p : Panels) p->OnKnowledgeChanged();
        UpdateStatus();
    });
    connect(H, &WBHost::recordingChanged, this, [this]() { UpdateStatus(); UpdateBanners(); for (WBPanel* p : Panels) p->Refresh(); });
    // state changes the user didn't make directly (GD-3)
    connect(H, &WBHost::notice, this, [this](const QString& text) { Notice(text, 8000); });
    connect(H, &WBHost::romChanged, this, [this]()
    {
        H->ClearStaticAnalysis();
        ClearAnalysisHold();
        GoTo(H->Core().Rom.ARM9Entry ? H->Core().Rom.ARM9Entry : 0x02000000, false);
        H->IdentifyFunctions();
    });
    connect(H, &WBHost::pausedChanged, this, [this](bool paused)
    {
        UpdateStatus();
        // a finished snapshot stays. A walk still reading memory is dropped.
        if (!paused) H->AbandonStaticAnalysis();
        for (WBPanel* p : Panels) p->Refresh();
    });
    connect(H, &WBHost::analysisChanged, this, [this]()
    {
        UpdateStatus();
        for (WBPanel* p : Panels) p->Refresh();
    });
    connect(H, &WBHost::analysisProgress, this, [this](int cpu, int n)
    {
        AnalysisLive = true;
        ShowAnalysisLine(QString("Marking unreached code on %1: %2 instruction%3 marked")
                             .arg(cpu ? "ARM7" : "ARM9").arg(n).arg(n == 1 ? "" : "s"),
                         false, true);
    });
    connect(H, &WBHost::analysisFinished, this, [this](const QString& text, int noteCpu, quint32 noteAddr, quint32 first0, quint32 first1)
    {
        AnalysisLive = false;
        AnalysisNoteCpu = noteCpu;
        AnalysisNoteAddr = noteAddr;
        QString line = text;
        if (DisasmView && DisasmView->ConfirmedOnly())
            line += " The disassembly is set to confirmed code only. The coverage map still shows the marks.";
        AnalysisSummary = line;
        ShowAnalysisLine(line, true, true);
        FollowAnalysisMarks(CurCpu ? first1 : first0);
    });
    connect(H, &WBHost::breakpointsChanged, this, [this]()
    {
        RefreshBreakpointCache();
        for (WBPanel* p : Panels)
            if (p->IsShown()) p->Refresh();
    });

    if (qEnvironmentVariableIsSet("DSWB_SHOW_PANEL"))
    {
        QString name = qEnvironmentVariable("DSWB_SHOW_PANEL");
        QTimer::singleShot(500, this, [this, name]() { ShowPanel(name); });
    }
    if (qEnvironmentVariableIsSet("DSWB_GOTO"))
    {
        // "addr[,size]" - start with the cursor and selection there (used by screenshot tests)
        QStringList p = qEnvironmentVariable("DSWB_GOTO").split(',');
        u32 a = p[0].toUInt(nullptr, 16), n = p.size() > 1 ? p[1].toUInt(nullptr, 16) : 1;
        QTimer::singleShot(6000, this, [this, a, n]() { SetLockToPC(false); GoTo(a, false); SetSelection(a, n); });
    }
    if (qEnvironmentVariableIsSet("DSWB_LOCATE"))
    {
        // "path in the game archive" - locate it once the archive is indexed (used by screenshot tests)
        QString path = qEnvironmentVariable("DSWB_LOCATE");
        QTimer::singleShot(7000, this, [this, path]()
        {
            SetLockToPC(false);
            // a file inside a container is given as /file.narc:name
            if (path.contains(':') && ContainersView) LocateContained(ContainersView->IndexOf(path), true);
            else if (ArchiveView) LocateArchived(ArchiveView->IndexOf(path), true);
        });
    }
    if (qEnvironmentVariableIsSet("DSWB_GRAPHICS"))
    {
        // "path[;x,y]" - open a graphics file in the Graphics panel, then click a pixel (used by UI tests)
        QStringList p = qEnvironmentVariable("DSWB_GRAPHICS").split(';');
        QString path = p[0];
        QStringList xy = p.size() > 1 ? p[1].split(',') : QStringList();
        QTimer::singleShot(7000, this, [this, path, xy]()
        {
            SetLockToPC(false);
            ShowPanel("dock_graphics");
            GraphicsPanel* g = FindPanel<GraphicsPanel>();
            if (!g) return;
            g->ShowFile(path);
            if (xy.size() == 2)
                QTimer::singleShot(1500, g, [g, xy]() { g->ClickPixel(QPoint(xy[0].toInt(), xy[1].toInt())); });
        });
    }

    TickTimer.setInterval(100);
    qApp->installEventFilter(this);
    ApplyDarkTheme();
    connect(&TickTimer, &QTimer::timeout, this, &WorkbenchWindow::Tick);
    TickTimer.start();
    UpdateStatus();
}

WorkbenchWindow::~WorkbenchWindow()
{
    SetEmbeddedScreen(false);
}

WBKB::KnowledgeStore& WorkbenchWindow::KB() { return H->KB(); }

void WorkbenchWindow::showEvent(QShowEvent* e)
{
    QMainWindow::showEvent(e);
    ReloadColors();
    if (Workspaces.empty()) QTimer::singleShot(0, this, [this]() { FinishLayout(); });
    QTimer::singleShot(0, this, [this]() { H->IdentifyFunctions(); });
}

void WorkbenchWindow::FinishLayout()
{
    if (!Workspaces.empty()) return;
    DefaultState = DockMgr->saveState(LayoutVersion);
    // the current workspace's layout; one that can't be restored falls back to the workspace's default
    InitWorkspaces();
    RebuildWorkspaceMenu();
    SavedState = DockMgr->saveState(LayoutVersion);
    // the game screen mode the user chose last time
    Config::Table cfg = Config::GetGlobalTable();
    if (cfg.GetBool("Workbench.EmbeddedScreen"))
    {
        ActEmbed->setChecked(true);
        ActHideMain->setChecked(cfg.GetBool("Workbench.HideMainWindow"));
        // not before the emulator has run a few frames: the game window's OpenGL context is made on the
        // emulation thread, which sets up its own at startup
        auto* wait = new QTimer(this);
        qint64 since = QDateTime::currentMSecsSinceEpoch();
        connect(wait, &QTimer::timeout, this, [this, wait, since]()
        {
            WBStatus st = H->Status();
            bool ready = st.EmuState != WBStatus::NoGame && st.Frame >= 30;
            if (!ready && QDateTime::currentMSecsSinceEpoch() - since < 10000) return;
            wait->deleteLater();
            if (ActEmbed->isChecked()) SetEmbeddedScreen(true);
        });
        wait->start(250);
    }
    // the first time (and until it is turned off), say what the switches do
    // (shown without taking the keyboard focus)
    if (!Config::GetGlobalTable().GetBool("Workbench.GettingStartedDismissed"))
        if (ads::CDockWidget* d = DockMgr->findDockWidget("dock_start")) { d->toggleView(true); d->raise(); }
    if (qEnvironmentVariableIsSet("DSWB_WORKSPACE"))
    {
        // start in a workspace by name (used by screenshot tests)
        QString name = qEnvironmentVariable("DSWB_WORKSPACE");
        for (size_t i = 0; i < Workspaces.size(); i++)
            if (Workspaces[i].Name == name) SwitchWorkspace((int)i);
    }
}

void WorkbenchWindow::closeEvent(QCloseEvent* e)
{
    // the game goes back to its own window, or it would vanish with this one
    if (GameWin)
        if (MainWindow* mw = H->Instance()->getMainWindow()) mw->show();
    SaveLayout();
    e->accept();
}

void WorkbenchWindow::SaveLayout()
{
    if (Workspaces.empty()) return;
    QByteArray state = DockMgr->saveState(LayoutVersion);
    QByteArray geom = saveGeometry();
    Config::Table cfg = Config::GetGlobalTable();
    bool sameWs = cfg.GetQString("Workbench.Workspace") == Workspaces[CurWorkspace].Name;
    Workspace& ws = Workspaces[CurWorkspace];
    if (state == SavedState && sameWs && ws.Geometry == geom && ws.Screen == ScreenMode()
        && QByteArray::fromBase64(cfg.GetQString("Workbench.WindowGeometry").toLatin1()) == geom) return;
    SavedState = state;
    CaptureWorkspace(ws);
    cfg.SetQString("Workbench.WindowGeometry", QString::fromLatin1(geom.toBase64()));
    StoreWorkspaces();
    Config::Save();
}

ads::CDockWidget* WorkbenchWindow::AddDock(const QString& title, const QString& name, QWidget* w)
{
    ads::CDockWidget* d = new ads::CDockWidget(DockMgr, title);
    d->setObjectName(name);
    QWidget* content = w;
    if (WBPanel* p = qobject_cast<WBPanel*>(w))
    {
        // a banner over the panel says what it is missing (PR-2)
        content = new QWidget();
        auto* v = new QVBoxLayout(content);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(2);
        NeedBanner* banner = new NeedBanner(H, content);
        v->addWidget(banner);
        v->addWidget(w, 1);
        Banners.push_back({p, banner});
    }
    d->setWidget(content, ads::CDockWidget::ForceNoScrollArea);
    // a panel never collapses to nothing and gets lost
    d->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidgetMinimumSize);
    d->setMinimumSize(160, 90);
    Docks.push_back(d);
    if (WBPanel* p = qobject_cast<WBPanel*>(w))
    {
        Panels.push_back(p);
        // panels refresh on a tick only while the game runs: one brought to the front while paused
        // shows what is there now, not what it last saw
        connect(d, &ads::CDockWidget::visibilityChanged, p, [p](bool visible)
        {
            if (visible) QTimer::singleShot(0, p, [p]() { if (p->IsShown()) p->Refresh(); });
        });
    }
    return d;
}

void WorkbenchWindow::Notice(const QString& text, int ms)
{
    if (!LblNotice) return;
    NoticeTransient = true;
    LblNotice->setTextFormat(Qt::PlainText);
    LblNotice->setText(text);
    LblNotice->setToolTip(text);
    NoticeTimer.start(ms);
}

void WorkbenchWindow::ShowAnalysisLine(const QString& plain, bool summary, bool force)
{
    AnalysisHold = plain;
    AnalysisHoldSummary = summary && !plain.isEmpty();
    if (NoticeTransient && !force) return;
    NoticeTransient = false;
    NoticeTimer.stop();
    if (!LblNotice) return;
    if (plain.isEmpty())
    {
        LblNotice->clear();
        LblNotice->setToolTip(QString());
        return;
    }
    LblNotice->setToolTip(plain);
    if (summary && AnalysisNoteCpu >= 0)
    {
        LblNotice->setTextFormat(Qt::RichText);
        LblNotice->setText(AnalysisSummaryHtml(plain));
    }
    else
    {
        LblNotice->setTextFormat(Qt::PlainText);
        LblNotice->setText(plain);
    }
}

void WorkbenchWindow::ClearAnalysisHold()
{
    AnalysisSummary.clear();
    AnalysisHold.clear();
    AnalysisHoldSummary = false;
    AnalysisLive = false;
    AnalysisNoteCpu = -1;
    AnalysisNoteAddr = 0;
    if (NoticeTransient || !LblNotice) return;
    NoticeTimer.stop();
    LblNotice->clear();
    LblNotice->setToolTip(QString());
}

QString WorkbenchWindow::AnalysisSummaryHtml(const QString& plain) const
{
    QString hex = QString("%1").arg(AnalysisNoteAddr, 8, 16, QChar('0'));
    QString key = "the first is " + hex;
    int at = plain.indexOf(key);
    QString body = plain.toHtmlEscaped();
    if (at >= 0)
    {
        QString href = QString("%1:%2").arg(AnalysisNoteCpu).arg(hex);
        QString link = QString("the first is <a href=\"%1\" style=\"color:%2;\">%3</a>")
                           .arg(href, WBColor::Blue().name(), hex);
        body = plain.left(at).toHtmlEscaped() + link + plain.mid(at + key.size()).toHtmlEscaped();
    }
    QFont f = LblNotice ? LblNotice->font() : font();
    QString size = f.pointSize() > 0 ? QString::number(f.pointSize()) + "pt" : QString::number(std::max(f.pixelSize(), 1)) + "px";
    return QString("<span style=\"font-family:'%1'; font-size:%2; color:%3;\">%4</span>")
        .arg(f.family().toHtmlEscaped(), size, WBColor::Text().name(), body);
}

void WorkbenchWindow::FollowAnalysisMarks(u32 firstInsn)
{
    const auto& ranges = H->AnalysisAll(CurCpu);
    for (const WBQ::AnalysisHints::Range& r : ranges)
        if (CurAddr >= r.Start && CurAddr < r.End) return;
    if (!DisasmView || !DisasmView->LookingAtUnknown(CurAddr) || !firstInsn) return;
    OpenAnalysisAddr(CurCpu, firstInsn);
}

void WorkbenchWindow::OpenAnalysisAddr(int cpu, u32 addr)
{
    if ((cpu & 1) != CurCpu)
    {
        CurCpu = cpu & 1;
        emit cpuChanged(CurCpu);
    }
    GoTo(addr);
    if (DisasmView)
    {
        DisasmView->window()->activateWindow();
        DisasmView->setFocus();
    }
}

void WorkbenchWindow::ShowPanel(const QString& name)
{
    ads::CDockWidget* d = DockMgr->findDockWidget(name);
    if (!d) return;
    d->toggleView(true);
    d->raise();
    DockMgr->setDockWidgetFocused(d);
    if (d->widget()) d->widget()->setFocus();
}

static bool TracingPanel(const QString& dock)
{
    // these show the selection, and the window's own actions for them leave the cursor where it is
    return dock == "dock_history" || dock == "dock_origin" || dock == "dock_inspector"
        || dock == "dock_xrefs" || dock == "dock_coverage";
}

bool WorkbenchWindow::PointAt(const UiPointRequest& req, UiPointState& out, QString& err)
{
    if (!req.Panel.isEmpty() && !DockMgr->findDockWidget(req.Panel))
    {
        err = "unknown panel '" + req.Panel + "'";
        return false;
    }

    // SetCpu jumps to that CPU's PC. Skip it when an address follows, or Back would return to the PC
    // instead of where the person was.
    const bool placed = req.HasAddr || req.HasSelAddr;
    if (req.Cpu >= 0 && !placed)
        SetCpu(req.Cpu);
    else if (req.Cpu >= 0 && (req.Cpu & 1) != CurCpu)
    {
        CurCpu = req.Cpu & 1;
        emit cpuChanged(CurCpu);
    }

    if (req.HasAddr)
    {
        if (LockPC) SetLockToPC(false);
        GoTo(req.Addr);
    }
    if (req.HasSelAddr || req.HasSelSize)
    {
        u32 n = req.HasSelSize ? req.SelSize : 1;
        if (!n) n = 1;
        SetSelection(req.HasSelAddr ? req.SelAddr : CurAddr, n);
    }
    // a selection with no cursor move still has to land on screen in the disassembly or memory
    if (!req.HasAddr && req.HasSelAddr && !TracingPanel(req.Panel))
    {
        if ((req.Panel.isEmpty() || req.Panel == "dock_disasm") && DisasmView)
            DisasmView->ShowAddress(req.SelAddr, false);
        if ((req.Panel.isEmpty() || req.Panel == "dock_memory") && MemoryView)
            MemoryView->OnNavigate(req.SelAddr);
    }

    if (isMinimized()) showNormal();
    show();
    raise();
    activateWindow();
    if (!req.Panel.isEmpty())
    {
        ShowPanel(req.Panel);
        if (ads::CDockWidget* d = DockMgr->findDockWidget(req.Panel))
            if (QWidget* top = d->window())
                if (top != this)
                {
                    if (top->isMinimized()) top->showNormal();
                    top->raise();
                    top->activateWindow();
                }
    }

    QString note = req.Note.trimmed();
    if (note.size() > 240) note = note.left(240);
    if (!note.isEmpty())
    {
        // the first show applies its workspace on the next turn, and that posts its own status text
        if (Workspaces.empty()) QTimer::singleShot(0, this, [this, note]() { Notice(note); });
        else Notice(note);
    }

    out.Cpu = CurCpu;
    out.Addr = CurAddr;
    out.SelAddr = SelAddr;
    out.SelSize = SelLen;
    out.Panel = req.Panel;
    out.FollowPC = LockPC;
    err.clear();
    return true;
}

void WorkbenchWindow::CaptureUi(const QString& dock, int maxWidth, UiCaptureFn cb)
{
    if (isMinimized()) { cb(false, {}, dock, false, "the workbench is minimized"); return; }
    if (!isVisible()) { cb(false, {}, dock, false, "the workbench is not open"); return; }

    QWidget* target = this;
    WBPanel* panel = nullptr;
    if (!dock.isEmpty())
    {
        ads::CDockWidget* d = DockMgr->findDockWidget(dock);
        if (!d || d->isClosed()) { cb(false, {}, dock, false, "panel is closed; ui.show it first"); return; }
        d->raise(); // its tab comes forward; the application is not activated
        QWidget* top = d->window();
        target = (top && top != this) ? top : static_cast<QWidget*>(d);
        QWidget* content = d->widget();
        for (WBPanel* p : Panels)
            if (p == content || (content && p->parentWidget() == content)) panel = p;
    }

    struct Job : QObject { std::function<void()> Step; };
    auto* job = new Job;
    job->setParent(this);
    auto spins = std::make_shared<int>(0);
    auto kicked = std::make_shared<bool>(false);
    job->Step = [this, job, dock, maxWidth, cb, target, panel, spins, kicked]()
    {
        int n = ++(*spins);
        bool stale = n * 30 >= 2000;
        if (panel && panel->IsShown() && !*kicked)
        {
            *kicked = true;
            if (!panel->IsBusy()) panel->Refresh();
        }
        bool busy = false;
        if (panel) busy = panel->IsBusy();
        else if (dock.isEmpty())
            for (WBPanel* p : Panels)
                if (p->IsShown() && p->IsBusy()) busy = true;
        bool wait = busy || (panel && !panel->IsShown()) || (panel && !*kicked) || (dock.isEmpty() && n < 2);
        if (!stale && wait)
        {
            QTimer::singleShot(30, job, [job]() { if (job->Step) job->Step(); });
            return;
        }

        auto fail = [&](const QString& e) { cb(false, {}, dock, false, e); job->deleteLater(); };
        if (isMinimized()) { fail("the workbench is minimized"); return; }
        if (!isVisible()) { fail("the workbench is not open"); return; }
        if (!target) { fail("could not capture the workbench"); return; }
        target->repaint();
        QPixmap pix = target->grab();
        if (pix.isNull() || pix.width() < 1 || pix.height() < 1) { fail("could not capture the workbench"); return; }
        QImage img = pix.toImage();
        if (maxWidth > 0 && img.width() > maxWidth)
            img = img.scaled(maxWidth, std::max(1, img.height()), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        cb(true, img, dock, stale, {});
        job->deleteLater();
    };
    QTimer::singleShot(0, job, [job]() { if (job->Step) job->Step(); });
}

void WorkbenchWindow::BuildPanels()
{
    DisasmView = new DisasmPanel(this);
    MemoryView = new MemoryPanel(this);
    // no central widget: every panel, the disassembly included, can dock, float or close

    AddDock("Disassembly", "dock_disasm", DisasmView);
    AddDock("Memory", "dock_memory", MemoryView);
    AddDock("Functions", "dock_functions", new FunctionsPanel(this));
    AddDock("Bookmarks", "dock_bookmarks", new BookmarksPanel(this));
    AddDock("Registers", "dock_registers", new RegistersPanel(this));
    AddDock("Inspector", "dock_inspector", new InspectorPanel(this));
    AddDock("Annotations", "dock_annotations", new AnnotationsPanel(this));
    AddDock("Call stack", "dock_callstack", new CallStackPanel(this));
    AddDock("Call graph", "dock_callgraph", new CallGraphPanel(this));
    AddDock("Cross-references", "dock_xrefs", new XrefsPanel(this));
    AddDock("Coverage map", "dock_coverage", new CoveragePanel(this));
    AddDock("Write history", "dock_history", new WriteHistoryPanel(this));
    AddDock("Data origin", "dock_origin", new DataOriginPanel(this));
    AddDock("Overlays", "dock_overlays", new OverlaysPanel(this));
    AddDock("Compare setup", "dock_compare_setup", new CompareSetupPanel(this));
    AddDock("Source and diff", "dock_source", new SourcePanel(this));
    AddDock("Decompiler", "dock_decompiler", new DecompilerPanel(this));
    AddDock("Search", "dock_search", new SearchPanel(this));
    AddDock("Archive", "dock_archive", ArchiveView = new ArchivePanel(this));
    connect(ArchiveView, &ArchivePanel::located, this, [this](int member, u32 addr, u32 size)
    {
        if (member != NavigateToArchived) return;
        NavigateToArchived = -1;
        if (!size) { Notice("That file is not in RAM now", 4000); return; }
        GoTo(addr);
        SetSelection(addr, size);
    });
    AddDock("Containers", "dock_containers", ContainersView = new ContainersPanel(this));
    connect(ContainersView, &ContainersPanel::located, this, [this](int global, u32 addr, u32 size)
    {
        if (global != NavigateToContained) return;
        NavigateToContained = -1;
        if (!size) { Notice("That file is not in RAM now", 4000); return; }
        GoTo(addr);
        SetSelection(addr, size);
    });
    AddDock("Graphics", "dock_graphics", new GraphicsPanel(this));
    AddDock("Breakpoints", "dock_breakpoints", new BreakpointsPanel(this));
    AddDock("Recording", "dock_recording", new RecordingPanel(this));
    AddDock("Getting started", "dock_start", new GettingStartedPanel(this));

    // the game screen: a placeholder until the game is shown here
    GameHost = new QWidget();
    auto* gl = new QVBoxLayout(GameHost);
    gl->setContentsMargins(0, 0, 0, 0);
    GamePlaceholder = new QWidget(GameHost);
    auto* pl = new QVBoxLayout(GamePlaceholder);
    auto* pt = new QLabel("The game runs in its own window. It can run here instead, with its menus, screen layout and input.", GamePlaceholder);
    pt->setWordWrap(true);
    auto* pb = new QPushButton("Show the game here", GamePlaceholder);
    // trigger(), not setChecked(): the menu action's handler runs on triggered, which setChecked doesn't emit.
    // Already checked (the saved mode, still waiting for the game to start): show it now
    connect(pb, &QPushButton::clicked, this, [this]()
    {
        if (!ActEmbed->isChecked()) ActEmbed->trigger();
        else SetEmbeddedScreen(true);
    });
    pl->addStretch();
    pl->addWidget(pt);
    pl->addWidget(pb, 0, Qt::AlignHCenter);
    pl->addStretch();
    gl->addWidget(GamePlaceholder);
    // at least one DS screen wide, so the game is never cropped
    AddDock("Game screen", "dock_game", GameHost)->setMinimumSize(270, 220);

    for (WBPanel* p : Panels)
    {
        connect(this, &WorkbenchWindow::navigated, p, [p](u32 a) { p->OnNavigate(a); });
        connect(this, &WorkbenchWindow::selectionChanged, p, [p](u32 a, u32 s) { p->OnSelection(a, s); });
    }
}

void WorkbenchWindow::DefaultLayout()
{
    // centre column: disassembly over memory over the bottom tabs; left: functions + bookmarks;
    // right: registers, inspector, annotations. The side columns run the full height
    // (the dock manager only knows docks once they are added)
    auto dock = [this](const char* name) -> ads::CDockWidget*
    {
        for (ads::CDockWidget* d : Docks)
            if (d->objectName() == name) return d;
        return nullptr;
    };
    ads::CDockAreaWidget* centre = DockMgr->addDockWidget(ads::CenterDockWidgetArea, dock("dock_disasm"));
    // the game screen sits left of the disassembly, closed until the game is shown in the workbench
    DockMgr->addDockWidget(ads::LeftDockWidgetArea, dock("dock_game"), centre);
    dock("dock_game")->toggleView(false);
    ads::CDockAreaWidget* mem = DockMgr->addDockWidget(ads::BottomDockWidgetArea, dock("dock_memory"), centre);
    ads::CDockAreaWidget* bottom = nullptr;
    // every panel must have a place here: a built-in workspace that shows a dock the default layout
    // left out floats it, and the next layout restore deletes it with its floating container
    static const char* const tabs[] = {"dock_callstack", "dock_callgraph", "dock_xrefs", "dock_coverage", "dock_history", "dock_origin",
                                       "dock_overlays", "dock_source", "dock_compare_setup", "dock_decompiler", "dock_search", "dock_archive",
                                       "dock_containers", "dock_graphics", "dock_breakpoints", "dock_recording", "dock_start"};
    for (const char* n : tabs)
    {
        if (!bottom) bottom = DockMgr->addDockWidget(ads::BottomDockWidgetArea, dock(n), mem);
        else DockMgr->addDockWidgetTabToArea(dock(n), bottom);
    }
    dock(tabs[0])->setAsCurrentTab();
    ads::CDockAreaWidget* left = DockMgr->addDockWidget(ads::LeftDockWidgetArea, dock("dock_functions"));
    DockMgr->addDockWidget(ads::BottomDockWidgetArea, dock("dock_bookmarks"), left);
    ads::CDockAreaWidget* right = DockMgr->addDockWidget(ads::RightDockWidgetArea, dock("dock_registers"));
    ads::CDockAreaWidget* insp = DockMgr->addDockWidget(ads::BottomDockWidgetArea, dock("dock_inspector"), right);
    DockMgr->addDockWidget(ads::BottomDockWidgetArea, dock("dock_annotations"), insp);

    DockMgr->setSplitterSizes(centre, {460, 200, 260});
    DockMgr->setSplitterSizes(left, {600, 300});
    DockMgr->setSplitterSizes(right, {330, 330, 200});
    if (QSplitter* root = DockMgr->findChild<QSplitter*>(QString(), Qt::FindDirectChildrenOnly))
        if (root->count() == 3) root->setSizes({300, 840, 360});
}

void WorkbenchWindow::BuildMenus()
{
    QMenuBar* mb = menuBar();
    QMenu* file = mb->addMenu("&File");
    file->addAction("Open knowledge store...", this, [this]()
    {
        QString p = QFileDialog::getOpenFileName(this, "Knowledge store", H->ProjectDir(), "SQLite (*.db *.sqlite);;All files (*)");
        if (p.isEmpty()) return;
        H->Settings().KnowledgeStorePath = p;
        H->SaveSettings();
        QString err;
        if (!H->KB().Open(p, H->Core().Rom, H->Settings().AuthorName, err)) ShowError("Open knowledge store", err);
        else emit H->knowledgeChanged();
    });
    QAction* imp = file->addAction("Import static analysis (JSON)...", this, &WorkbenchWindow::ImportStaticAnalysis);
    imp->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    file->addSeparator();
    file->addAction("Save recording session now", this, [this]() { H->SaveSessionNow(); });
    file->addAction("Open project folder", this, [this]() { QDir().mkpath(H->ProjectDir()); QDesktopServices::openUrl(QUrl::fromLocalFile(H->ProjectDir())); });
    file->addSeparator();
    file->addAction("Workbench settings...", this, &WorkbenchWindow::OpenSettings);
    file->addAction("Close", this, &QWidget::close);

    QMenu* edit = mb->addMenu("&Edit");
    ActUndo = edit->addAction("Undo", this, [this]() { QString e; if (!KB().Undo(e)) Notice(e, 3000); });
    ActUndo->setShortcut(QKeySequence::Undo);
    ActRedo = edit->addAction("Redo", this, [this]() { QString e; if (!KB().Redo(e)) Notice(e, 3000); });
    ActRedo->setShortcut(QKeySequence::Redo);
    edit->addSeparator();
    QAction* a;
    a = edit->addAction("Rename...", this, [this]() { RenameAt(CurAddr); });
    a->setShortcut(QKeySequence(Qt::Key_N));
    a = edit->addAction("Comment...", this, [this]() { CommentAt(SelLen > 1 ? SelAddr : CurAddr, SelLen > 1 ? SelLen : 1); });
    a->setShortcut(QKeySequence(Qt::Key_Semicolon));
    a = edit->addAction("Define function...", this, [this]() { DefineFunctionAt(CurAddr); });
    a->setShortcut(QKeySequence(Qt::Key_P));
    a = edit->addAction("Define data...", this, [this]() { DefineDataAt(SelLen > 1 ? SelAddr : CurAddr, SelLen > 1 ? SelLen : 0); });
    a->setShortcut(QKeySequence(Qt::Key_D));
    a = edit->addAction("Add bookmark", this, [this]() { AddBookmarkAt(CurAddr); });
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    edit->addAction("Types...", this, [this]() { TypeEditorDialog d(this); d.exec(); });

    QMenu* nav = mb->addMenu("&Navigate");
    a = nav->addAction("Go to address...", this, &WorkbenchWindow::PromptGoTo);
    a->setShortcuts({QKeySequence(Qt::Key_G), QKeySequence(Qt::CTRL | Qt::Key_G)});
    a = nav->addAction("Back", this, &WorkbenchWindow::Back);
    a->setShortcuts({QKeySequence(Qt::ALT | Qt::Key_Left), QKeySequence(Qt::Key_Escape)});
    a = nav->addAction("Forward", this, &WorkbenchWindow::Forward);
    a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    ActLock = nav->addAction("Lock to PC", this, [this](bool on) { SetLockToPC(on); });
    ActLock->setCheckable(true);
    ActLock->setChecked(LockPC);
    ActLock->setShortcut(QKeySequence(Qt::Key_L));
    a = nav->addAction("Switch CPU (ARM9/ARM7)", this, [this]() { SetCpu(CurCpu ^ 1); });
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    a = nav->addAction("Locate archived file...", this, &WorkbenchWindow::PromptLocateArchived);
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    nav->addAction("Locate file in a container...", this, &WorkbenchWindow::PromptLocateContained);
    a = nav->addAction("Cross-references", this, [this]() { ShowXrefs(SelLen > 1 ? SelAddr : CurAddr); });
    a->setShortcut(QKeySequence(Qt::Key_X));

    QMenu* dbg = mb->addMenu("&Debug");
    dbg->setToolTipsVisible(true);
    ActPause = dbg->addAction("Pause", this, [this]()
    {
        if (H->Core().Dbg.InBreak() || !H->EmuRunning()) H->Resume();
        else H->Pause();
    });
    ActPause->setShortcut(QKeySequence(Qt::Key_F5));
    ActAnalyze = dbg->addAction("Mark unreached code", this, [this]()
    {
        if (H->AnalysisRunning()) H->CancelStaticAnalysis();
        else H->RunStaticAnalysis();
    });
    ActClearAnalysis = dbg->addAction("Clear unreached marks", this, [this]()
    {
        H->ClearStaticAnalysis();
        ClearAnalysisHold();
    });
    ActClearAnalysis->setToolTip("Remove the marks from the last run.");
    ActBreak = dbg->addAction("Break", this, [this]() { H->BreakNow(CurCpu); });
    ActBreak->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    ActBreak->setToolTip("Stop at the next instruction");
    ActStepInto = dbg->addAction("Step into", this, [this]() { H->Step(CurCpu, 1); });
    ActStepInto->setShortcuts({QKeySequence(Qt::Key_F7), QKeySequence(Qt::Key_F11)});
    ActStepOver = dbg->addAction("Step over", this, [this]() { H->StepOver(CurCpu); });
    ActStepOver->setShortcut(QKeySequence(Qt::Key_F10));
    ActFrame = dbg->addAction("Run one frame", this, [this]() { H->RunFrames(1, nullptr); });
    ActFrame->setShortcut(QKeySequence(Qt::Key_F8));
    ActRunTo = dbg->addAction("Run to cursor", this, [this]() { RunToAddr(CurAddr); });
    ActRunTo->setShortcut(QKeySequence(Qt::Key_F4));
    ActToggleBp = dbg->addAction("Toggle breakpoint", this, [this]() { ToggleBreakpointAt(CurAddr); });
    ActToggleBp->setShortcut(QKeySequence(Qt::Key_F9));
    BuildDebugBar();

    QMenu* build = mb->addMenu("&Build");
    a = build->addAction("Compare function with the original", this, [this]()
    {
        for (WBPanel* p : Panels)
            if (SourcePanel* sp = qobject_cast<SourcePanel*>(p))
            {
                ShowPanel("dock_source");
                sp->CompareAt(CurAddr);
            }
    });
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    a = build->addAction("Open the function's source in the editor", this, [this]()
    {
        for (WBPanel* p : Panels)
            if (SourcePanel* sp = qobject_cast<SourcePanel*>(p)) sp->OpenEditorAt(CurAddr);
    });
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    build->addSeparator();
    build->addAction("Compare setup...", this, [this]()
    {
        ShowPanel("dock_compare_setup");
        if (CompareSetupPanel* sp = FindPanel<CompareSetupPanel>()) sp->ShowTab(0);
    });

    QMenu* rec = mb->addMenu("&Recording");
    ActRec = rec->addAction("Recording", this, [this](bool on) { H->SetRecording(on); });
    ActRec->setCheckable(true);
    ActRec->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    ActHist = rec->addAction("Write history", this, [this](bool on) { H->SetWriteHistory(on); });
    ActHist->setCheckable(true);
    ActHist->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
    ActOrigin = rec->addAction("Data origin", this, [this](bool on) { H->SetDataOrigin(on); });
    ActOrigin->setCheckable(true);
    ActOrigin->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    rec->setToolTipsVisible(true);
    QMenu* also = rec->addMenu("When recording starts, also turn on");
    a = also->addAction("Write history", this, [this](bool on) { H->Settings().AlsoHistory = on; H->SaveSettings(); });
    a->setCheckable(true);
    a->setChecked(H->Settings().AlsoHistory);
    a = also->addAction("Data origin", this, [this](bool on) { H->Settings().AlsoOrigin = on; H->SaveSettings(); });
    a->setCheckable(true);
    a->setChecked(H->Settings().AlsoOrigin);
    rec->addSeparator();
    a = rec->addAction("Reset game with tracking...", this, [this]() { ResetWithTracking(); });
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));

    QMenu* view = mb->addMenu("&View");
    BuildViewMenu(view);
    view->addSeparator();
    ActEmbed = view->addAction("Game screen in the workbench", this, [this](bool on)
    {
        Config::Table cfg = Config::GetGlobalTable();
        cfg.SetBool("Workbench.EmbeddedScreen", on);
        Config::Save();
        SetEmbeddedScreen(on);
    });
    ActEmbed->setCheckable(true);
    ActEmbed->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    ActEmbed->setToolTip("Run the game in the Game screen panel instead of its own window");
    ActHideMain = view->addAction("Hide the separate game window", this, [this](bool on)
    {
        Config::Table cfg = Config::GetGlobalTable();
        cfg.SetBool("Workbench.HideMainWindow", on);
        Config::Save();
        SetHideMainWindow(on);
    });
    ActHideMain->setCheckable(true);
    ActHideMain->setEnabled(false);

    QMenu* help = mb->addMenu("&Help");
    help->addAction("Getting started", this, [this]() { ShowPanel("dock_start"); });
    help->addAction("Feature requirements...", this, [this]() { ShowFeatureRequirements(); });

    QMenu* win = mb->addMenu("&Window");
    WorkspaceMenu = win->addMenu("Workspaces");
    a = win->addAction("Panels...", this, [this]() { ShowPanelPalette(); });
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    win->addSeparator();
    win->addAction("Reset layout", this, [this]() { if (!Workspaces.empty()) { RestoreLayout(Workspaces[CurWorkspace].Default); SaveLayout(); } });

    for (QAction* act : findChildren<QAction*>())
        act->setShortcutContext(Qt::WindowShortcut);
}

void WorkbenchWindow::BuildStatusBar()
{
    auto badge = [this](const QString& text, const QString& bg)
    {
        QLabel* l = new QLabel(text, this);
        l->setStyleSheet(QString("QLabel { background: %1; color: %2; font-weight: bold; padding: 1px 6px; border-radius: 3px; }").arg(bg, WBColor::Text().name()));
        l->setVisible(false);
        statusBar()->addWidget(l);
        return l;
    };
    // REC, HIST and ORIGIN are always there: grey when off, coloured when on; a click toggles them
    auto toggle = [this](const QString& text)
    {
        QToolButton* b = new QToolButton(this);
        b->setText(text);
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        statusBar()->addWidget(b);
        return b;
    };
    BadgeRec = toggle("REC");
    BadgeHist = toggle("HIST");
    BadgeOrigin = toggle("ORIGIN");
    connect(BadgeRec, &QToolButton::clicked, this, [this]() { H->SetRecording(!H->Status().Recording); });
    connect(BadgeHist, &QToolButton::clicked, this, [this]() { H->SetWriteHistory(!H->Status().History); });
    connect(BadgeOrigin, &QToolButton::clicked, this, [this]() { H->SetDataOrigin(!H->Status().Origin); });
    LblPause = badge("PAUSED", WBColor::Select().name());
    LblFrames = new QLabel(this);
    LblSpeed = new QLabel(this);
    LblPC = new QLabel(this);
    LblKB = new QLabel(this);
    statusBar()->addWidget(LblFrames);
    statusBar()->addWidget(LblSpeed);
    // notices get their own place: showMessage would draw over the badges
    LblNotice = new QLabel(this);
    LblNotice->setTextInteractionFlags(Qt::TextBrowserInteraction);
    LblNotice->setOpenExternalLinks(false);
    connect(LblNotice, &QLabel::linkActivated, this, [this](const QString& href)
    {
        int colon = href.indexOf(':');
        if (colon < 0) return;
        bool ok = false;
        int cpu = href.left(colon).toInt(&ok);
        if (!ok) return;
        u32 addr = href.mid(colon + 1).toUInt(&ok, 16);
        if (!ok) return;
        OpenAnalysisAddr(cpu, addr);
    });
    statusBar()->addWidget(LblNotice, 1);
    NoticeTimer.setSingleShot(true);
    connect(&NoticeTimer, &QTimer::timeout, this, [this]()
    {
        NoticeTransient = false;
        if (!AnalysisHold.isEmpty()) ShowAnalysisLine(AnalysisHold, AnalysisHoldSummary, true);
        else if (LblNotice)
        {
            LblNotice->clear();
            LblNotice->setToolTip(QString());
        }
    });
    statusBar()->addPermanentWidget(LblPC);
    statusBar()->addPermanentWidget(LblKB);
}

void WorkbenchWindow::UpdateStatus()
{
    WBStatus s = H->Status();
    auto setBadge = [this](int i, QToolButton* b, const QString& bg, const QString& fg, const QString& text, const QString& tip)
    {
        QString style = QString("QToolButton { background: %1; color: %2; font-weight: bold; padding: 1px 6px; border-radius: 3px; border: none; }")
                            .arg(bg, fg);
        if (style != BadgeStyle[i]) { BadgeStyle[i] = style; b->setStyleSheet(style); }
        if (b->text() != text) b->setText(text);
        if (b->toolTip() != tip) b->setToolTip(tip);
    };
    QString recMissing = H->Missing(WBNeed::Recording);
    setBadge(0, BadgeRec, s.Recording ? WBColor::ChipRec().name() : WBColor::Off().name(),
             s.Recording ? WBColor::Text().name() : WBColor::Muted().name(), "REC",
             s.Recording ? QString("Recording since frame %1. Click to stop it and save the session.").arg(s.RecordingSince)
                         : recMissing + (s.Available && s.EmuState != WBStatus::NoGame ? " Click to start it.\n\n" : "\n\n") + WBHost::Covers(WBNeed::Recording));
    u32 histFrom = s.HistoryWrapped ? s.HistoryOldestFrame : s.HistorySince;
    int fill = s.HistoryCap ? (int)(s.HistoryCount * 100 / s.HistoryCap) : 0;
    setBadge(1, BadgeHist, WBColor::Off().name(),
             !s.History ? WBColor::Muted().name() : s.HistoryWrapped ? WBColor::Amber().name() : WBColor::Text().name(),
             !s.History ? "HIST" : s.HistoryWrapped ? QString("HIST full, since %1").arg(histFrom) : QString("HIST since %1, %2%").arg(histFrom).arg(fill),
             s.History ? QString("Write history: %1 of %2 writes kept%3. Click to turn it off.")
                             .arg(s.HistoryCount).arg(s.HistoryCap)
                             .arg(s.HistoryWrapped ? QString("; full, so writes before frame %1 were dropped").arg(histFrom) : QString(", since frame %1").arg(histFrom))
                       : "Write history is off. Click to turn it on.\n\n" + WBHost::Covers(WBNeed::WriteHistory));
    setBadge(2, BadgeOrigin, s.Origin ? WBColor::ChipOrigin().name() : WBColor::Off().name(),
             s.Origin ? WBColor::Text().name() : WBColor::Muted().name(), s.Origin ? QString("ORIGIN since %1").arg(s.OriginSince) : "ORIGIN",
             s.Origin ? QString("Data origin since frame %1. Click to turn it off.").arg(s.OriginSince)
                      : "Data origin is off. Click to turn it on.\n\n" + WBHost::Covers(WBNeed::DataOrigin));
    bool paused = s.EmuState == WBStatus::Paused || s.EmuState == WBStatus::Break;
    LblPause->setVisible(paused || s.EmuState == WBStatus::NoGame);
    LblPause->setText(s.EmuState == WBStatus::Break ? QString("BREAK (%1)").arg(QString::fromStdString(s.BreakInfo.Reason))
                      : s.EmuState == WBStatus::NoGame ? "NO GAME" : "PAUSED");
    LblFrames->setText(s.Recording ? QString("frames recorded: %1").arg(s.FramesRecorded) : QString("frame %1").arg(s.Frame));
    LblSpeed->setText(s.EmuState == WBStatus::Running ? QString("%1 fps").arg(s.Fps, 0, 'f', 1) : QString());
    QString funcName;
    if (KB().IsOpen())
        if (const WBKB::Function* f = KB().FunctionAt(CurCpu, CurAddr, OverlayContext(CurAddr)))
            funcName = f->Name.isEmpty() ? QString("sub_%1").arg(f->Addr, 8, 16, QChar('0')) : f->Name;
    LblPC->setText(QString("%1 PC %2%3").arg(CurCpu ? "ARM7" : "ARM9").arg(s.PC[CurCpu], 8, 16, QChar('0'))
                   .arg(funcName.isEmpty() ? QString() : "  " + funcName));
    LblKB->setText(KB().IsOpen() ? QString("KB: %1").arg(KB().Path()) : QString("no knowledge store"));
    QString title = "melonDS Decomp Workbench";
    if (!s.RomTitle.isEmpty()) title += " - " + s.RomTitle + " (" + s.RomCode + ")";
    if (s.Recording) title = "[REC] " + title;
    setWindowTitle(title);
    bool avail = s.Available;
    bool game = s.EmuState != WBStatus::NoGame;
    // disabled items say why; write history and data origin start Recording themselves
    QString why = !avail ? "Disabled while the JIT recompiler is on: recording needs the interpreter" : !game ? "No game is running" : QString();
    for (QAction* a : {ActRec, ActHist, ActOrigin})
    {
        a->setEnabled(avail && game);
        a->setToolTip(why);
    }
    ActRec->setChecked(s.Recording);
    ActHist->setChecked(s.History);
    ActOrigin->setChecked(s.Origin);
    if (ActPause)
    {
        ActPause->setText(paused ? "Resume" : "Pause");
        ActPause->setEnabled(game);
        bool step = game && avail;
        for (QAction* act : {ActBreak, ActStepInto, ActStepOver, ActFrame, ActRunTo, ActToggleBp})
            if (act) act->setEnabled(step);
    }
    if (ActAnalyze)
    {
        bool busy = H->AnalysisRunning();
        ActAnalyze->setEnabled(busy || (game && paused));
        ActAnalyze->setText(busy ? "Cancel static analysis" : "Mark unreached code");
        QString tip = "While paused, follow code that has already run and mark the instructions and literal pools it has not reached. "
                      "The marks stay until they are cleared or this command is run again. They are not written to the knowledge store.";
        if (busy) tip = "Stop this walk. The marks already shown stay.";
        else if (!game) tip = "No game is running.";
        else if (!paused) tip = "Pause the game first (F5).";
        ActAnalyze->setToolTip(tip);
    }
    if (ActClearAnalysis) ActClearAnalysis->setEnabled(H->HasAnalysis() && !H->AnalysisRunning());
    if (s.EmuState == WBStatus::NoGame && (H->HasAnalysis() || AnalysisLive || !AnalysisSummary.isEmpty()))
    {
        bool running = H->AnalysisRunning();
        ClearAnalysisHold();
        if (H->HasAnalysis() || running) H->ClearStaticAnalysis();
    }
    ActUndo->setEnabled(KB().CanUndo());
    ActRedo->setEnabled(KB().CanRedo());
}

void WorkbenchWindow::Tick()
{
    if (!isVisible()) return;
    // the layout is saved as it changes (checked once a second), so a crash doesn't lose it
    if (++SaveTicks >= 10)
    {
        SaveTicks = 0;
        SaveLayout();
    }
    if (++IdentTicks >= 8)
    {
        IdentTicks = 0;
        H->IdentifyFunctions();
    }
    if (SaveTicks % 5 == 0) UpdateBanners();
    WBStatus s = H->Status();
    UpdateStatus();
    bool running = s.EmuState == WBStatus::Running;
    u32 pc = s.EmuState == WBStatus::Break ? s.BreakInfo.PC : s.PC[CurCpu];
    if (running || pc != LastPC || (s.EmuState == WBStatus::Break) != LastWasBreak)
    {
        LastPC = pc;
        LastWasBreak = s.EmuState == WBStatus::Break;
        if (LockPC && !running) { CurAddr = pc; emit navigated(pc); }
        RefreshBreakpointCache();
        for (WBPanel* p : Panels)
            if (p->IsShown()) p->Refresh();
    }
}

void WorkbenchWindow::SetEmbeddedScreen(bool on)
{
    EmuInstance* inst = H->Instance();
    if (on && !GameWin && inst)
    {
        GameWin = inst->createEmbeddedWindow(GameHost);
        if (!GameWin) { ShowError("Game screen", "melonDS has no free window slot for the game screen."); if (ActEmbed) ActEmbed->setChecked(false); return; }
        GameHost->layout()->addWidget(GameWin);
        GamePlaceholder->hide();
        GameWin->show();
        connect(GameWin, &QObject::destroyed, this, [this]()
        {
            // closed from its own menu (File > Quit closes the window)
            GameWin = nullptr;
            GamePlaceholder->show();
            if (ActEmbed) ActEmbed->setChecked(false);
            if (ActHideMain) ActHideMain->setEnabled(false);
            if (MainWindow* mw = H->Instance()->getMainWindow()) mw->show();
        });
        ShowPanel("dock_game");
        if (qEnvironmentVariableIsSet("DSWB_FLOAT_TEST"))
        {
            // test hook: float the game panel, then dock it back (the native OpenGL window is reparented twice)
            QTimer::singleShot(3000, this, [this]() { if (ads::CDockWidget* d = DockMgr->findDockWidget("dock_game")) d->setFloating(); });
            QTimer::singleShot(7000, this, [this]()
            {
                ads::CDockWidget* d = DockMgr->findDockWidget("dock_game");
                ads::CDockWidget* dis = DockMgr->findDockWidget("dock_disasm");
                if (d && dis && dis->dockAreaWidget()) DockMgr->addDockWidget(ads::LeftDockWidgetArea, d, dis->dockAreaWidget());
            });
        }
        if (ActHideMain)
        {
            ActHideMain->setEnabled(true);
            SetHideMainWindow(ActHideMain->isChecked());
        }
        Notice("The game runs in the Game screen panel; its menus are at the top of the panel.", 8000);
    }
    else if (!on && GameWin)
    {
        MainWindow* g = GameWin;
        GameWin = nullptr;
        disconnect(g, &QObject::destroyed, this, nullptr);
        if (inst) inst->deleteWindow(g->getWindowID(), true);
        GamePlaceholder->show();
        if (ActHideMain) ActHideMain->setEnabled(false);
        if (inst)
            if (MainWindow* mw = inst->getMainWindow()) mw->show();
    }
}

void WorkbenchWindow::SetHideMainWindow(bool on)
{
    MainWindow* mw = H->Instance() ? H->Instance()->getMainWindow() : nullptr;
    if (!mw) return;
    // only while the game shows here: otherwise there would be no game on screen
    if (on && GameWin) mw->hide();
    else mw->show();
}

bool WorkbenchWindow::eventFilter(QObject* o, QEvent* e)
{
    if (e->type() == QEvent::ChildAdded)
    {
        if (auto* b = qobject_cast<QAbstractButton*>(static_cast<QChildEvent*>(e)->child()))
        {
            QPointer<QAbstractButton> btn(b);
            QTimer::singleShot(0, this, [this, btn]() { if (btn) StampButton(btn); });
        }
    }
    // The game window uses some of the same keys (F9 loads a state, Ctrl+Shift+R toggles recording).
    // Two shortcuts for one key are ambiguous, so neither action runs. While the key is going to the
    // workbench, run its action here and accept the event so the game window does not take it.
    if (e->type() == QEvent::ShortcutOverride)
    {
        QWidget* w = qobject_cast<QWidget*>(o);
        if (w && (w == this || isAncestorOf(w)) && !(GameWin && (w == GameWin || GameWin->isAncestorOf(w))))
        {
            auto* ke = static_cast<QKeyEvent*>(e);
            QKeySequence seq((ke->modifiers() & ~Qt::KeypadModifier) | ke->key());
            bool owned = false;
            QAction* hit = nullptr;
            for (QAction* a : findChildren<QAction*>())
                for (const QKeySequence& s : a->shortcuts())
                    if (s == seq)
                    {
                        owned = true;
                        if (a->isEnabled()) hit = a;
                    }
            if (owned)
            {
                if (hit && w == QApplication::focusWidget() && !ke->isAutoRepeat()) hit->trigger();
                e->accept();
                return true;
            }
        }
    }
    // keys typed into the embedded game go to the game: the workbench's shortcuts stay out of the way,
    // but the game window's own (savestates, pause...) still work
    if (GameWin && e->type() == QEvent::ShortcutOverride)
    {
        QWidget* w = qobject_cast<QWidget*>(o);
        if (w && (w == GameWin || GameWin->isAncestorOf(w)))
        {
            auto* ke = static_cast<QKeyEvent*>(e);
            QKeySequence seq(ke->modifiers() | ke->key());
            for (QAction* a : GameWin->findChildren<QAction*>())
                for (const QKeySequence& s : a->shortcuts())
                    if (s == seq) return false;
            e->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

void WorkbenchWindow::ResetWithTracking()
{
    // reset the console with tracking armed, so loading at boot is covered (PR-4)
    WBStatus s = H->Status();
    if (s.EmuState == WBStatus::NoGame) { ShowError("Reset game with tracking", H->Missing(WBNeed::Game)); return; }
    if (!s.Available) { ShowError("Reset game with tracking", H->Missing(WBNeed::Interpreter) + " Use the Recording panel or Turn off the JIT first."); return; }
    QDialog dlg(this);
    dlg.setWindowTitle("Reset game with tracking");
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel("The game restarts with these on, so everything it loads from boot is recorded.", &dlg));
    auto* rec = new QCheckBox("Recording (coverage, calls, card reads)", &dlg);
    auto* hist = new QCheckBox("Write history", &dlg);
    auto* origin = new QCheckBox("Data origin", &dlg);
    rec->setChecked(true);
    hist->setChecked(s.History || H->Settings().AlsoHistory);
    origin->setChecked(s.Origin || H->Settings().AlsoOrigin);
    connect(hist, &QCheckBox::toggled, rec, [rec](bool on) { if (on) rec->setChecked(true); });
    connect(origin, &QCheckBox::toggled, rec, [rec](bool on) { if (on) rec->setChecked(true); });
    connect(rec, &QCheckBox::toggled, &dlg, [hist, origin](bool on) { if (!on) { hist->setChecked(false); origin->setChecked(false); } });
    lay->addWidget(rec);
    lay->addWidget(hist);
    lay->addWidget(origin);
    auto* form = new QFormLayout();
    auto* fileMB = new QSpinBox(&dlg);
    fileMB->setRange(0, 65536);
    fileMB->setSuffix(" MB");
    fileMB->setSpecialValueText("in memory");
    fileMB->setValue((int)H->Settings().HistoryFileMB);
    fileMB->setToolTip("Write history in a file of this size (1 GB holds about 33 million writes); in memory keeps 4 million");
    form->addRow("Write history capacity:", fileMB);
    lay->addLayout(form);
    auto* keep = new QRadioButton("Keep the current session going", &dlg);
    auto* fresh = new QRadioButton("Save the current session and start a new one", &dlg);
    fresh->setChecked(true);
    keep->setEnabled(s.Recording);
    if (!s.Recording) fresh->setText("Start a new session");
    lay->addWidget(keep);
    lay->addWidget(fresh);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->button(QDialogButtonBox::Ok)->setText("Reset");
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;

    QString err;
    if ((quint32)fileMB->value() != H->Settings().HistoryFileMB && !H->SetHistoryFile((quint32)fileMB->value(), err, true))
    {
        ShowError("Reset game with tracking", err);
        return;
    }
    if (s.Recording && (fresh->isChecked() || !rec->isChecked())) H->SetRecording(false);
    if (rec->isChecked()) H->SetRecording(true);
    H->SetWriteHistory(hist->isChecked());
    H->SetDataOrigin(origin->isChecked());
    if (!H->Reset(err)) { ShowError("Reset game with tracking", err); return; }
    QStringList on;
    if (rec->isChecked()) on << "recording";
    if (hist->isChecked()) on << "write history";
    if (origin->isChecked()) on << "data origin";
    Notice("Game reset" + (on.isEmpty() ? QString() : " with " + on.join(", ") + " on from boot"), 8000);
}

void WorkbenchWindow::BuildDebugBar()
{
    QToolBar* bar = addToolBar("Debug");
    bar->setObjectName("DebugBar");
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->addAction(ActPause);
    bar->addAction(ActBreak);
    bar->addAction(ActStepInto);
    bar->addAction(ActStepOver);
    bar->addAction(ActFrame);
    bar->addAction(ActRunTo);
    bar->addAction(ActToggleBp);
}

void WorkbenchWindow::RefreshBreakpointCache()
{
    int gen = ++BpCacheGen;
    H->Query<std::vector<ExecBpMark>>([this]()
    {
        std::vector<ExecBpMark> v;
        for (const Breakpoint& bp : H->Core().Dbg.List())
            if (bp.Kind == BreakKind::Exec) v.push_back({bp.Addr, bp.Enabled});
        return v;
    }, this, [this, gen](std::vector<ExecBpMark> v)
    {
        if (gen != BpCacheGen) return;
        ExecBreakpoints = std::move(v);
        if (DisasmView) DisasmView->update();
    });
}

void WorkbenchWindow::MutateBreakpointAt(u32 addr, int op)
{
    int cpu = CurCpu;
    H->RunSync([this, addr, cpu, op]()
    {
        for (const Breakpoint& bp : H->Core().Dbg.List())
            if (bp.Kind == BreakKind::Exec && bp.Addr == (addr & ~1u) && (bp.Cpu == cpu || bp.Cpu == -1))
            {
                if (op == 0) H->Core().Dbg.RemoveBreakpoint(bp.Id);
                else H->Core().Dbg.SetBreakpointEnabled(bp.Id, op == 1);
                return;
            }
    });
    RefreshBreakpointCache();
    emit H->breakpointsChanged();
}

void WorkbenchWindow::SetAllBreakpoints(bool enabled)
{
    H->RunSync([this, enabled]()
    {
        for (const Breakpoint& bp : H->Core().Dbg.List())
            H->Core().Dbg.SetBreakpointEnabled(bp.Id, enabled);
    });
    RefreshBreakpointCache();
    emit H->breakpointsChanged();
}

void WorkbenchWindow::SetCpu(int cpu)
{
    CurCpu = cpu & 1;
    emit cpuChanged(CurCpu);
    WBStatus s = H->Status();
    GoTo(s.PC[CurCpu], false);
}

void WorkbenchWindow::SetLockToPC(bool on)
{
    LockPC = on;
    ActLock->setChecked(on);
    if (on)
    {
        WBStatus s = H->Status();
        GoTo(s.EmuState == WBStatus::Break && s.BreakInfo.Cpu == (u32)CurCpu ? s.BreakInfo.PC : s.PC[CurCpu], false);
    }
}

int WorkbenchWindow::OverlayContext(u32 addr) const
{
    if (Epoch >= 0)
    {
        for (const WBKB::Module& m : const_cast<WorkbenchWindow*>(this)->KB().Modules())
            if (m.Kind == "overlay" && m.OverlayId == Epoch && addr >= m.Load && addr < m.End()) return Epoch;
    }
    return H->OverlayLoadedAt(addr);
}

void WorkbenchWindow::SetEpochOverride(int overlay)
{
    Epoch = overlay;
    for (WBPanel* p : Panels) p->Refresh();
    emit navigated(CurAddr);
}

void WorkbenchWindow::GoTo(u32 addr, bool record)
{
    if (record && addr != CurAddr)
    {
        BackStack.push_back(CurAddr);
        if (BackStack.size() > 256) BackStack.erase(BackStack.begin());
        FwdStack.clear();
        if (LockPC) SetLockToPC(false);
    }
    CurAddr = addr;
    SelAddr = addr;
    SelLen = 1;
    emit navigated(addr);
    emit selectionChanged(addr, 1);
}

void WorkbenchWindow::SetSelection(u32 addr, u32 size)
{
    SelAddr = addr;
    SelLen = std::max<u32>(size, 1);
    emit selectionChanged(SelAddr, SelLen);
}

void WorkbenchWindow::Back()
{
    if (BackStack.empty()) return;
    FwdStack.push_back(CurAddr);
    u32 a = BackStack.back();
    BackStack.pop_back();
    if (LockPC) SetLockToPC(false);
    CurAddr = a;
    SelAddr = a;
    SelLen = 1;
    emit navigated(a);
    emit selectionChanged(a, 1);
}

void WorkbenchWindow::Forward()
{
    if (FwdStack.empty()) return;
    BackStack.push_back(CurAddr);
    u32 a = FwdStack.back();
    FwdStack.pop_back();
    CurAddr = a;
    SelAddr = a;
    SelLen = 1;
    emit navigated(a);
    emit selectionChanged(a, 1);
}

bool WorkbenchWindow::ParseAddress(const QString& text, u32& addr, int* cpu)
{
    QString t = text.trimmed();
    if (t.isEmpty()) return false;
    int c = CurCpu;
    if (KB().LookupName(t, addr, c)) { if (cpu) *cpu = c; return true; }
    if (t.compare("pc", Qt::CaseInsensitive) == 0) { addr = H->Status().PC[CurCpu]; return true; }
    bool ok;
    QString h = t;
    if (h.startsWith("0x", Qt::CaseInsensitive)) h = h.mid(2);
    if (h.endsWith('h', Qt::CaseInsensitive)) h.chop(1);
    addr = h.toUInt(&ok, 16);
    return ok;
}

void WorkbenchWindow::PromptGoTo()
{
    bool ok;
    QString t = QInputDialog::getText(this, "Go to", "Address, name or 'pc':", QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    u32 a;
    int cpu = CurCpu;
    if (!ParseAddress(t, a, &cpu)) { ShowError("Go to", "Not an address or known name: " + t); return; }
    if (cpu != CurCpu) { CurCpu = cpu; emit cpuChanged(cpu); }
    GoTo(a);
    if (DisasmView) { DisasmView->window()->activateWindow(); DisasmView->setFocus(); }
}

QString WorkbenchWindow::AddrText(u32 addr) const
{
    QString n = const_cast<WorkbenchWindow*>(this)->KB().NameAt(CurCpu, addr, OverlayContext(addr), true);
    QString h = QString("%1").arg(addr, 8, 16, QChar('0'));
    return n.isEmpty() ? h : h + " <" + n + ">";
}

void WorkbenchWindow::ShowError(const QString& what, const QString& err)
{
    QMessageBox::warning(this, what, err);
}

// ---------------------------------------------------------------- shared actions

void WorkbenchWindow::RenameAt(u32 addr)
{
    if (!KB().IsOpen()) { ShowError("Rename", "No knowledge store is open (load a ROM first)."); return; }
    int ov = OverlayContext(addr);
    QString cur = KB().NameAt(CurCpu, addr, ov);
    bool ok;
    QString n = QInputDialog::getText(this, "Rename", QString("Name for %1:").arg(addr, 8, 16, QChar('0')), QLineEdit::Normal, cur, &ok);
    if (!ok) return;
    QString err;
    if (!KB().Rename(CurCpu, addr, n.trimmed(), ov, err)) ShowError("Rename", err);
}

void WorkbenchWindow::CommentAt(u32 addr, u32 size)
{
    if (!KB().IsOpen()) { ShowError("Comment", "No knowledge store is open (load a ROM first)."); return; }
    AnnotationDialog d(this, nullptr, addr, size);
    d.exec();
}

void WorkbenchWindow::DefineFunctionAt(u32 addr)
{
    if (!KB().IsOpen()) { ShowError("Define function", "No knowledge store is open (load a ROM first)."); return; }
    FunctionDialog d(this, addr);
    d.exec();
}

void WorkbenchWindow::DefineDataAt(u32 addr, u32 size)
{
    if (!KB().IsOpen()) { ShowError("Define data", "No knowledge store is open (load a ROM first)."); return; }
    DataDialog d(this, addr, size);
    d.exec();
}

void WorkbenchWindow::ToggleBreakpointAt(u32 addr)
{
    if (H->Core().JITActive()) { ShowError("Breakpoint", "Breakpoints need the interpreter. Turn the JIT recompiler off in Emu settings."); return; }
    int cpu = CurCpu;
    H->RunSync([this, addr, cpu]()
    {
        for (const Breakpoint& bp : H->Core().Dbg.List())
            if (bp.Kind == BreakKind::Exec && bp.Addr == (addr & ~1u) && (bp.Cpu == cpu || bp.Cpu == -1))
            {
                H->Core().Dbg.RemoveBreakpoint(bp.Id);
                return;
            }
        Breakpoint bp;
        bp.Kind = BreakKind::Exec;
        bp.Addr = addr & ~1u;
        bp.Size = 1;
        bp.Cpu = cpu;
        bp.Author = "person";
        std::string e;
        H->Core().Dbg.AddBreakpoint(bp, e);
    });
    RefreshBreakpointCache();
    for (WBPanel* p : Panels) p->Refresh();
}

void WorkbenchWindow::AddWatchpoint(u32 addr, u32 size, int kind)
{
    if (H->Core().JITActive()) { ShowError("Watchpoint", "Watchpoints need the interpreter. Turn the JIT recompiler off in Emu settings."); return; }
    int cpu = CurCpu;
    H->RunSync([this, addr, size, kind, cpu]()
    {
        Breakpoint bp;
        bp.Kind = (BreakKind)kind;
        bp.Addr = addr;
        bp.Size = std::max<u32>(size, 1);
        bp.Cpu = cpu;
        bp.Author = "person";
        std::string e;
        H->Core().Dbg.AddBreakpoint(bp, e);
    });
    for (WBPanel* p : Panels) p->Refresh();
}

void WorkbenchWindow::RunToAddr(u32 addr)
{
    if (H->Core().JITActive()) return;
    int cpu = CurCpu;
    H->RunSync([this, addr, cpu]()
    {
        Breakpoint bp;
        bp.Kind = BreakKind::Exec;
        bp.Addr = addr & ~1u;
        bp.Size = 1;
        bp.Cpu = cpu;
        bp.Temporary = true;
        std::string e;
        H->Core().Dbg.AddBreakpoint(bp, e);
    });
    SetLockToPC(true);
    H->Resume();
}

void WorkbenchWindow::TrackValue(u32 addr, u32 size)
{
    if (!H->Status().Recording || !H->Status().Origin)
    {
        if (QMessageBox::question(this, "Track this value", "Data origin tracking is off. Turn recording and data origin on now?") != QMessageBox::Yes) return;
        if (!H->Status().Recording) H->SetRecording(true);
        H->SetDataOrigin(true);
    }
    bool ok;
    QString name = QInputDialog::getText(this, "Track this value", "Label for values derived from these bytes:", QLineEdit::Normal,
                                         QString("%1+%2").arg(KB().NameAt(CurCpu, addr, OverlayContext(addr), true).isEmpty() ? QString("%1").arg(addr, 8, 16, QChar('0')) : KB().NameAt(CurCpu, addr, OverlayContext(addr), true)).arg(size), &ok);
    if (!ok) return;
    int cpu = CurCpu;
    H->RunSync([this, addr, size, cpu, name]()
    {
        DataSource s;
        s.Kind = SourceKind::Range;
        s.Addr = addr;
        s.Size = size;
        s.Cpu = (u32)cpu;
        s.Name = name.toStdString();
        H->Core().Taint.AddSource(s);
    });
    for (WBPanel* p : Panels) p->Refresh();
}

void WorkbenchWindow::TrackRegister(int reg)
{
    if (!H->Status().Recording || !H->Status().Origin)
    {
        if (QMessageBox::question(this, "Track this value", "Data origin tracking is off. Turn recording and data origin on now?") != QMessageBox::Yes) return;
        if (!H->Status().Recording) H->SetRecording(true);
        H->SetDataOrigin(true);
    }
    int cpu = CurCpu;
    bool ok;
    QString name = QInputDialog::getText(this, "Track this value", "Label:", QLineEdit::Normal, QString("r%1 @%2").arg(reg).arg(H->Status().PC[cpu], 8, 16, QChar('0')), &ok);
    if (!ok) return;
    H->RunSync([this, reg, cpu, name]()
    {
        DataSource s;
        s.Kind = SourceKind::Register;
        s.Cpu = (u32)cpu;
        s.Reg = (u32)reg;
        s.Addr = H->Core().CurrentPC(cpu);
        s.Name = name.toStdString();
        H->Core().Taint.AddSource(s);
    });
}

void WorkbenchWindow::AddBookmarkAt(u32 addr)
{
    if (!KB().IsOpen()) { ShowError("Bookmark", "No knowledge store is open (load a ROM first)."); return; }
    bool ok;
    QString n = QInputDialog::getText(this, "Bookmark", "Name:", QLineEdit::Normal, AddrText(addr), &ok);
    if (!ok) return;
    QString err;
    if (!KB().AddBookmark(CurCpu, addr, n, err)) ShowError("Bookmark", err);
}

void WorkbenchWindow::AddressActions(QMenu& m, int cpu, u32 addr, u32 size)
{
    // on the row's CPU, and at the address rather than following the PC
    auto on = [this, cpu](std::function<void()> f)
    {
        return [this, cpu, f]() { if (cpu >= 0 && cpu != CurCpu) SetCpu(cpu); f(); };
    };
    auto open = [this, addr](const char* panel) { SetLockToPC(false); GoTo(addr); ShowPanel(panel); };
    m.addAction("Go to disassembly", on([=]() { open("dock_disasm"); }));
    m.addAction("Go to memory", on([=]() { open("dock_memory"); }));
    m.addSeparator();
    m.addAction("Rename...\tN", on([=]() { RenameAt(addr); }));
    m.addAction("Comment...\t;", on([=]() { CommentAt(addr, size); }));
    m.addAction("Add bookmark", on([=]() { AddBookmarkAt(addr); }));
    m.addAction("Cross-references\tX", on([=]() { ShowXrefs(addr); }));
    m.addSeparator();
    m.addAction("Write history", on([=]() { SetSelection(addr, size); ShowPanel("dock_history"); }));
    m.addAction("Data origin", on([=]() { SetSelection(addr, size); ShowPanel("dock_origin"); }));
    m.addAction("Track origin from here", on([=]() { TrackValue(addr, size); }));
    m.addSeparator();
    m.addAction("Copy address", [addr]() { QApplication::clipboard()->setText(QString("%1").arg(addr, 8, 16, QChar('0'))); });
}

void WorkbenchWindow::AddRowMenu(QAbstractItemView* view, RowAddress at, std::function<void(QMenu&, const QModelIndex&)> extra)
{
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, view, [this, view, at, extra](const QPoint& pos)
    {
        QModelIndex idx = view->indexAt(pos);
        if (!idx.isValid()) return;
        QMenu m(view);
        if (extra) extra(m, idx);
        u32 a = 0, n = 1;
        int cpu = -1;
        if (at && at(idx, a, n, cpu))
        {
            if (!m.isEmpty()) m.addSeparator();
            AddressActions(m, cpu, a, std::max<u32>(n, 1));
        }
        if (!m.isEmpty()) m.exec(view->viewport()->mapToGlobal(pos));
    });
}

void WorkbenchWindow::ShowXrefs(u32 addr)
{
    ShowPanel("dock_xrefs");
    for (WBPanel* p : Panels)
        if (XrefsPanel* x = qobject_cast<XrefsPanel*>(p)) x->ShowFor(addr);
}

void WorkbenchWindow::LocateArchived(int member, bool navigate)
{
    if (member < 0) return;
    ShowPanel("dock_archive");
    NavigateToArchived = navigate ? member : -1;
    if (ArchiveView) ArchiveView->Locate(member);
}

void WorkbenchWindow::PromptLocateArchived()
{
    if (!ArchiveView) return;
    QStringList paths = ArchiveView->Paths();
    if (paths.isEmpty()) { ShowError("Locate archived file", "No game archive is available: " + H->ArchiveStatus()); return; }
    bool ok;
    QString t = QInputDialog::getItem(this, "Locate archived file", "File in the game archive:", paths, 0, true, &ok);
    if (!ok || t.trimmed().isEmpty()) return;
    int i = ArchiveView->IndexOf(t);
    if (i < 0) { ShowError("Locate archived file", "Not in the archive: " + t); return; }
    LocateArchived(i, true);
}

void WorkbenchWindow::LocateContained(int global, bool navigate)
{
    if (global < 0) return;
    ShowPanel("dock_containers");
    NavigateToContained = navigate ? global : -1;
    if (ContainersView) ContainersView->Locate(global);
}

void WorkbenchWindow::PromptLocateContained()
{
    if (!ContainersView) return;
    QStringList paths = ContainersView->Paths();
    if (paths.isEmpty()) { ShowError("Locate file in a container", H->Missing(WBNeed::Containers)); return; }
    bool ok;
    QString t = QInputDialog::getItem(this, "Locate file in a container", "File inside a NARC or SDAT (/file.narc:name):", paths, 0, true, &ok);
    if (!ok || t.trimmed().isEmpty()) return;
    int g = ContainersView->IndexOf(t);
    if (g < 0) { ShowError("Locate file in a container", "No such file in a container: " + t); return; }
    LocateContained(g, true);
}

void WorkbenchWindow::OpenSettings()
{
    SettingsDialog d(this);
    d.exec();
}

void WorkbenchWindow::ImportStaticAnalysis()
{
    if (!KB().IsOpen()) { ShowError("Import", "No knowledge store is open (load a ROM first)."); return; }
    QString p = QFileDialog::getOpenFileName(this, "Static analysis export", QString(), "JSON (*.json)");
    if (p.isEmpty()) return;
    QString err;
    int n = ImportAnalysisJson(KB(), p, err);
    if (n < 0) ShowError("Import", err);
    else Notice(QString("Imported %1 entries").arg(n), 5000);
}
