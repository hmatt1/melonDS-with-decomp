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

// The coverage map (UI_REQUIREMENTS CV-1..CV-4): which bytes of a memory region were executed, read,
// written or copied by DMA. Zooms from a whole region down to single bytes drawn as cells, fetches
// only what is visible, says what is under the mouse, and selects, navigates and acts on ranges.

#include <algorithm>
#include <cmath>
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "Panels.h"
#include "WBColors.h"
#include "WBHost.h"
#include "WBQueries.h"
#include "KnowledgeStore.h"
#include "NDS.h"

using namespace melonDS;
using namespace melonDS::WB;

namespace
{
QString Hex8(u32 v) { return QString("%1").arg(v, 8, 16, QChar('0')); }

const char* const KindName[4] = {"exec", "read", "write", "DMA"};
}

// the drawing surface: everything below the controls, above the legend
class CoverageCanvas : public QWidget
{
public:
    explicit CoverageCanvas(CoveragePanel* p) : QWidget(p), P(p)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
        setMinimumHeight(60);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); P->PaintMap(p, rect()); }
    void resizeEvent(QResizeEvent*) override
    {
        // the first time a region is shown, all of it
        if (!P->V().Fitted && height() > 100) { P->V().Fitted = true; P->Fit(); return; }
        P->Layout();
        P->Fetch();
    }
    void wheelEvent(QWheelEvent* e) override
    {
        int steps = e->angleDelta().y() / 120;
        if (!steps) steps = e->angleDelta().y() > 0 ? 1 : -1;
        if (e->modifiers() & Qt::ControlModifier) P->ZoomBy(steps, e->position().toPoint());
        else P->ScrollRows(-steps * 3);
    }
    void mousePressEvent(QMouseEvent* e) override { P->MousePress(e); }
    void mouseMoveEvent(QMouseEvent* e) override { P->MouseMove(e); }
    void mouseReleaseEvent(QMouseEvent* e) override { P->MouseRelease(e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { P->MouseDoubleClick(e); }
    void contextMenuEvent(QContextMenuEvent* e) override { P->ContextMenu(e); }
    void leaveEvent(QEvent*) override { P->HoverCell = -1; update(); }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ToolTip) { P->ShowHoverTip(static_cast<QHelpEvent*>(e)); return true; }
        return QWidget::event(e);
    }

private:
    CoveragePanel* P;
};

bool CoveragePanel::Need(WBNeed& need, QString& what) const
{
    if (Scope->currentIndex() == 1) return false;     // all time: saved sessions
    need = WBNeed::Recording;
    what = "coverage of this session";
    return true;
}

CoveragePanel::CoveragePanel(WorkbenchWindow* w) : WBPanel(w)
{
    RegionSel = new QComboBox(this);
    RegionSel->addItems({"main RAM", "ITCM", "DTCM", "shared WRAM", "ARM7 WRAM", "VRAM (LCDC)"});
    KindSel = new QComboBox(this);
    KindSel->addItems({"exec", "read", "write", "dma", "combined"});
    KindSel->setItemData(4, "Each cell in the colour of its most frequent access, relative to that access's busiest cell", Qt::ToolTipRole);
    Scope = new QComboBox(this);
    Scope->addItems({"this session", "all time", "last N frames"});
    LastN = new QSpinBox(this);
    LastN->setRange(1, 1000000);
    LastN->setValue(60);
    LastN->setPrefix("N=");
    BytesPerPixel = new QSpinBox(this);
    BytesPerPixel->setRange(1, 1 << 20);
    BytesPerPixel->setSuffix(" B/cell");
    BytesPerPixel->setToolTip("Bytes per cell; zoom with Ctrl+wheel, + and -");
    auto tool = [this](const QString& text, const QString& tip)
    {
        QToolButton* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        return b;
    };
    QToolButton* zin = tool("+", "Zoom in (Ctrl+wheel)");
    QToolButton* zout = tool("-", "Zoom out (Ctrl+wheel)");
    QToolButton* fit = tool("Fit", "The whole region");
    QToolButton* layers = tool("Layers", "What is drawn over the map");
    layers->setPopupMode(QToolButton::InstantPopup);
    QMenu* lm = new QMenu(layers);
    auto layer = [this, lm](const QString& name, bool& flag)
    {
        QAction* a = lm->addAction(name, this, [this, &flag](bool on) { flag = on; Map->update(); Fetch(); });
        a->setCheckable(true);
        a->setChecked(flag);
    };
    if (qEnvironmentVariableIsSet("DSWB_COVERAGE_ARCHIVED")) ShowArchived = true;      // test hook (screenshots)
    layer("Functions", ShowFunctions);
    layer("From analysis", ShowAnalysis);
    layer("Data objects", ShowData);
    layer("Loaded overlays", ShowOverlays);
    layer("Bookmarks", ShowBookmarks);
    layer("Archived files (slower)", ShowArchived);
    layer("PC and cursor", ShowMarkers);
    layer("Address ruler", ShowRuler);
    QAction* p2 = lm->addAction("Power-of-two rows", this, [this](bool on) { Pow2 = on; Layout(); Fetch(); });
    p2->setCheckable(true);
    p2->setChecked(Pow2);
    p2->setToolTip("Row widths a power of two bytes, so addresses line up");
    layers->setMenu(lm);

    Map = new CoverageCanvas(this);
    VBar = new QScrollBar(Qt::Vertical, this);
    Info = new QLabel(this);
    Info->setTextFormat(Qt::RichText);

    QHBoxLayout* top = new QHBoxLayout();
    top->addWidget(RegionSel);
    top->addWidget(KindSel);
    top->addWidget(Scope);
    top->addWidget(LastN);
    top->addWidget(BytesPerPixel);
    top->addWidget(zin);
    top->addWidget(zout);
    top->addWidget(fit);
    top->addWidget(layers);
    top->addStretch();
    QHBoxLayout* mid = new QHBoxLayout();
    mid->setSpacing(0);
    mid->addWidget(Map, 1);
    mid->addWidget(VBar);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addLayout(top);
    v->addLayout(mid, 1);
    v->addWidget(Info);

    HoverTimer.setSingleShot(true);
    HoverTimer.setInterval(120);
    connect(&HoverTimer, &QTimer::timeout, this, [this]() { FetchHover(); });

    auto refetch = [this]() { LastMs = 0; Fetch(); };
    connect(RegionSel, &QComboBox::currentIndexChanged, this, [this, refetch](int i)
    {
        Region = i;        // each region keeps its own zoom and position
        Cells.clear();
        if (!V().Fitted && Map->height() > 100) { V().Fitted = true; Fit(); return; }
        Layout();
        refetch();
    });
    connect(KindSel, &QComboBox::currentIndexChanged, this, [this]() { Map->update(); UpdateInfo(); });
    connect(Scope, &QComboBox::currentIndexChanged, this, [this, refetch]() { if (Scope->currentIndex() == 1) H->LoadAllTimeSessions(); refetch(); });
    connect(LastN, &QSpinBox::valueChanged, this, refetch);
    connect(BytesPerPixel, &QSpinBox::valueChanged, this, [this](int v)
    {
        if (SyncingSpin) return;
        // the nearest power of two, as a zoom step
        int s = 0;
        while ((1 << (s + 1)) <= v && s < 20) s++;
        SetScale(s, QPoint(Map->width() / 2, Map->height() / 2));
    });
    connect(zin, &QToolButton::clicked, this, [this]() { ZoomBy(1, QPoint(Map->width() / 2, Map->height() / 2)); });
    connect(zout, &QToolButton::clicked, this, [this]() { ZoomBy(-1, QPoint(Map->width() / 2, Map->height() / 2)); });
    connect(fit, &QToolButton::clicked, this, [this]() { Fit(); });
    connect(VBar, &QScrollBar::valueChanged, this, [this](int v)
    {
        if (V().TopRow == (u32)v) return;
        V().TopRow = (u32)v;
        Fetch();
    });
    for (View& vw : Views) vw.Scale = 6;      // 64 bytes per cell to start
    RegionBounds(0, Base, Span);
}

// ---------------------------------------------------------------- geometry

void CoveragePanel::RegionBounds(int ri, u32& base, u32& span) const
{
    NDS* nds = H->Core().GetNDS();
    switch (ri)
    {
    case 0: base = 0x02000000; span = nds ? nds->MainRAMMask + 1 : 0x400000; break;
    case 1: base = 0x01FF8000; span = 0x8000; break;
    case 2: base = nds && nds->ARM9.DTCMBase != 0xFFFFFFFF ? nds->ARM9.DTCMBase : 0x027E0000; span = 0x4000; break;
    case 3: base = 0x03000000; span = 0x8000; break;
    case 4: base = 0x03800000; span = 0x10000; break;
    default: base = 0x06800000; span = 0xA4000; break;
    }
}

int CoveragePanel::RulerWidth() const
{
    if (!ShowRuler) return 0;
    return Map->fontMetrics().horizontalAdvance("00000000") + 8;
}

int CoveragePanel::LegendHeight() const { return 0; }

void CoveragePanel::Layout()
{
    RegionBounds(Region, Base, Span);
    View& v = V();
    // cells are never smaller than 3 px, so sparse coverage is still visible; past one byte per cell they grow
    CellPx = v.Scale < 0 ? (3 << -v.Scale) : 3;
    CellBytes = v.Scale >= 0 ? (1u << v.Scale) : 1;
    int w = std::max(1, Map->width() - RulerWidth());
    Cols = std::max(1, w / CellPx);
    if (Pow2)
    {
        int c = 1;
        while (c * 2 <= Cols) c *= 2;
        Cols = c;
    }
    RowBytes = (u32)Cols * CellBytes;
    TotalRows = (Span + RowBytes - 1) / RowBytes;
    VisRows = std::max(1, (Map->height() - LegendHeight()) / CellPx);
    u32 maxTop = TotalRows > (u32)VisRows ? TotalRows - (u32)VisRows : 0;
    if (v.TopRow > maxTop) v.TopRow = maxTop;
    VBar->blockSignals(true);
    VBar->setRange(0, (int)maxTop);
    VBar->setPageStep(VisRows);
    VBar->setValue((int)v.TopRow);
    VBar->blockSignals(false);
    SyncingSpin = true;
    BytesPerPixel->setValue((int)CellBytes);
    BytesPerPixel->setSuffix(CellPx > 1 ? QString(" B/cell, %1 px").arg(CellPx) : QString(" B/cell"));
    SyncingSpin = false;
}

bool CoveragePanel::CellAt(QPoint pos, u32& addr) const
{
    int x = pos.x() - RulerWidth();
    if (x < 0 || pos.y() < 0 || pos.y() >= VisRows * CellPx) return false;
    int col = x / CellPx, row = pos.y() / CellPx;
    if (col >= Cols) return false;
    u64 a = (u64)Base + ((u64)V().TopRow + (u64)row) * RowBytes + (u64)col * CellBytes;
    if (a >= (u64)Base + Span) return false;
    addr = (u32)a;
    return true;
}

QRect CoveragePanel::CellRect(u32 addr) const
{
    u32 off = addr - Base;
    int row = (int)(off / RowBytes) - (int)V().TopRow;
    int col = (int)((off % RowBytes) / CellBytes);
    return QRect(RulerWidth() + col * CellPx, row * CellPx, CellPx, CellPx);
}

void CoveragePanel::SetScale(int scale, QPoint anchor)
{
    scale = std::clamp(scale, -5, 20);
    // keep the address under the anchor where it is
    u32 a = 0;
    bool have = CellAt(anchor, a);
    V().Scale = scale;
    Layout();
    if (have)
    {
        u32 row = (a - Base) / RowBytes;
        int y = anchor.y() / CellPx;
        V().TopRow = row > (u32)y ? row - (u32)y : 0;
        Layout();
    }
    Cells.clear();
    Fetch();
}

void CoveragePanel::ZoomBy(int steps, QPoint anchor) { SetScale(V().Scale - steps, anchor); }

void CoveragePanel::Fit()
{
    // the largest cells that still show the whole region
    for (int s = -5; s <= 20; s++)
    {
        V().Scale = s;
        V().TopRow = 0;
        Layout();
        if (TotalRows <= (u32)VisRows) break;
    }
    Cells.clear();
    Fetch();
}

void CoveragePanel::ScrollRows(int delta)
{
    View& v = V();
    s64 t = (s64)v.TopRow + delta;
    s64 maxTop = TotalRows > (u32)VisRows ? TotalRows - VisRows : 0;
    v.TopRow = (u32)std::clamp<s64>(t, 0, maxTop);
    Layout();
    Fetch();
}

// ---------------------------------------------------------------- data

void CoveragePanel::Refresh()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - LastMs < 1000) return;
    Fetch();
}

void CoveragePanel::OnNavigate(u32) { Map->update(); }
void CoveragePanel::OnSelection(u32, u32) { Map->update(); }

void CoveragePanel::Fetch()
{
    if (Busy) { FetchPending = true; return; }
    Layout();
    Busy = true;
    FetchPending = false;
    LastMs = QDateTime::currentMSecsSinceEpoch();
    u32 start = Base + V().TopRow * RowBytes;
    u32 end = (u32)std::min<u64>((u64)start + (u64)(VisRows + 1) * RowBytes, (u64)Base + Span);
    int ri = Region;
    int scope = Scope->currentIndex();
    u32 lastN = (u32)LastN->value();
    u32 cb = CellBytes;
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    u32 frame = H->Status().Frame;
    bool wantArc = ShowArchived && ri == 0 && (start != ArcStart || end != ArcEnd || (frame != ArcFrame && nowMs - ArcMs > 1000));
    struct Res { std::vector<CovCell> Cells; u32 Start, CellBytes; u64 Covered[4]; bool HaveArc = false; std::vector<ArcRun> Arc; };
    H->Query<Res>([this, ri, scope, lastN, start, end, cb, wantArc]()
    {
        Res r;
        if (wantArc)
        {
            // which archived (or packed) ROM files the visible bytes equal
            Workbench& w = H->Core();
            NDS* nds = w.GetNDS();
            r.HaveArc = true;
            if (nds && nds->MainRAM && w.SubFileResolve)
            {
                u32 len = nds->MainRAMMask + 1;
                std::vector<SubFileRun> runs;
                for (u32 a = start; a < end; )
                {
                    u32 chunk = std::min<u32>(0x10000, end - a);
                    runs.clear();
                    w.SubFileResolve(nds->MainRAM, 0x02000000, len, a, chunk, 3, runs);
                    u32 next = a + chunk;
                    for (const SubFileRun& s : runs)
                    {
                        if (!s.Count) continue;
                        QString name;
                        if (s.Members[0] && w.SubFileName) name = QString::fromStdString(w.SubFileName(s.FileId, s.Members[0]));
                        else if (const RomFile* f = w.Rom.FileById(s.FileId)) name = QString::fromStdString(f->Path);
                        if (s.Count > 1) name += QString(" (and %1 identical)").arg(s.Count - 1);
                        r.Arc.push_back({s.Addr, s.Size, name});
                        next = std::max(next, s.Addr + s.Size);
                    }
                    a = next;
                }
            }
        }
        r.Start = start;
        r.CellBytes = cb;
        for (int k = 0; k < 4; k++) r.Covered[k] = 0;
        Workbench& wb = H->Core();
        u32 cpu = ri == 4 ? 1 : 0;
        const MergedSessions* all = scope == 1 ? H->AllTime() : nullptr;
        u32 now = wb.Frame();
        u32 n = end > start ? (end - start + cb - 1) / cb : 0;
        r.Cells.assign(n, CovCell());
        for (u32 c = 0; c < n; c++)
        {
            CovCell& cell = r.Cells[c];
            u32 cs = start + c * cb, ce = std::min(cs + cb, end);
            for (u32 a = cs; a < ce; )
            {
                PhysAddr pa = wb.Translate(cpu, a);
                RecordPage* p = pa.Recordable() ? wb.Rec.Store().Get(pa.Region, pa.Offset >> PageShift) : nullptr;
                RecordPage* q = (all && pa.Recordable()) ? all->Records.Get(pa.Region, pa.Offset >> PageShift) : nullptr;
                if (!p && !q)
                {
                    a += PageSize - (a & (PageSize - 1));
                    continue;
                }
                u32 idx = pa.Offset & (PageSize - 1);
                u32 cnt[4] = {0, 0, 0, 0};
                auto add = [&](const ByteRecord& br)
                {
                    cnt[0] += br.exec_count;
                    cnt[1] += br.read_count;
                    cnt[2] += br.write_count;
                    cnt[3] += br.dma_read_count + br.dma_write_count;
                    if (br.exec_count) cell.Modes |= (br.flags & ByteFlag::Thumb) ? 2 : 1;
                };
                if (p) add(p->Rec[idx]);
                if (q) add(q->Rec[idx]);
                for (int k = 0; k < 4; k++)
                {
                    // last N frames: only blocks that saw that access recently
                    if (scope == 2 && (!p || !p->BlockFrame[idx >> BlockShift][k] || now - p->BlockFrame[idx >> BlockShift][k] > lastN)) cnt[k] = 0;
                    if (cnt[k]) r.Covered[k]++;
                    cell.Count[k] = std::max(cell.Count[k], cnt[k]);
                }
                a++;
            }
        }
        return r;
    }, this, [this](Res r)
    {
        Busy = false;
        Cells = std::move(r.Cells);
        if (r.HaveArc)
        {
            ArcRuns = std::move(r.Arc);
            ArcStart = r.Start;
            ArcEnd = r.Start + (u32)Cells.size() * r.CellBytes;
            ArcMs = QDateTime::currentMSecsSinceEpoch();
            ArcFrame = H->Status().Frame;
        }
        CellsStart = r.Start;
        CellsBytes = r.CellBytes;
        for (int k = 0; k < 4; k++) Covered[k] = r.Covered[k];
        Map->update();
        UpdateInfo();
        if (FetchPending) Fetch();
    });
}

void CoveragePanel::UpdateInfo()
{
    int k = std::min(KindSel->currentIndex(), 3);
    QString what = KindSel->currentIndex() == 4 ? QString("exec %1, read %2, write %3, DMA %4 bytes")
                                                      .arg(Covered[0]).arg(Covered[1]).arg(Covered[2]).arg(Covered[3])
                                                : QString("%1 bytes %2").arg(Covered[k]).arg(KindName[k]);
    Info->setText(QString("%1..%2 shown: %3. Ctrl+wheel zooms, drag selects, middle-drag pans, double-click opens.")
                  .arg(Hex8(Base + V().TopRow * RowBytes))
                  .arg(Hex8((u32)std::min<u64>((u64)Base + (u64)(V().TopRow + VisRows) * RowBytes, (u64)Base + Span) - 1))
                  .arg(what));
}

// ---------------------------------------------------------------- drawing

void CoveragePanel::PaintMap(QPainter& p, const QRect& area)
{
    QColor bg = WBColor::Coverage();
    p.fillRect(area, bg);
    int rw = RulerWidth();
    int mapH = VisRows * CellPx;
    int kind = KindSel->currentIndex();

    // cells: one image pixel per cell, scaled to the cell size
    if (!Cells.empty() && CellsBytes == CellBytes)
    {
        QImage img(Cols, VisRows + 1, QImage::Format_RGB32);
        img.fill(bg);
        QColor touched = WBColor::Touched();
        QRgb touch = qRgb(touched.red(), touched.green(), touched.blue());
        u32 top = Base + V().TopRow * RowBytes;
        for (size_t i = 0; i < Cells.size(); i++)
        {
            u32 a = CellsStart + (u32)i * CellsBytes;
            if (a < top) continue;
            u32 off = a - top;
            int row = (int)(off / RowBytes), col = (int)((off % RowBytes) / CellBytes);
            if (row > VisRows || col >= Cols) continue;
            const CovCell& c = Cells[i];
            bool hit = false;
            if (kind == 4)
            {
                for (int j = 0; j < 4; j++) if (c.Count[j]) { hit = true; break; }
            }
            else
                hit = c.Count[kind] != 0;
            if (hit) img.setPixel(col, row, touch);
        }
        p.drawImage(QRect(rw, 0, Cols * CellPx, (VisRows + 1) * CellPx), img);
    }

    u32 viewStart = Base + V().TopRow * RowBytes;
    u32 viewEnd = (u32)std::min<u64>((u64)viewStart + (u64)(VisRows + 1) * RowBytes, (u64)Base + Span);
    // a range drawn as outlines along the rows it covers
    auto outline = [&](u32 s, u32 e, const QColor& c, bool fill, Qt::PenStyle style = Qt::SolidLine)
    {
        s = std::max(s, viewStart);
        e = std::min(e, viewEnd);
        if (s >= e) return;
        u32 a = s;
        while (a < e)
        {
            u32 rowEnd = viewStart + ((a - viewStart) / RowBytes + 1) * RowBytes;
            u32 b = std::min(e, rowEnd);
            QRect r1 = CellRect(a), r2 = CellRect(b - 1);
            QRect r(r1.left(), r1.top(), r2.right() - r1.left() + 1, CellPx);
            if (fill) p.fillRect(r, c);
            else
            {
                p.setPen(QPen(c, 1, style));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(0, 0, -1, -1));
            }
            a = b;
        }
    };
    WBKB::KnowledgeStore& kb = W->KB();
    int cpu = Region == 4 ? 1 : 0;
    if (ShowOverlays)
        for (const WBKB::Module& m : kb.Modules())
            if (m.Kind == "overlay" && m.CpuNum() == cpu && m.End() > viewStart && m.Load < viewEnd && W->OverlayContext(m.Load) == m.OverlayId)
                outline(m.Load, m.End(), WBColor::Overlay(), true);
    if (ShowAnalysis)
    {
        const std::vector<WBQ::AnalysisHints::Range>& guessed = H->AnalysisAll(cpu);
        auto it = std::lower_bound(guessed.begin(), guessed.end(), viewStart,
                                   [](const WBQ::AnalysisHints::Range& r, u32 a) { return r.Start < a; });
        if (it != guessed.begin()) --it;
        while (it != guessed.end() && it->Start < viewEnd)
        {
            u32 s = it->Start, e = it->End;
            auto next = it + 1;
            while (next != guessed.end() && next->Start <= e && next->Start < viewEnd) { e = std::max(e, next->End); ++next; }
            outline(s, e, WBColor::Amber(), false, Qt::DashLine);
            it = next;
        }
    }
    if (ShowFunctions)
        for (const WBKB::Function& f : kb.Functions())
        {
            const WBKB::Module* m = kb.ModuleById(f.ModuleId);
            if (!m || m->CpuNum() != cpu || f.Addr >= viewEnd || f.Addr + std::max<u32>(f.Size, 2) <= viewStart) continue;
            if (m->Kind == "overlay" && W->OverlayContext(f.Addr) != m->OverlayId) continue;
            outline(f.Addr, f.Addr + std::max<u32>(f.Size, 2), WBColor::Decode(), false);
        }
    if (ShowData)
        for (const WBKB::DataObject& d : kb.DataObjects())
            if (d.Addr < viewEnd && d.Addr + std::max<u32>(d.Size, 1) > viewStart)
                outline(d.Addr, d.Addr + std::max<u32>(d.Size, 1), WBColor::Muted(), false);
    if (ShowArchived && Region == 0)
        for (const ArcRun& r : ArcRuns)
            outline(r.Addr, r.Addr + r.Size, WBColor::Muted(), false, Qt::DashLine);
    if (ShowBookmarks)
        for (const WBKB::Bookmark& b : kb.Bookmarks())
            if (b.Cpu == cpu && b.Address >= viewStart && b.Address < viewEnd)
            {
                QRect r = CellRect(b.Address);
                QPolygon tri;
                tri << QPoint(r.left(), r.top()) << QPoint(r.left() + 6, r.top()) << QPoint(r.left(), r.top() + 6);
                p.setPen(Qt::NoPen);
                p.setBrush(WBColor::Blue());
                p.drawPolygon(tri);
            }
    if (ShowMarkers)
    {
        // the selection, the cursor and the PC
        u32 sel = W->SelStart(), seln = std::max<u32>(W->SelSize(), 1);
        if (Dragging) { sel = std::min(DragA, DragB); seln = std::max(DragA, DragB) + CellBytes - sel; }
        outline(sel, sel + seln, WBColor::SelectionFill(), true);
        if (W->Cursor() >= viewStart && W->Cursor() < viewEnd)
        {
            p.setPen(QPen(WBColor::Blue(), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(CellRect(W->Cursor()).adjusted(0, 0, -1, -1));
        }
        WBStatus st = H->Status();
        u32 pc = st.PC[cpu];
        if (st.EmuState != WBStatus::NoGame && pc >= viewStart && pc < viewEnd)
        {
            p.setPen(QPen(WBColor::Blue(), 2));
            p.drawRect(CellRect(pc).adjusted(0, 0, -1, -1));
        }
    }
    if (HoverCell >= 0)
    {
        u32 a = (u32)HoverCell;
        if (a >= viewStart && a < viewEnd)
        {
            p.setPen(WBColor::Text());
            p.setBrush(Qt::NoBrush);
            p.drawRect(CellRect(a).adjusted(0, 0, -1, -1));
        }
    }

    // address ruler: every row with room for its text
    QFontMetrics fm = Map->fontMetrics();
    if (ShowRuler)
    {
        p.fillRect(0, 0, rw, mapH, WBColor::Dark());
        p.setPen(WBColor::Muted());
        int every = std::max(1, (fm.height() + CellPx - 1) / CellPx);
        for (int r = 0; r < VisRows; r += every)
        {
            u64 a = (u64)Base + ((u64)V().TopRow + r) * RowBytes;
            if (a >= (u64)Base + Span) break;
            p.drawText(QRect(2, r * CellPx, rw - 4, fm.height()), Qt::AlignRight | Qt::AlignTop, Hex8((u32)a));
        }
    }

}

// ---------------------------------------------------------------- mouse

void CoveragePanel::MousePress(QMouseEvent* e)
{
    u32 a;
    if (e->button() == Qt::MiddleButton)
    {
        Panning = true;
        PanY = e->pos().y();
        return;
    }
    if (e->button() != Qt::LeftButton || !CellAt(e->pos(), a)) return;
    Dragging = true;
    DragA = DragB = a;
    Map->update();
}

void CoveragePanel::MouseMove(QMouseEvent* e)
{
    if (Panning)
    {
        int rows = (PanY - e->pos().y()) / CellPx;
        if (rows) { PanY -= rows * CellPx; ScrollRows(rows); }
        return;
    }
    u32 a;
    if (!CellAt(e->pos(), a)) { HoverCell = -1; Map->update(); return; }
    if (Dragging) { DragB = a; Map->update(); return; }
    if (HoverCell != (s64)a)
    {
        HoverCell = a;
        Map->update();
        QToolTip::hideText();
        HoverTimer.start();
    }
}

void CoveragePanel::MouseRelease(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton) { Panning = false; return; }
    if (!Dragging) return;
    Dragging = false;
    // a click selects the cell, a drag the cells it covered; the Inspector, Data origin and Write history follow
    u32 s = std::min(DragA, DragB), en = std::max(DragA, DragB) + CellBytes;
    if (Region == 4 && W->Cpu() != 1) W->SetCpu(1);
    W->SetSelection(s, en - s);
    Map->update();
}

void CoveragePanel::MouseDoubleClick(QMouseEvent* e)
{
    u32 a;
    if (!CellAt(e->pos(), a)) return;
    OpenAt(a, KindSel->currentIndex() == 0 ? "dock_disasm" : "dock_memory");
}

void CoveragePanel::OpenAt(u32 a, const QString& panel)
{
    if (Region == 4 && W->Cpu() != 1) W->SetCpu(1);
    W->SetLockToPC(false);
    W->GoTo(a);
    W->ShowPanel(panel);
}

void CoveragePanel::ContextMenu(QContextMenuEvent* e)
{
    u32 a;
    if (!CellAt(e->pos(), a)) return;
    // the selection when the cell is in it, else the cell
    u32 s = a, n = CellBytes;
    if (W->SelSize() > 1 && a >= W->SelStart() && a < W->SelStart() + W->SelSize()) { s = W->SelStart(); n = W->SelSize(); }
    QMenu m(this);
    int cpu = Region == 4 ? 1 : -1;      // the ARM7 region on the ARM7, others on the current CPU
    m.addAction("Define function here", [=]() { if (cpu >= 0 && W->Cpu() != cpu) W->SetCpu(cpu); W->DefineFunctionAt(s); });
    m.addAction("Define data here", [=]() { if (cpu >= 0 && W->Cpu() != cpu) W->SetCpu(cpu); W->DefineDataAt(s, n); });
    m.addAction("Locate archived file...", [=]() { W->PromptLocateArchived(); });
    m.addSeparator();
    W->AddressActions(m, cpu, s, n);
    m.exec(e->globalPos());
}

// ---------------------------------------------------------------- hover

void CoveragePanel::FetchHover()
{
    if (HoverCell < 0) return;
    u32 a = (u32)HoverCell;
    u32 n = CellBytes;
    int cpu = Region == 4 ? 1 : 0;
    // knowledge-store names on this thread, records on the emulation thread
    int ov = W->OverlayContext(a);
    WBQ::KbHints hints = WBQ::KbHints::Collect(W->KB(), cpu, a, a + n, ov);
    WBQ::AnalysisHints guessed = H->AnalysisNear(cpu, a, a + n);
    QString names;
    if (const WBKB::Function* f = W->KB().FunctionAt(cpu, a, ov)) names += "function " + W->KB().NameAt(cpu, f->Addr, ov) + "<br>";
    if (const WBKB::DataObject* d = W->KB().DataObjectAt(cpu, a, ov)) names += "data " + (d->Name.isEmpty() ? d->TypeName : d->Name) + "<br>";
    if (const WBKB::Module* m = W->KB().ModuleFor(cpu, a, ov)) names += "module " + m->Name + "<br>";
    if (ShowArchived && Region == 0)
        for (const ArcRun& r : ArcRuns)
            if (a + n > r.Addr && a < r.Addr + r.Size) { names += "equals " + r.Name.toHtmlEscaped() + "<br>"; break; }
    WBApi::MetadataParams p;
    p.addr = a;
    p.size = n;
    p.cpu = cpu ? "arm7" : "arm9";
    p.group = "block";
    p.block_size = n;
    p.scope = Scope->currentIndex() == 1 ? "all" : "session";
    QPoint at = QCursor::pos();
    H->Query<WBApi::MetadataResult>([this, p, hints, guessed]()
    {
        WBApi::MetadataResult r;
        QString e;
        WBQ::Metadata(H->Core(), p.scope == "all" ? H->AllTime() : nullptr, p, r, e, &hints, &guessed);
        return r;
    }, this, [this, a, n, names, at, all = p.scope == "all"](WBApi::MetadataResult r)
    {
        if (HoverCell != (s64)a || r.entries.empty() || !Map->underMouse()) return;
        const WBApi::MetadataEntry& e = r.entries[0];
        QString t = QString("<b>%1</b>%2 &nbsp; <span style='color:%MUTED%'>%3</span><br>")
                        .arg(Hex8(a), n > 1 ? QString("..%1").arg(Hex8(a + n - 1)) : QString(), e.region);
        t += names;
        t += QString("exec %1, read %2, write %3, DMA %4<br>").arg(e.exec).arg(e.read).arg(e.write).arg(e.dma_read + e.dma_write);
        if (!e.mode.isEmpty()) t += "mode " + e.mode + "<br>";
        t += QString("decoded as %1 (%2)<br>").arg(e.classification, e.confidence);
        if (!all && e.first_seen)
        {
            // the first frame of each kind of access, earliest byte of the cell
            QStringList firsts;
            for (const QString& k : e.first_kinds)
            {
                u32 f = k == "exec" ? e.first_exec : k == "read" ? e.first_read : k == "write" ? e.first_write : e.first_dma;
                firsts << QString("%1 %2").arg(k == "dma" ? "DMA" : k).arg(f);
            }
            t += "first: " + firsts.join(", ") + "<br>";
        }
        if (e.last_write_frame) t += QString("last write frame %1 by %2 %3<br>").arg(e.last_write_frame).arg(e.last_writer_cpu, Hex8(e.last_writer_pc));
        QStringList fl;
        for (const QString& f : e.flags) fl << f;
        if (!fl.isEmpty()) t += "<span style='color:%MUTED%'>" + fl.join(", ") + "</span>";
        QToolTip::showText(at, WBColor::ApplyHtmlColors(t), Map);
    });
}

void CoveragePanel::ShowHoverTip(QHelpEvent* e)
{
    // the tooltip comes from FetchHover once the records arrive
    u32 a;
    if (CellAt(e->pos(), a) && HoverCell != (s64)a)
    {
        HoverCell = a;
        HoverTimer.start();
    }
}
