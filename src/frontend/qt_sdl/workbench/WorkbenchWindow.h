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

#ifndef WB_WORKBENCHWINDOW_H
#define WB_WORKBENCHWINDOW_H

#include <functional>
#include <vector>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>
#include <QWidget>

#include "types.h"

class WBHost;
enum class WBNeed;
class QAbstractButton;
class QAction;
class QPushButton;
class QToolButton;
class MainWindow;
class QMenu;
class QJsonObject;
namespace ads { class CDockManager; class CDockWidget; }
class WorkbenchWindow;
class QAbstractItemView;
class QMenu;
class QModelIndex;
namespace WBKB { class KnowledgeStore; }

// Base for every dockable panel. Panels refresh on the window's tick and on
// navigation; they fetch emulator data through WBHost::Query so the
// emulation thread is never touched directly.
class WBPanel : public QWidget
{
    Q_OBJECT
public:
    WBPanel(WorkbenchWindow* win, QWidget* parent = nullptr);
    virtual void Refresh() {}                  // periodic (emulation running or data changed)
    virtual void OnNavigate(melonDS::u32 addr) { (void)addr; }
    virtual void OnSelection(melonDS::u32 addr, melonDS::u32 size) { (void)addr; (void)size; }
    virtual void OnKnowledgeChanged() { Refresh(); }
    // what the panel needs before it has data, if anything right now; the window shows a banner with
    // a button that turns it on (UI_REQUIREMENTS PR-2)
    virtual bool Need(WBNeed& need, QString& what) const { (void)need; (void)what; return false; }
    bool IsShown() const;
    bool IsBusy() const { return Busy; }

protected:
    WorkbenchWindow* W;
    WBHost* H;
    bool Busy = false;     // a query is in flight
};

// a strip above a panel: what it is missing, a button that fixes it, and what the data will cover
class NeedBanner : public QFrame
{
    Q_OBJECT
public:
    NeedBanner(WBHost* h, QWidget* parent);
    void Update(WBPanel* p);
private:
    WBHost* H;
    QLabel* Text;
    QPushButton* Fix;
    int FixKind = -1;
};

class DisasmPanel;
class MemoryPanel;
class ArchivePanel;
class ContainersPanel;

class WorkbenchWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit WorkbenchWindow(WBHost* host);
    ~WorkbenchWindow();

    WBHost* Host() { return H; }
    WBKB::KnowledgeStore& KB();

    // navigation state shared by all panels
    int Cpu() const { return CurCpu; }
    void SetCpu(int cpu);
    melonDS::u32 Cursor() const { return CurAddr; }
    melonDS::u32 SelStart() const { return SelAddr; }
    melonDS::u32 SelSize() const { return SelLen; }
    bool LockedToPC() const { return LockPC; }
    void SetLockToPC(bool on);
    int OverlayContext(melonDS::u32 addr) const;   // overlay id for names at addr (epoch override or loaded overlay)
    int EpochOverride() const { return Epoch; }
    void SetEpochOverride(int overlay);              // -1 = follow the loaded overlay

    void GoTo(melonDS::u32 addr, bool record = true);
    void SetSelection(melonDS::u32 addr, melonDS::u32 size);
    void Back();
    void Forward();
    void PromptGoTo();
    bool ParseAddress(const QString& text, melonDS::u32& addr, int* cpu = nullptr);
    QString AddrText(melonDS::u32 addr) const;     // hex + name

    // shared actions (panels call these; shortcuts are bound in the window)
    void RenameAt(melonDS::u32 addr);
    void CommentAt(melonDS::u32 addr, melonDS::u32 size = 1);
    void DefineFunctionAt(melonDS::u32 addr);
    void DefineDataAt(melonDS::u32 addr, melonDS::u32 size = 0);
    void ToggleBreakpointAt(melonDS::u32 addr);
    void AddWatchpoint(melonDS::u32 addr, melonDS::u32 size, int kind);
    void RunToAddr(melonDS::u32 addr);
    void TrackValue(melonDS::u32 addr, melonDS::u32 size);
    void TrackRegister(int reg);
    void AddBookmarkAt(melonDS::u32 addr);
    void ShowXrefs(melonDS::u32 addr);
    void LocateArchived(int member, bool navigate = false);   // Archive panel: where the file is in RAM
    void PromptLocateArchived();
    void LocateContained(int global, bool navigate = false);  // Containers panel: where a file inside a container is in RAM
    void PromptLocateContained();
    void ShowError(const QString& what, const QString& err);
    void ShowPanel(const QString& name);            // shows, raises and focuses a panel by object name
    template <typename T> T* FindPanel() const
    {
        for (WBPanel* p : Panels)
            if (T* t = qobject_cast<T*>(p)) return t;
        return nullptr;
    }

    // ui.show / ui.capture. Panel is a dock object name (dock_disasm), already resolved, or empty.
    struct UiPointRequest
    {
        int Cpu = -1;                  // -1: leave the window's CPU
        bool HasAddr = false;
        melonDS::u32 Addr = 0;
        bool HasSelAddr = false;
        melonDS::u32 SelAddr = 0;
        bool HasSelSize = false;
        melonDS::u32 SelSize = 1;
        QString Panel;
        QString Note;
    };
    struct UiPointState
    {
        int Cpu = 0;
        melonDS::u32 Addr = 0, SelAddr = 0, SelSize = 1;
        QString Panel;
        bool FollowPC = true;
    };
    // empty or whitespace: no panel. Otherwise a name a person would type, or a dock object name.
    static bool ResolvePanel(const QString& name, QString& dock, QString& err);
    bool PointAt(const UiPointRequest& req, UiPointState& out, QString& err);
    using UiCaptureFn = std::function<void(bool ok, QImage image, QString panel, bool stale, QString err)>;
    void CaptureUi(const QString& dock, int maxWidth, UiCaptureFn cb);
    // GD-2: the actions every table, tree and map offers for an address (size > 1: a range); cpu -1 is
    // the current one
    void AddressActions(QMenu& m, int cpu, melonDS::u32 addr, melonDS::u32 size = 1);
    // right-click on a row of a table, tree or list: `at` gives the row's address, or false when it has
    // none; `extra` adds the panel's own actions above the shared ones
    using RowAddress = std::function<bool(const QModelIndex& row, melonDS::u32& addr, melonDS::u32& size, int& cpu)>;
    void AddRowMenu(QAbstractItemView* view, RowAddress at, std::function<void(QMenu&, const QModelIndex&)> extra = {});
    // a short message in the status bar, after the badges (GD-3)
    void Notice(const QString& text, int ms = 8000);
    void ResetWithTracking();                       // Recording > Reset game with tracking (PR-4)
    QString FeatureTableHtml() const;               // the prerequisites table with live state (PR-1, PR-6)
    void ShowFeatureRequirements();

    DisasmPanel* Disasm() { return DisasmView; }
    struct ExecBpMark { melonDS::u32 Addr = 0; bool Enabled = true; };
    std::vector<ExecBpMark> ExecBreakpoints;      // cached for the disassembly gutter (enabled and disabled)
    void MutateBreakpointAt(melonDS::u32 addr, int op);   // 0 delete, 1 enable, 2 disable
    void SetAllBreakpoints(bool enabled);

signals:
    void navigated(melonDS::u32 addr);
    void selectionChanged(melonDS::u32 addr, melonDS::u32 size);
    void cpuChanged(int cpu);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    void BuildPanels();
    void ApplyDarkTheme();
    void ApplyChrome();
    // Windows 11 paints a button's label black unless that button's own palette sets ButtonText.
    void StampButton(QAbstractButton* b);
    void ReloadColors();
    void BuildDebugBar();
    void BuildMenus();
    void BuildStatusBar();
    void UpdateStatus();
    void Tick();
    void DefaultLayout();
    ads::CDockWidget* AddDock(const QString& title, const QString& name, QWidget* w);
    void SaveLayout();

    // layout (WBLayout.cpp): the View menu, the Panels palette, workspaces
    struct Workspace
    {
        QString Name;
        QByteArray State;        // as the user left it
        QByteArray Default;      // what Reset to default restores
        bool BuiltIn = false;
        // the main window's geometry and the game screen mode (0 its own window, 1 in the workbench,
        // 2 in the workbench with the separate window hidden), as the user left them; empty and -1
        // until the workspace has been used, and then switching to it leaves them as they are
        QByteArray Geometry;
        int Screen = -1;
    };
    int ScreenMode() const;
    void CaptureWorkspace(Workspace& w);
    void ApplyWorkspace(int idx, bool layout);
    void DescribePanels();
    void BuildViewMenu(QMenu* view);
    void ShowPanelPalette();
    bool RestoreLayout(const QByteArray& state);
    void InitWorkspaces();
    void MergeWorkspace(const QJsonObject& o, bool project);
    void StoreWorkspaces();
    void SwitchWorkspace(int idx);
    void RebuildWorkspaceMenu();
    void FinishLayout();
    std::vector<Workspace> Workspaces;
    int CurWorkspace = 0;
    QMenu* WorkspaceMenu = nullptr;
    void RefreshBreakpointCache();
    void OpenSettings();
    void ImportStaticAnalysis();
    // the unreached-code summary stays until the next run, a clear, or a new game
    void ShowAnalysisLine(const QString& plain, bool summary, bool force);
    void ClearAnalysisHold();
    QString AnalysisSummaryHtml(const QString& plain) const;
    void FollowAnalysisMarks(melonDS::u32 firstInsn);
    void OpenAnalysisAddr(int cpu, melonDS::u32 addr);

    WBHost* H;
    std::vector<WBPanel*> Panels;
    std::vector<std::pair<WBPanel*, NeedBanner*>> Banners;
    void UpdateBanners();

    // the game screen inside the workbench (UI_REQUIREMENTS WS-1): a melonDS window embedded in the
    // Game screen panel, with its own menus; the separate window can be hidden
    void SetEmbeddedScreen(bool on);
    void SetHideMainWindow(bool on);
    QWidget* GameHost = nullptr;
    QWidget* GamePlaceholder = nullptr;
    MainWindow* GameWin = nullptr;
    QAction* ActEmbed = nullptr;
    QAction* ActPause = nullptr;
    QAction* ActAnalyze = nullptr;
    QAction* ActClearAnalysis = nullptr;
    QAction* ActBreak = nullptr;
    QAction* ActStepInto = nullptr;
    QAction* ActStepOver = nullptr;
    QAction* ActFrame = nullptr;
    QAction* ActRunTo = nullptr;
    QAction* ActToggleBp = nullptr;
    int BpCacheGen = 0;
    int IdentTicks = 0;
    QAction* ActHideMain = nullptr;
    QToolButton* BadgeRec = nullptr;
    QToolButton* BadgeHist = nullptr;
    QToolButton* BadgeOrigin = nullptr;
    ads::CDockManager* DockMgr = nullptr;
    std::vector<ads::CDockWidget*> Docks;
    QByteArray DefaultState;
    QByteArray SavedState;               // the layout last written to the config
    static constexpr int LayoutVersion = 1;    // bumped when panels are added or renamed
    int SaveTicks = 0;
    DisasmPanel* DisasmView = nullptr;
    MemoryPanel* MemoryView = nullptr;
    ArchivePanel* ArchiveView = nullptr;
    int NavigateToArchived = -1;
    ContainersPanel* ContainersView = nullptr;
    int NavigateToContained = -1;
    QTimer TickTimer;

    int CurCpu = 0;
    melonDS::u32 CurAddr = 0x02000000;
    melonDS::u32 SelAddr = 0x02000000, SelLen = 1;
    bool LockPC = true;
    int Epoch = -1;
    std::vector<melonDS::u32> BackStack, FwdStack;
    melonDS::u32 LastPC = 0;
    bool LastWasBreak = false;

    QString BadgeStyle[3];
    QLabel* LblNotice = nullptr;
    QTimer NoticeTimer;
    QString AnalysisSummary;             // last finished run; restored when a later run is cancelled
    QString AnalysisHold;                // what the notice label shows once a transient notice fades
    bool AnalysisHoldSummary = false;
    bool AnalysisLive = false;           // a run is showing its progress line
    bool NoticeTransient = false;
    int AnalysisNoteCpu = -1;
    melonDS::u32 AnalysisNoteAddr = 0;
    QLabel* LblPause;
    QLabel* LblFrames;

    QLabel* LblSpeed;
    QLabel* LblPC;
    QLabel* LblKB;
    QAction* ActLock;
    QAction* ActRec;
    QAction* ActHist;
    QAction* ActOrigin;
    QAction* ActUndo;
    QAction* ActRedo;
};

#endif
