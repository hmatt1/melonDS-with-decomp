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

#ifndef WB_PANELS_H
#define WB_PANELS_H

#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include "WBHost.h"
#include "WBClassify.h"
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTreeWidget>

#include "WorkbenchWindow.h"
#include "WBApi.h"
#include "WBAssets.h"
#include "Workbench.h"

class QScrollBar;
class QToolButton;
namespace WBQ { struct ScanState; }

// ---------------------------------------------------------------- disassembly

struct DisasmRow
{
    enum Kind { Code, Data, LabelLine } Type = Code;
    melonDS::u32 Addr = 0, Size = 4;
    bool Thumb = false;
    bool Dim = false;               // not confirmed by the recording (inferred, override, unknown)
    WBQ::CodeKind Class = WBQ::CodeKind::Unknown;   // how this row is classified (mode gutter)
    WBQ::Evidence From = WBQ::Evidence::None;
    bool Conflict = false;
    QString Why;
    QString Bytes, Mnemonic, Operands;
    bool HasTarget = false;
    melonDS::u32 Target = 0;
    bool HasRef = false, Literal = false, LiteralValid = false;
    melonDS::u32 Ref = 0, LiteralValue = 0;
    melonDS::u32 Exec = 0, Read = 0, Write = 0;
    melonDS::u16 Flags = 0, RegMask = 0;
    bool IsCall = false, IsReturn = false;
    bool InFunction = false;
    QString LabelText;              // LabelLine
    QString Explain;
    QString Ascii;
    QString FuncName;
};


class DisasmPanel : public WBPanel
{
    Q_OBJECT
public:
    DisasmPanel(WorkbenchWindow* w);
    bool Need(WBNeed& need, QString& what) const override { need = WBNeed::Recording; what = "execution counts and recorded ARM/Thumb modes"; return true; }
    void Refresh() override;
    void OnNavigate(melonDS::u32 addr) override;
    void ShowAddress(melonDS::u32 addr, bool center);
    bool ConfirmedOnly() const { return OnlyConfirmed; }
    // a row already on screen covers addr and is still unknown bytes
    bool LookingAtUnknown(melonDS::u32 addr) const;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    bool event(QEvent* e) override;

private:
    void SetDecode(melonDS::u32 start, melonDS::u32 end, const QString& mode);
    int VisibleRows() const;
    int Chrome() const;
    void Fetch();
    void ScrollRows(int delta);
    int RowAt(int y) const;
    melonDS::u32 PCForView() const;

    QWidget* Opts = nullptr;
    QCheckBox* ShowBytes = nullptr;
    QCheckBox* ShowData = nullptr;
    QString Sticky;
    QScrollBar* Bar;
    melonDS::u32 Top = 0x02000000;
    std::vector<DisasmRow> Rows;
    int LineH = 16;
    int ColMode = 0, ColHeat = 0, ColAddr = 0, ColBytes = 0, ColMnem = 0, ColOps = 0, ColCmt = 0;
    bool OnlyConfirmed = false;     // decode only what the recording (or an override) confirms
    struct Hit { QRect R; melonDS::u32 Addr; };
    std::vector<Hit> Links;
    bool Pending = false;
    melonDS::u32 PendingTop = 0;
};

// ---------------------------------------------------------------- memory

struct MemByte
{
    melonDS::u8 Value = 0;
    bool Valid = false;
    melonDS::u8 Heat = 0;      // 0..255
    melonDS::u8 Kind = 0;      // dominant access kind: 0 none 1 exec 2 read 3 write 4 dma
    bool Tainted = false;
    WBQ::CodeKind Class = WBQ::CodeKind::Unknown;     // ARM, Thumb or data, as the Disassembly's gutter
    WBQ::Evidence From = WBQ::Evidence::None;
    bool Confirmed = false;
};

class MemoryPanel : public WBPanel
{
    Q_OBJECT
public:
    MemoryPanel(WorkbenchWindow* w);
    void Refresh() override;
    void OnNavigate(melonDS::u32 addr) override;
    void OnSelection(melonDS::u32 addr, melonDS::u32 size) override;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    bool event(QEvent* e) override;

private:
    int VisibleRows() const;
    bool ByteAtPos(const QPoint& p, melonDS::u32& addr) const;
    void Fetch();
    // the graphics file part a byte is in (File structure layer); false outside every copy
    bool FilePartAt(melonDS::u32 addr, const WBAsset::File*& file, melonDS::u32& fileOffset, const WBGfx::Part*& part) const;

    QScrollBar* Bar;
    melonDS::u32 Top = 0x02000000;
    std::vector<MemByte> Bytes;
    melonDS::u32 BytesBase = 0;
    QString RegionText;
    int LineH = 16, CharW = 8;
    int ColAddr = 0, ColHex = 0, ColAscii = 0, ColFields = 0;
    bool Dragging = false;
    melonDS::u32 DragStart = 0;
    bool Pending = false;
    // layers (the Layers button): data objects' fields, and the parts of graphics files' RAM copies
    QToolButton* Layers;
    bool ShowData = true, ShowFiles = true;
    std::shared_ptr<const WBAsset::Catalog> FileCat;
    std::vector<WBAsset::RamRun> FileRuns;
};

// ---------------------------------------------------------------- simple panels

class RegistersPanel : public WBPanel
{
    Q_OBJECT
public:
    RegistersPanel(WorkbenchWindow* w);
    void Refresh() override;
private:
    QTabWidget* Tabs;
    QTableWidget* Table[2];
    WBApi::RegisterSet Last[2];
};

class InspectorPanel : public WBPanel
{
    Q_OBJECT
public:
    InspectorPanel(WorkbenchWindow* w);
    void Refresh() override;
    void OnSelection(melonDS::u32 addr, melonDS::u32 size) override;
private:
    QTextBrowser* View;
    QString LastHtml;
};

class AnnotationsPanel : public WBPanel
{
    Q_OBJECT
public:
    AnnotationsPanel(WorkbenchWindow* w);
    void Refresh() override;
    void OnNavigate(melonDS::u32) override { if (InView->isChecked()) Refresh(); }
private:
    void ShowOverrides(bool conflictsOnly);
    QComboBox* Author;
    QComboBox* Kind;
    QLineEdit* Tag;
    QCheckBox* InView;
    QTableWidget* Table;
};

// what Recording, Write history and Data origin give you, and whether they are on (PR-6)
class GettingStartedPanel : public WBPanel
{
    Q_OBJECT
public:
    GettingStartedPanel(WorkbenchWindow* w);
    void Refresh() override;
private:
    QTextBrowser* View;
    QPushButton *RecBtn, *HistBtn, *OriginBtn;
    QString LastHtml;
};

class FunctionsPanel : public WBPanel
{
    Q_OBJECT
public:
    FunctionsPanel(WorkbenchWindow* w);
    void Refresh() override;
    void OnKnowledgeChanged() override { Rebuild(); LastRefreshMs = 0; Refresh(); }    // a new function's calls, while paused too
    bool Need(WBNeed& need, QString& what) const override;
private:
    void Rebuild();
    enum { FiltSeen = 0, FiltUnnamed, FiltMatched, FiltPartial, FiltUnmatched, FiltOutside };
    QButtonGroup* Filters = nullptr;
    QPushButton* FilterBtn[6] = {};
    QLineEdit* Filter;
    QTableWidget* Table;
    std::map<melonDS::u32, melonDS::u32> ExecCounts;
    std::vector<WBApi::CoverageRange> Unknown;
    qint64 LastRefreshMs = 0;
};

class BookmarksPanel : public WBPanel
{
    Q_OBJECT
public:
    BookmarksPanel(WorkbenchWindow* w);
    void Refresh() override;
private:
    QListWidget* List;
};

class CallStackPanel : public WBPanel
{
    Q_OBJECT
public:
    CallStackPanel(WorkbenchWindow* w);
    bool Need(WBNeed& need, QString& what) const override { need = WBNeed::Recording; what = "the call stack"; return true; }
    void Refresh() override;
private:
    QTabWidget* Tabs;
    QTreeWidget* Tree[2];
    QLabel* Note;
};

class CallGraphPanel : public WBPanel
{
    Q_OBJECT
public:
    CallGraphPanel(WorkbenchWindow* w);
    bool Need(WBNeed& need, QString& what) const override { need = WBNeed::Recording; what = "new calls (saved sessions still show)"; return true; }
    void OnNavigate(melonDS::u32 addr) override;
    void Refresh() override {}
private:
    void Populate(QTreeWidgetItem* item, melonDS::u32 func, bool callers);
    QTreeWidget* Tree;
    melonDS::u32 Root = 0xFFFFFFFF;
};

class XrefsPanel : public WBPanel
{
    Q_OBJECT
public:
    XrefsPanel(WorkbenchWindow* w);
    void OnSelection(melonDS::u32 addr, melonDS::u32 size) override;
    void Refresh() override {}
    void ShowFor(melonDS::u32 addr);
private:
    QLabel* Title;
    QTableWidget* Table;
    melonDS::u32 Addr = 0;
};

// the coverage map (WBCoverage.cpp)
class CoverageCanvas;
class CoveragePanel : public WBPanel
{
    Q_OBJECT
public:
    CoveragePanel(WorkbenchWindow* w);
    void Refresh() override;
    void OnNavigate(melonDS::u32 addr) override;
    void OnSelection(melonDS::u32 addr, melonDS::u32 size) override;
    bool Need(WBNeed& need, QString& what) const override;

private:
    friend class CoverageCanvas;
    struct View { int Scale = 6; melonDS::u32 TopRow = 0; bool Fitted = false; };   // Scale: log2 bytes per cell; < 0: a byte is 3 * 2^-Scale px
    struct CovCell { melonDS::u32 Count[4] = {0, 0, 0, 0}; melonDS::u8 Modes = 0; };   // exec, read, write, dma; 1 ARM 2 Thumb
    View& V() { return Views[Region]; }
    const View& V() const { return Views[Region]; }
    void RegionBounds(int region, melonDS::u32& base, melonDS::u32& span) const;
    void Layout();
    void Fetch();
    void UpdateInfo();
    int RulerWidth() const;
    int LegendHeight() const;
    bool CellAt(QPoint pos, melonDS::u32& addr) const;
    QRect CellRect(melonDS::u32 addr) const;
    void SetScale(int scale, QPoint anchor);
    void ZoomBy(int steps, QPoint anchor);
    void Fit();
    void ScrollRows(int delta);
    void PaintMap(QPainter& p, const QRect& area);
    void MousePress(QMouseEvent* e);
    void MouseMove(QMouseEvent* e);
    void MouseRelease(QMouseEvent* e);
    void MouseDoubleClick(QMouseEvent* e);
    void ContextMenu(QContextMenuEvent* e);
    void OpenAt(melonDS::u32 addr, const QString& panel);
    void FetchHover();
    void ShowHoverTip(QHelpEvent* e);

    QComboBox* RegionSel;
    QComboBox* KindSel;
    QComboBox* Scope;
    QSpinBox* LastN;
    QSpinBox* BytesPerPixel;
    CoverageCanvas* Map;
    QScrollBar* VBar;
    QLabel* Info;
    QTimer HoverTimer;

    View Views[6];
    int Region = 0;
    melonDS::u32 Base = 0x02000000, Span = 0x400000;
    int CellPx = 1, Cols = 1, VisRows = 1;
    melonDS::u32 CellBytes = 64, RowBytes = 64, TotalRows = 1;
    bool Pow2 = false, SyncingSpin = false, FetchPending = false;
    bool ShowFunctions = true, ShowData = true, ShowOverlays = true, ShowBookmarks = true, ShowMarkers = true, ShowRuler = true;
    bool ShowAnalysis = true;
    // archived and packed ROM files found in RAM by content (main RAM only; matched when the view moves,
    // at most every few seconds, since matching runs on the emulation thread)
    bool ShowArchived = false;
    struct ArcRun { melonDS::u32 Addr, Size; QString Name; };
    std::vector<ArcRun> ArcRuns;
    melonDS::u32 ArcStart = 0, ArcEnd = 0;
    qint64 ArcMs = 0;
    melonDS::u32 ArcFrame = 0;
    std::vector<CovCell> Cells;
    melonDS::u32 CellsStart = 0, CellsBytes = 0;
    melonDS::u64 Covered[4] = {0, 0, 0, 0};
    qint64 HoverCell = -1;
    bool Dragging = false, Panning = false;
    melonDS::u32 DragA = 0, DragB = 0;
    int PanY = 0;
    qint64 LastMs = 0;
};

class WriteHistoryPanel : public WBPanel
{
    Q_OBJECT
public:
    WriteHistoryPanel(WorkbenchWindow* w);
    void Refresh() override;
    // a saved session's history needs nothing running
    bool Need(WBNeed& need, QString& what) const override
    {
        if (SessionPick->currentIndex() > 0) return false;
        need = WBNeed::WriteHistory;
        what = "the write history of the selection";
        return true;
    }
    void OnSelection(melonDS::u32, melonDS::u32) override { HaveDetail = false; Refresh(); }
private:
    void FillSessions();
    void Show(const WBApi::WritesListResult& r, const WBApi::WritesListParams& p, int cpu, const QString& where);
    struct WriteGroup { melonDS::u32 addr = 0, count = 0, pc = 0, frame = 0; melonDS::u8 cpu = 0, size = 0; };
    void ApplyGroups(const std::vector<WriteGroup>& groups, int omitted);
    int TrimGroups(std::vector<WriteGroup>& groups, bool focusOn, melonDS::u32 focus);
    QLabel* Note;
    QComboBox* SessionPick;     // this recording, or a saved session (HC-3)
    QLineEdit* AddrFilter = nullptr;
    QCheckBox* MoreThanOnce = nullptr;
    QTableWidget* Groups = nullptr;
    QTableWidget* Table;
    QString LastKey;            // a saved session's query is run again only when it changes
    QString GroupedKey;         // session plus the more-than-once filter, so grouping is not repeated per address
    bool HaveDetail = false;
    melonDS::u32 DetailAddr = 0;
    melonDS::u32 DetailSize = 1;
    qint64 LastGroupMs = 0;
};

class DataOriginPanel : public WBPanel
{
    Q_OBJECT
public:
    DataOriginPanel(WorkbenchWindow* w);
    void Refresh() override;
    bool Need(WBNeed& need, QString& what) const override { need = WBNeed::DataOrigin; what = "the origin of new data"; return true; }
    void OnSelection(melonDS::u32, melonDS::u32) override { FocusLabel = 0; Refresh(); }
private:
    QTextBrowser* View;
    QTableWidget* Sources;
    QString LastHtml;
    melonDS::u32 FocusLabel = 0;    // a label whose whole tree is shown (a "show how" link)
};

// HTML for archive and container matches (Data origin, Inspector) and the link handler of the HTML panels
// (addr:<hex>, sel:<hex>:<hexsize>, arc:<member>, cont:<global id>)
QString ArchiveSegmentsHtml(WorkbenchWindow* W, const std::vector<WBApi::ArchiveSegment>& segs, bool compact, bool containers = false);
void FollowLink(WorkbenchWindow* W, const QUrl& u);

// Files of a game archive in the ROM (TrackMania DS's vfs.zip): list, filter, and find where a file is
// in RAM and which code loaded it
class ArchivePanel : public WBPanel
{
    Q_OBJECT
public:
    ArchivePanel(WorkbenchWindow* w);
    bool Need(WBNeed& need, QString& what) const override { need = WBNeed::Archive; what = "the files of a game archive"; return true; }
    void Refresh() override {}
    void Reload();                          // archive detected or changed
    void Locate(int member);
    QStringList Paths() const;
    int IndexOf(const QString& path) const;
signals:
    void located(int member, melonDS::u32 addr, melonDS::u32 size);   // largest RAM copy (size 0: not in RAM)
private:
    void ApplyFilter();
    QLabel* Status;
    QLineEdit* Filter;
    QTableWidget* Table;
    QTextBrowser* Result;
    int Shown = -1;
};

// NARC and SDAT files in the ROM and the files inside them: list, filter, and find where a file is
class ContainersPanel : public WBPanel
{
    Q_OBJECT
public:
    ContainersPanel(WorkbenchWindow* w);
    bool Need(WBNeed& need, QString& what) const override { need = WBNeed::Containers; what = "NARC or SDAT files"; return true; }
    void Refresh() override {}
    void Reload();                          // containers indexed or changed
    void Locate(int global);                // a file inside a container, by its global id
    QStringList Paths() const;              // "/file.narc:name" of every file inside a container
    int IndexOf(const QString& path) const; // global id, -1 if none
signals:
    void located(int global, melonDS::u32 addr, melonDS::u32 size);   // largest RAM copy (size 0: not in RAM)
private:
    void ShowContainer(int container);
    void ApplyFilter();
    QLabel* Status;
    QLineEdit* Filter;
    QTableWidget* List;                     // the containers
    QTableWidget* Table;                    // the files of the selected container
    QTextBrowser* Result;
    int Current = -1;                       // container shown in Table
    int Shown = -1;                         // global id being located
};

class OverlaysPanel : public WBPanel
{
    Q_OBJECT
public:
    OverlaysPanel(WorkbenchWindow* w);
    bool Need(WBNeed& need, QString& what) const override { need = WBNeed::Recording; what = "overlay load events"; return true; }
    void Refresh() override;
private:
    QTableWidget* Table;
    QTableWidget* Events;
    QLabel* EpochLabel;
    size_t LastEvents = (size_t)-1;
};

// ---------------------------------------------------------------- compare setup

class CodeEditor;
class QGridLayout;
class QToolButton;

// Before running a compare command for the first time (or after it or the script changed), shows the
// exact command and asks. True when it is approved.
bool ConfirmCompareCommand(QWidget* parent, WBHost* h, const WBHost::CompareJob& job);

// The Compare setup checklist at the top of the Source and diff panel: one row per check with a
// button that fixes it, shown until everything passes
class CompareChecklist : public QFrame
{
    Q_OBJECT
public:
    CompareChecklist(WorkbenchWindow* w, QWidget* parent);
    // probes again when forced, or when the function's module or compiler differs from the last check
    void Check(qint64 functionId, bool force = false);
    bool Ready() const { return IsReady; }
signals:
    void changed(bool ready);
private:
    void Fix(const QString& fix);
    WorkbenchWindow* W;
    WBHost* H;
    QLabel* Title;
    QToolButton* Collapse;
    QWidget* Body;
    QGridLayout* Grid;
    qint64 FuncId = 0;
    QString Module, Label;
    bool Checked = false;
    bool IsReady = false;
    bool Collapsed = false;
    int Gen = 0;
};

// Build > Compare setup: the project's compare script, compare.json and this computer's tool paths,
// and a test run that shows exactly what the command printed
class CompareSetupPanel : public WBPanel
{
    Q_OBJECT
public:
    CompareSetupPanel(WorkbenchWindow* w);
    void OnNavigate(melonDS::u32 addr) override;
    void Refresh() override {}
    void OnKnowledgeChanged() override { UpdatePreview(); }
    void ShowTab(int index);                // 0 script, 1 settings, 2 run
    void RunSelfTest() { Run(true); }
signals:
    void setupChanged();
private:
    void Load();
    void SaveScript();
    void ResetScript();
    void ShowChanges();
    void SaveConfig();
    void SaveLocal();
    void SaveCommand();
    void UpdatePreview();
    void ValidateConfig();
    void Run(bool selfTest);
    void ShowRun(const WBHost::CompareRun& r, bool selfTest);
    QTabWidget* Tabs;
    CodeEditor* Script;
    QLabel* ScriptState;
    CodeEditor* Config;
    QLabel* ConfigState;
    QLineEdit* Python;
    QLineEdit* Objdiff;
    QSpinBox* Timeout;
    QTableWidget* Compilers;
    QLineEdit* Command;
    QPlainTextEdit* Preview;
    QCheckBox* Record;
    QPushButton* RunBtn;
    QPushButton* SelfTestBtn;
    QPlainTextEdit* OutStdout;
    QPlainTextEdit* OutStderr;
    QTextBrowser* OutResult;
    qint64 FuncId = 0;
    bool Loaded = false;
};

// ---------------------------------------------------------------- graphics

class GfxImageView;
class QScrollArea;

// opens a graphics file (asset.list's path) in the Graphics panel
void ShowGraphics(WorkbenchWindow* W, const QString& path);

// Graphics files as images (WBAssets.h): an NCLR's palettes, an NCGR's tiles, an NSCR's map, an
// NCER's cells, TEX0 textures, or any bytes viewed as tiles. Clicking a pixel selects its bytes;
// selecting bytes marks the tiles they belong to. RAM sources update while the game runs.
class GraphicsPanel : public WBPanel
{
    Q_OBJECT
public:
    GraphicsPanel(WorkbenchWindow* w);
    void Refresh() override;
    void OnSelection(melonDS::u32 addr, melonDS::u32 size) override;
    void ShowFile(const QString& path);                          // a graphics file (asset.list's path)
    void ShowRam(melonDS::u32 addr, melonDS::u32 size);          // size 0: the file whose copy holds addr; else raw bytes
    void ClickPixel(QPoint px) { Clicked(px); }                  // as a click on the image (tests)
private:
    void ReloadFiles();
    void Load();
    void Render();
    void FillControls();
    void Clicked(QPoint px);
    bool PixelBytes(QPoint px, melonDS::u32& off, melonDS::u32& size, QRect& cell) const;
    void UpdateHighlight();
    void UpdateInfo();
    void Export(bool allTextures);
    WBAsset::RenderOptions Options() const;
    QLineEdit* Filter;
    QTreeWidget* Files;
    QLabel* Source;
    QComboBox* Part;
    QComboBox* Item;
    QComboBox* PaletteChoice;
    QSpinBox* PalNumber;
    QComboBox* Zoom;
    QCheckBox* Grid;
    QWidget* RawRow;
    QComboBox* Bpp;
    QCheckBox* Tiled;
    QSpinBox* RawWidth;
    QSpinBox* RawPalOffset;
    QScrollArea* Scroll;
    GfxImageView* View;
    QLabel* Info;
    QString SrcPath;
    melonDS::u32 SrcAddr = 0, SrcSize = 0;
    bool HaveSource = false;
    WBAsset::Asset Cur;
    std::vector<std::string> Video;
    std::vector<melonDS::u32> Copies;       // RAM copies of the file (starts)
    std::vector<melonDS::u8> LivePal;
    WBAsset::RenderInfo Drawn;
    QString ClickText;                      // what the last clicked pixel's bytes are
    QString Format;                         // the controls were filled for this format
    bool Rendering = false, Pending = false;
    qint64 LastLive = 0;
    melonDS::u32 SelA = 0, SelN = 0;
};

class SourcePanel : public WBPanel
{
    Q_OBJECT
public:
    SourcePanel(WorkbenchWindow* w);
    void OnNavigate(melonDS::u32 addr) override;
    void Refresh() override;
    void OnKnowledgeChanged() override { Load(true); }
    void CompareAt(melonDS::u32 addr);     // Build > Compare (runs the configured compare command)
    void OpenEditorAt(melonDS::u32 addr);
    void RecheckSetup() { Checklist->Check(FuncId, true); }
private:
    void Load(bool force);
    void RunCompare();
    void OpenEditor();
    bool EnsureSourceFile(bool offerCreate);
    void ShowFailure(const QString& explanation, const QString& error, const QString& details);
    CompareChecklist* Checklist;
    QLabel* Failure;
    QToolButton* FailureMore;
    QPushButton* Compare = nullptr;
    QPlainTextEdit* FailureLog;
    QLabel* Header;
    QPlainTextEdit* Source;
    QTableWidget* Diff;
    QPushButton* Editor;
    QComboBox* Attempts;
    qint64 FuncId = 0;
    QString FilePath;
    bool NavPending = false;
    melonDS::u32 PendingNav = 0;
};

class DecompilerPanel : public WBPanel
{
    Q_OBJECT
public:
    DecompilerPanel(WorkbenchWindow* w);
    void OnNavigate(melonDS::u32 addr) override;
    void OnKnowledgeChanged() override { FuncId = -1; OnNavigate(W->Cursor()); }
    void Refresh() override {}
private:
    void UseAsSource();
    QLabel* Header;
    QPlainTextEdit* Text;
    QPushButton* UseSource = nullptr;
    qint64 FuncId = -1;
    QString Draft;
};

class SearchPanel : public WBPanel
{
    Q_OBJECT
public:
    SearchPanel(WorkbenchWindow* w);
    void Refresh() override {}
private:
    void SearchBytes();
    void SearchString();
    void ScanFirst();
    void ScanNext();
    void Find(const QByteArray& pat, const QByteArray& mask);
    void ShowScan(const WBQ::ScanState& s);
    void ShowResults(const std::vector<std::pair<melonDS::u32, QString>>& r);
    QTabWidget* Tabs;
    QLineEdit* BytesEdit;
    QLineEdit* StringEdit;
    QComboBox* StringEnc;
    QComboBox* ScanType;
    QComboBox* ScanCond;
    QLineEdit* ScanValue;
    QComboBox* ScanRegion;
    QLabel* ScanInfo;
    QListWidget* Results;
    std::vector<melonDS::u32> Candidates;
    std::vector<melonDS::u32> Previous;
    int ScanSize = 4;
};

class BreakpointsPanel : public WBPanel
{
    Q_OBJECT
public:
    BreakpointsPanel(WorkbenchWindow* w);
    bool Need(WBNeed& need, QString& what) const override { need = WBNeed::Interpreter; what = "breakpoints and stepping"; return true; }
    void Refresh() override;
private:
    void AddDialog();
    QTableWidget* Table;
    QString LastKey;
    bool PendingRefresh = false;
};

class RecordingPanel : public WBPanel
{
    Q_OBJECT
public:
    RecordingPanel(WorkbenchWindow* w);
    void Refresh() override;
private:
    QPushButton* RecBtn;
    QCheckBox* Hist;
    QCheckBox* Origin;
    QSpinBox* Cap;
    QRadioButton* AllRom;
    QRadioButton* Chosen;
    QCheckBox* FollowAddr;
    QLabel* Indicator;
    QLabel* JitNote;
    QTableWidget* Sources;
    QListWidget* Log;
    QCheckBox* SkipBulk;
    QCheckBox* SkipStack;
    QCheckBox* SkipSame;
    QCheckBox* MainOnly;
    QSpinBox* HistFileMB;
    QLabel* HistNote;
    void ApplyHistoryFilter();
    QCheckBox* OptKeypad;
    QCheckBox* OptTouch;
    QCheckBox* OptRtc;
    bool Updating = false;
    size_t LastLog = 0;
};

#endif
