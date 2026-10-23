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

#include <set>
#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "WBColors.h"

#include "Panels.h"
#include "Dialogs.h"
#include "WBHost.h"
#include "WBQueries.h"
#include "NDS.h"

using namespace melonDS;
using namespace melonDS::WB;

static QString Hex8(u32 v) { return QString("%1").arg(v, 8, 16, QChar('0')); }
static QString ByteCount(u32 n) { return n == 1 ? QString("1 byte") : QString("%1 bytes").arg(n); }

QTableWidget* MakeTable(QWidget* parent, const QStringList& headers)
{
    QTableWidget* t = new QTableWidget(parent);
    t->setColumnCount(headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(18);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setWordWrap(false);
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    t->setFont(f);
    // until rows arrive, columns as wide as their headers (not 100 px each, which cuts the last ones off)
    t->horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);
    for (int c = 0; c < t->columnCount() - 1; c++) t->setColumnWidth(c, std::max(t->columnWidth(c) + 12, 40));
    return t;
}

// size columns to their contents (for short tables; long ones keep their widths)
void FitColumns(QTableWidget* t)
{
    if (t->rowCount() > 1000) return;
    t->resizeColumnsToContents();
    for (int c = 0; c < t->columnCount() - 1; c++)
        t->setColumnWidth(c, std::min(std::max(t->columnWidth(c) + 12, 40), 320));
}

QTableWidgetItem* Cell(const QString& text, const QVariant& data = QVariant())
{
    QTableWidgetItem* it = new QTableWidgetItem(text);
    if (data.isValid()) it->setData(Qt::UserRole, data);
    return it;
}

static bool LooksLikeAddress(u32 v)
{
    u32 hi = v >> 24;
    return hi == 0x01 || hi == 0x02 || hi == 0x03 || hi == 0x04 || hi == 0x05 || hi == 0x06 || hi == 0x07 || (v >> 16) == 0xFFFF;
}

// ================================================================ registers

RegistersPanel::RegistersPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Tabs = new QTabWidget(this);
    static const QStringList names = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc", "cpsr", "spsr", "mode"};
    for (int c = 0; c < 2; c++)
    {
        Table[c] = MakeTable(this, {"Reg", "Value", "Annotation"});
        Table[c]->setRowCount(names.size());
        for (int i = 0; i < names.size(); i++) Table[c]->setItem(i, 0, Cell(names[i]));
        Table[c]->setColumnWidth(0, 44);
        Table[c]->setColumnWidth(1, 90);
        Tabs->addTab(Table[c], c ? "ARM7" : "ARM9");
        connect(Table[c], &QTableWidget::cellDoubleClicked, this, [this, c](int row, int col)
        {
            if (row >= 16 || col != 1) return;
            u32 v = Last[c].r.size() > (size_t)row ? Last[c].r[row] : 0;
            if (LooksLikeAddress(v)) { if (W->Cpu() != c) W->SetCpu(c); W->GoTo(v & (row == 15 ? ~1u : ~0u)); }
        });
        Table[c]->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(Table[c], &QWidget::customContextMenuRequested, this, [this, c](const QPoint& pos)
        {
            int row = Table[c]->rowAt(pos.y());
            if (row < 0 || row >= 16) return;
            u32 v = Last[c].r.size() > (size_t)row ? Last[c].r[row] : 0;
            QMenu m(this);
            if (LooksLikeAddress(v)) W->AddressActions(*m.addMenu(QString("At %1").arg(Hex8(v))), c, v & (row == 15 ? ~1u : ~0u), 4);
            m.addAction("Track this value (data origin)", [=]() { if (W->Cpu() != c) W->SetCpu(c); W->TrackRegister(row); });
            m.addAction("Annotate register in this function", [=]()
            {
                if (W->Cpu() != c) W->SetCpu(c);
                WBKB::Annotation a;
                a.TargetKind = "register";
                static const QStringList regs = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
                a.Register = regs[row];
                u32 pc = Last[c].r.size() > 15 ? Last[c].r[15] : 0;
                const WBKB::Function* f = W->KB().FunctionAt(c, pc, W->OverlayContext(pc));
                if (!f) { W->ShowError("Register annotation", "The PC is not inside a known function. Define the function first (P)."); return; }
                a.FunctionId = f->Id;
                a.HasAddr = true;
                a.Addr = pc;
                a.EndAddr = pc + 4;
                AnnotationDialog d(W, nullptr, pc, 1);
                d.setWindowTitle(QString("Annotate %1 in %2").arg(a.Register, W->KB().NameAt(c, f->Addr, W->OverlayContext(f->Addr))));
                // preset the register target in the dialog
                d.findChild<QComboBox*>()->setCurrentText("register");
                for (QComboBox* cb : d.findChildren<QComboBox*>())
                    if (cb->findText("r0") >= 0) cb->setCurrentText(a.Register);
                d.exec();
            });
            m.exec(Table[c]->viewport()->mapToGlobal(pos));
        });
    }
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(Tabs);
    connect(w, &WorkbenchWindow::cpuChanged, this, [this](int c) { Tabs->setCurrentIndex(c); });
}

void RegistersPanel::Refresh()
{
    if (Busy) return;
    Busy = true;
    H->Query<std::pair<WBApi::RegisterSet, WBApi::RegisterSet>>([this]()
    {
        return std::make_pair(WBQ::Registers(H->Core(), 0), WBQ::Registers(H->Core(), 1));
    }, this, [this](std::pair<WBApi::RegisterSet, WBApi::RegisterSet> r)
    {
        Busy = false;
        WBApi::RegisterSet sets[2] = {r.first, r.second};
        WBKB::KnowledgeStore& kb = W->KB();
        static const QStringList regs = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
        for (int c = 0; c < 2; c++)
        {
            const WBApi::RegisterSet& s = sets[c];
            if (s.r.size() < 16) continue;
            u32 pc = s.r[15];
            const WBKB::Function* f = kb.FunctionAt(c, pc, W->OverlayContext(pc));
            auto anns = kb.RegisterAnnotations(f, pc);
            for (int i = 0; i < 16; i++)
            {
                bool changed = Last[c].r.size() == 16 && Last[c].r[i] != s.r[i];
                QTableWidgetItem* it = Cell(Hex8(s.r[i]));
                if (changed) it->setForeground(WBColor::Red());
                Table[c]->setItem(i, 1, it);
                QString ann;
                for (const WBKB::Annotation* a : anns)
                    if (a->Register == regs[i]) ann += (ann.isEmpty() ? "" : "; ") + (a->AuthorKind == "agent" ? QString("[agent] ") : QString()) + a->Comment.section('\n', 0, 0);
                if (ann.isEmpty() && LooksLikeAddress(s.r[i]))
                {
                    QString n = kb.NameAt(c, s.r[i], W->OverlayContext(s.r[i]), true);
                    if (!n.isEmpty()) ann = "-> " + n;
                }
                Table[c]->setItem(i, 2, Cell(ann));
            }
            Table[c]->setItem(16, 1, Cell(Hex8(s.cpsr)));
            Table[c]->setItem(16, 2, Cell(s.flags));
            Table[c]->setItem(17, 1, Cell(Hex8(s.spsr)));
            Table[c]->setItem(18, 1, Cell(s.mode + (s.thumb ? " thumb" : " arm")));
            QString fn = f ? kb.NameAt(c, f->Addr, W->OverlayContext(f->Addr)) : QString();
            Table[c]->setItem(18, 2, Cell(fn.isEmpty() ? QString() : "in " + fn));
            Last[c] = s;
        }
    });
}

// ================================================================ inspector

InspectorPanel::InspectorPanel(WorkbenchWindow* w) : WBPanel(w)
{
    View = new QTextBrowser(this);
    View->setOpenLinks(false);
    connect(View, &QTextBrowser::anchorClicked, this, [this](const QUrl& u) { FollowLink(W, u); });
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(View);
}

void InspectorPanel::OnSelection(u32, u32) { Refresh(); }

void InspectorPanel::Refresh()
{
    if (Busy) return;
    Busy = true;
    u32 addr = W->SelStart();
    u32 cpu = (u32)W->Cpu();
    struct Res
    {
        QByteArray Bytes;
        bool Valid = false;
        ByteRecord Rec {}, All {};
        bool HasRec = false, HasAll = false;
        u32 Readers[2] {};
        u32 First[4] {};        // 1 + first frame of exec, read, write, DMA (0 = never)
        WBApi::WritesListResult Writes;
        WBApi::OriginResult Origin;
        QString Region;
        std::vector<WBAsset::RamRun> Gfx;
    };
    WBQ::ArchiveCtx arc {H->Archive(), H->ArchiveStatus()};
    WBQ::ContainerCtx cont {H->Containers(), H->ContainerStatus()};
    std::shared_ptr<const WBAsset::Catalog> cat = H->Graphics();
    H->Query<Res>([this, addr, cpu, arc, cont, cat]()
    {
        Res r;
        Workbench& wb = H->Core();
        if (cat) r.Gfx = WBAsset::RamRuns(wb, *cat, addr, 1, 4);
        r.Bytes.resize(8);
        for (int i = 0; i < 8; i++)
        {
            u32 v = 0;
            if (wb.Peek(cpu, addr + i, 1, v)) r.Valid = true;
            r.Bytes[i] = (char)v;
        }
        PhysAddr pa = wb.Translate(cpu, addr);
        r.Region = QString::fromStdString(RegionLabel(pa.Region)) + QString("+%1").arg(pa.Offset, 0, 16);
        r.HasRec = WBQ::ByteAt(wb, nullptr, cpu, addr, r.Rec);
        r.HasAll = WBQ::ByteAt(wb, H->AllTime(), cpu, addr, r.All);
        if (pa.Recordable())
            if (RecordPage* p = wb.Rec.Store().Get(pa.Region, pa.Offset >> PageShift))
            {
                r.Readers[0] = p->Readers[(pa.Offset & (PageSize - 1)) >> 2][0];
                r.Readers[1] = p->Readers[(pa.Offset & (PageSize - 1)) >> 2][1];
                memcpy(r.First, p->First[pa.Offset & (PageSize - 1)], sizeof(r.First));
            }
        WBApi::WritesListParams wp;
        wp.addr = addr;
        wp.size = 4;
        wp.limit = 8;
        QString e;
        WBQ::Writes(wb, wp, r.Writes, e);
        WBApi::OriginParams op;
        op.addr = addr;
        op.size = 4;
        op.cpu = cpu ? "arm7" : "arm9";
        WBQ::Origin(wb, op, r.Origin, e, arc, cont);
        return r;
    }, this, [this, addr, cpu, cat](Res r)
    {
        Busy = false;
        WBKB::KnowledgeStore& kb = W->KB();
        auto link = [&](u32 a)
        {
            QString n = kb.NameAt((int)cpu, a, W->OverlayContext(a), true);
            return QString("<a href=\"addr:%1\">%1</a>%2").arg(Hex8(a), n.isEmpty() ? QString() : " &lt;" + n.toHtmlEscaped() + "&gt;");
        };
        const u8* b = (const u8*)r.Bytes.constData();
        u32 v32; memcpy(&v32, b, 4);
        u16 v16; memcpy(&v16, b, 2);
        QString h = QString("<b>%1</b> &nbsp; <span style='color:%MUTED%'>%2</span><br>").arg(link(addr), r.Region);
        QString n = kb.NameAt((int)cpu, addr, W->OverlayContext(addr), true);
        if (!r.Valid) h += "<i>not readable without side effects (IO or unmapped)</i><br>";
        h += "<table cellspacing=0 cellpadding=1>";
        auto row = [&](const QString& k, const QString& v) { h += "<tr><td style='color:%MUTED%'>" + k + "&nbsp;</td><td>" + v + "</td></tr>"; };
        row("u8", QString("%1 (0x%2)").arg(b[0]).arg(b[0], 2, 16, QChar('0')));
        row("s8", QString::number((s8)b[0]));
        row("u16", QString("%1 (0x%2)").arg(v16).arg(v16, 4, 16, QChar('0')));
        row("s16", QString::number((s16)v16));
        row("u32", QString("%1 (0x%2)").arg(v32).arg(Hex8(v32)));
        row("s32", QString::number((s32)v32));
        row("fx32", QString::number((s32)v32 / 4096.0, 'f', 6));
        row("fx16", QString::number((s16)v16 / 4096.0, 'f', 6));
        row("binary", QString("%1").arg(v32, 32, 2, QChar('0')).replace(QRegularExpression("(.{8})(?!$)"), "\\1 "));
        if (LooksLikeAddress(v32)) row("pointer", link(v32));
        h += "</table>";
        // how the byte decodes, and why (the disassembly's mode gutter)
        WBQ::KbHints hints = WBQ::KbHints::Collect(kb, (int)cpu, addr, addr + 1, W->OverlayContext(addr));
        WBQ::AnalysisHints guessed = H->AnalysisNear((int)cpu, addr, addr + 1);
        WBQ::Classification cls = WBQ::Classify(r.HasRec ? &r.Rec : nullptr, hints, addr, &guessed);
        h += QString("<p><b>Decoded as</b> %1%2").arg(cls.Conflict ? "<span style='color:%RED%'>&#9888;</span> " : "", cls.Why.toHtmlEscaped());

        auto counters = [&](const char* title, const ByteRecord& rec)
        {
            QStringList fl;
            WBQ::FlagNames(rec.flags, &fl);
            h += QString("<p><b>%1</b><br>exec %2, read %3, write %4, dma read %5, dma write %6<br><span style='color:%MUTED%'>%7</span>")
                .arg(title).arg(rec.exec_count).arg(rec.read_count).arg(rec.write_count).arg(rec.dma_read_count).arg(rec.dma_write_count).arg(fl.join(", "));
        };
        if (r.HasRec)
        {
            counters("This session", r.Rec);
            QStringList firsts;
            static const char* kinds[4] = {"exec", "read", "write", "DMA"};
            for (int k = 0; k < 4; k++)
                if (r.First[k]) firsts << QString("%1 %2").arg(kinds[k]).arg(r.First[k] - 1);
            if (!firsts.isEmpty()) h += "<br>first frame: " + firsts.join(", ");
            if (r.Rec.write_count || r.Rec.dma_write_count)
            {
                bool dma = (r.Rec.last_writer_cpu & 0xF) >= 2;
                h += QString("<br>last writer: %1 %2 in frame %3")
                    .arg(WBQ::CpuName(r.Rec.last_writer_cpu & 0xF))
                    .arg(dma ? QString("from %1").arg(Hex8(r.Rec.last_writer_pc)) : link(r.Rec.last_writer_pc))
                    .arg(r.Rec.last_write_frame);
            }
            for (int k = 0; k < 2; k++)
                if (r.Readers[k]) h += QString("<br>reader: %1 %2").arg(WBQ::CpuName(r.Readers[k] & 1), link(r.Readers[k] & ~1u));
        }
        else h += "<p style='color:%MUTED%'>No recorded accesses in this session.";
        if (r.HasAll && H->AllTime()) counters("All sessions", r.All);

        if (!r.Writes.entries.empty())
        {
            h += "<p><b>Write history</b><table cellspacing=0 cellpadding=1>";
            for (auto it = r.Writes.entries.rbegin(); it != r.Writes.entries.rend(); ++it)
                h += QString("<tr><td>f%1&nbsp;</td><td>%2&nbsp;</td><td>%3&nbsp;</td><td>%4 -&gt; %5</td></tr>")
                    .arg(it->frame).arg(it->cpu).arg(it->cpu.startsWith("dma") ? Hex8(it->pc) : link(it->pc))
                    .arg(it->old_value, 0, 16).arg(it->new_value, 0, 16);
            h += "</table>";
        }
        else if (!r.Writes.history_enabled) h += "<p style='color:%MUTED%'>Write history is off.";

        if (!r.Origin.chain.empty())
        {
            h += "<p><b>Origin</b><br>";
            for (const auto& c : r.Origin.chain)
            {
                h += QString("%1: ").arg(c.kind);
                if (!c.file_name.isEmpty()) h += "<b>" + c.file_name.toHtmlEscaped() + "</b> ";
                h += QString("%1+%2 -&gt; %3").arg(Hex8(c.src_addr)).arg(c.src_size, 0, 16).arg(Hex8(c.dst_addr));
                if (c.pc) h += " by " + link(c.pc);
                if (!c.detail.isEmpty()) h += " <span style='color:%MUTED%'>(" + c.detail.toHtmlEscaped() + ")</span>";
                h += "<br>";
            }
        }
        for (const WBAsset::RamRun& g : r.Gfx)
        {
            if (!cat || addr < g.Addr || addr >= g.Addr + g.Size || g.File < 0) continue;
            const WBAsset::File& f = cat->Files()[g.File];
            u32 fo = g.FileOffset + (addr - g.Addr);
            WBGfx::FieldInfo fi = WBGfx::DescribeAt(f.Desc, f.Data, f.Size, fo);
            h += QString("<p><b>File structure</b> <a href=\"gfx:%1\">[graphics]</a><br>%2 +0x%3<br>")
                     .arg(Hex8(addr), QString::fromStdString(f.Path).toHtmlEscaped()).arg(fo, 0, 16);
            if (fi.HasColor)
            {
                QRgb c = WBGfx::Bgr555(fi.Color);
                h += QString("<span style='background-color:#%1%2%3'>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;</span> ")
                         .arg(qRed(c), 2, 16, QChar('0')).arg(qGreen(c), 2, 16, QChar('0')).arg(qBlue(c), 2, 16, QChar('0'));
            }
            h += QString::fromStdString(fi.Text).toHtmlEscaped();
            // the tile, drawn with the paired palette
            if (fi.Tile >= 0 && (f.Kind == "NCGR" || f.Kind == "NSCR"))
            {
                int gi = f.Kind == "NCGR" ? g.File : cat->Pair(g.File, "NCGR");
                int pi = gi >= 0 ? cat->Pair(gi, "NCLR") : -1;
                if (gi >= 0)
                {
                    const WBAsset::File& gf = cat->Files()[gi];
                    std::vector<QRgb> pal = pi >= 0 ? WBGfx::PaletteColors(cat->Files()[pi].Desc, cat->Files()[pi].Data, cat->Files()[pi].Size)
                                                    : std::vector<QRgb>();
                    QImage tile = WBGfx::RenderTile(gf.Desc, gf.Data, gf.Size, fi.Tile, pal, std::max(0, fi.Palette));
                    if (!tile.isNull())
                    {
                        View->document()->addResource(QTextDocument::ImageResource, QUrl("wbgfx:tile"), tile.scaled(32, 32));
                        h += QString("<br><img src=\"wbgfx:tile\" width=32 height=32> <span style='color:%MUTED%'>tile %1</span>").arg(fi.Tile);
                        LastHtml.clear();   // the image may have changed under the same name
                    }
                }
            }
            break;
        }
        if (!r.Origin.archive.empty())
            h += "<p><b>Archived file</b><br>" + ArchiveSegmentsHtml(W, r.Origin.archive, true);
        if (!r.Origin.containers.empty())
            h += "<p><b>File in a container</b><br>" + ArchiveSegmentsHtml(W, r.Origin.containers, true, true);
        for (const auto& t : r.Origin.taint)
        {
            QStringList s;
            for (const QString& x : t.sources) s << x;
            h += QString("<br>derived from: <b>%1</b> (%2)").arg(s.join(", ").toHtmlEscaped()).arg(ByteCount(t.size));
        }
        auto anns = kb.AnnotationsAt((int)cpu, addr, W->OverlayContext(addr));
        if (!anns.empty())
        {
            h += "<p><b>Annotations</b><br>";
            for (const WBKB::Annotation* a : anns)
                h += QString("%1%2 <span style='color:%MUTED%'>- %3</span><br>").arg(a->AuthorKind == "agent" ? "[agent] " : "", a->Comment.toHtmlEscaped(), a->Author.toHtmlEscaped());
        }
        if (h != LastHtml)
        {
            LastHtml = h;
            View->setHtml(WBColor::ApplyHtmlColors(h));
        }
    });
}

// ================================================================ annotations

AnnotationsPanel::AnnotationsPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Author = new QComboBox(this);
    Kind = new QComboBox(this);
    Kind->addItems({"all targets", "address", "range", "function", "data_object", "struct_field", "type", "register", "overlay", "savestate",
                    "decode overrides", "decode conflicts"});
    Kind->setItemData(Kind->count() - 2, "Ranges set to decode as ARM, Thumb or data (Disassembly > Treat as)", Qt::ToolTipRole);
    Kind->setItemData(Kind->count() - 1, "Overrides the recording disagrees with: marked data but executed, or executed in the other mode", Qt::ToolTipRole);
    Tag = new QLineEdit(this);
    Tag->setPlaceholderText("tag");
    InView = new QCheckBox("near cursor", this);
    Table = MakeTable(this, {"Target", "Where", "Comment", "Tags", "Author", "Updated"});
    QPushButton* add = new QPushButton("Add", this);
    QPushButton* edit = new QPushButton("Edit", this);
    QPushButton* del = new QPushButton("Delete", this);
    QHBoxLayout* f = new QHBoxLayout();
    f->addWidget(Author);
    f->addWidget(Kind);
    f->addWidget(Tag);
    f->addWidget(InView);
    QHBoxLayout* b = new QHBoxLayout();
    b->addWidget(add);
    b->addWidget(edit);
    b->addWidget(del);
    b->addStretch();
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addLayout(f);
    v->addWidget(Table);
    v->addLayout(b);
    connect(Author, &QComboBox::currentTextChanged, this, [this]() { Refresh(); });
    connect(Kind, &QComboBox::currentTextChanged, this, [this]() { Refresh(); });
    connect(Tag, &QLineEdit::textChanged, this, [this]() { Refresh(); });
    connect(InView, &QCheckBox::toggled, this, [this]() { Refresh(); });
    connect(add, &QPushButton::clicked, this, [this]() { W->CommentAt(W->Cursor()); });
    auto current = [this]() -> const WBKB::Annotation*
    {
        int r = Table->currentRow();
        if (r < 0) return nullptr;
        qint64 id = Table->item(r, 0)->data(Qt::UserRole).toLongLong();
        for (const WBKB::Annotation& a : W->KB().Annotations()) if (a.Id == id) return &a;
        return nullptr;
    };
    connect(edit, &QPushButton::clicked, this, [this, current]()
    {
        const WBKB::Annotation* a = current();
        if (!a) return;
        WBKB::Annotation copy = *a;
        AnnotationDialog d(W, &copy, copy.Addr, copy.EndAddr - copy.Addr);
        d.exec();
    });
    connect(del, &QPushButton::clicked, this, [this, current]()
    {
        // a decode override row: clear that override
        int row = Table->currentRow();
        QVariant at = row >= 0 ? Table->item(row, 0)->data(Qt::UserRole + 1) : QVariant();
        if (at.isValid())
        {
            for (const WBKB::DecodeOverride& o : W->KB().DecodeOverrides())
                if (o.Start == at.toUInt())
                {
                    QString err;
                    WBKB::DecodeOverride copy = o;
                    if (!W->KB().SetDecodeOverride(copy.Cpu, copy.Start, copy.End, "clear", W->OverlayContext(copy.Start), err))
                        W->ShowError("Clear override", err);
                    break;
                }
            return;
        }
        const WBKB::Annotation* a = current();
        if (!a) return;
        QString err;
        if (!W->KB().DeleteAnnotation(a->Id, err)) W->ShowError("Delete annotation", err);
    });
    W->AddRowMenu(Table, [this](const QModelIndex& i, u32& a, u32& n, int& cpu)
    {
        QVariant at = i.sibling(i.row(), 0).data(Qt::UserRole + 1);
        if (at.isValid()) { a = at.toUInt(); return true; }
        qint64 id = i.sibling(i.row(), 0).data(Qt::UserRole).toLongLong();
        for (const WBKB::Annotation& x : W->KB().Annotations())
            if (x.Id == id && x.HasAddr) { a = x.Addr; n = x.EndAddr > x.Addr ? x.EndAddr - x.Addr : 1; cpu = x.Cpu; return true; }
        return false;
    });
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this, current](int row, int)
    {
        // decode override rows carry their address
        QVariant at = Table->item(row, 0)->data(Qt::UserRole + 1);
        if (at.isValid()) { W->GoTo(at.toUInt()); return; }
        const WBKB::Annotation* a = current();
        if (a && a->HasAddr) W->GoTo(a->Addr);
    });
}

void AnnotationsPanel::ShowOverrides(bool conflictsOnly)
{
    if (Busy) return;
    struct Row { WBKB::DecodeOverride O; QString Problem; };
    std::vector<Row> rows;
    QString af = Author->currentText();
    for (const WBKB::DecodeOverride& o : W->KB().DecodeOverrides())
    {
        if (af == "people" && o.AuthorKind != "person") continue;
        if (af == "agents" && o.AuthorKind != "agent") continue;
        if (af != "all authors" && af != "people" && af != "agents" && o.Author != af) continue;
        rows.push_back({o, QString()});
    }
    Busy = true;
    // what the recording says inside each range (emulation thread)
    H->Query<std::vector<Row>>([this, rows]() mutable
    {
        Workbench& wb = H->Core();
        for (Row& r : rows)
            r.Problem = WBQ::DecodeConflict(wb, (u32)r.O.Cpu, r.O.Start, r.O.End, r.O.Mode);
        return rows;
    }, this, [this, conflictsOnly](std::vector<Row> rows)
    {
        Busy = false;
        Table->setSortingEnabled(false);
        Table->setRowCount(0);
        for (const Row& r : rows)
        {
            if (conflictsOnly && r.Problem.isEmpty()) continue;
            int i = Table->rowCount();
            Table->insertRow(i);
            QTableWidgetItem* t = Cell(r.Problem.isEmpty() ? "decode override" : QString::fromUtf8("\u26A0 decode conflict"));
            t->setData(Qt::UserRole + 1, (uint)r.O.Start);
            if (!r.Problem.isEmpty()) t->setForeground(WBColor::Red());
            Table->setItem(i, 0, t);
            Table->setItem(i, 1, Cell(QString("%1 .. %2").arg(W->AddrText(r.O.Start)).arg(r.O.End, 8, 16, QChar('0'))));
            Table->setItem(i, 2, Cell(QString("decode as %1%2").arg(r.O.Mode, r.Problem.isEmpty() ? QString() : "; " + r.Problem)));
            Table->setItem(i, 3, Cell(QString()));
            QTableWidgetItem* au = Cell((r.O.AuthorKind == "agent" ? "[agent] " : "") + r.O.Author);
            Table->setItem(i, 4, au);
            Table->setItem(i, 5, Cell(QString()));
        }
        Table->setSortingEnabled(true);
    });
}

void AnnotationsPanel::Refresh()
{
    WBKB::KnowledgeStore& kb = W->KB();
    QString curAuthor = Author->currentText();
    std::set<QString> authors;
    for (const WBKB::Annotation& a : kb.Annotations()) authors.insert(a.Author);
    QStringList items = {"all authors", "people", "agents"};
    for (const QString& a : authors) items << a;
    if (Author->count() != items.size())
    {
        Author->blockSignals(true);
        Author->clear();
        Author->addItems(items);
        int i = Author->findText(curAuthor);
        Author->setCurrentIndex(i < 0 ? 0 : i);
        Author->blockSignals(false);
    }
    QString af = Author->currentText();
    QString kf = Kind->currentText();
    if (kf == "decode overrides" || kf == "decode conflicts") { ShowOverrides(kf == "decode conflicts"); return; }
    QString tf = Tag->text().trimmed();
    u32 cur = W->Cursor();
    Table->setSortingEnabled(false);
    Table->setRowCount(0);
    for (const WBKB::Annotation& a : kb.Annotations())
    {
        if (af == "people" && a.AuthorKind != "person") continue;
        if (af == "agents" && a.AuthorKind != "agent") continue;
        if (af != "all authors" && af != "people" && af != "agents" && a.Author != af) continue;
        if (kf != "all targets" && a.TargetKind != kf) continue;
        if (!tf.isEmpty() && !a.TagList().contains(tf, Qt::CaseInsensitive)) continue;
        if (InView->isChecked() && (!a.HasAddr || a.Addr + 0x400 < cur || a.Addr > cur + 0x400)) continue;
        int r = Table->rowCount();
        Table->insertRow(r);
        QString where;
        if (a.HasAddr) where = W->AddrText(a.Addr);
        else if (a.FunctionId)
        {
            const WBKB::Function* f = W->KB().FunctionById(a.FunctionId);
            where = f ? W->KB().NameAt(W->Cpu(), f->Addr, W->OverlayContext(f->Addr)) : QString("function %1").arg(a.FunctionId);
        }
        else if (!a.TypeName.isEmpty()) where = a.TypeName + (a.FieldName.isEmpty() ? "" : "." + a.FieldName);
        else if (a.OverlayId >= 0) where = QString("overlay %1").arg(a.OverlayId);
        else if (!a.Savestate.isEmpty()) where = a.Savestate;
        if (!a.Register.isEmpty()) where = a.Register + " in " + where;
        Table->setItem(r, 0, Cell(a.TargetKind, a.Id));
        Table->setItem(r, 1, Cell(where));
        Table->setItem(r, 2, Cell(a.Comment.section('\n', 0, 0)));
        Table->setItem(r, 3, Cell(a.Tags));
        QTableWidgetItem* au = Cell((a.AuthorKind == "agent" ? "[agent] " : "") + a.Author);
        Table->setItem(r, 4, au);
        Table->setItem(r, 5, Cell(a.Updated.left(19).replace('T', ' ')));
    }
    Table->setSortingEnabled(true);
}

// ================================================================ functions

namespace
{

class MatchDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        QStyleOptionViewItem o = opt;
        initStyleOption(&o, idx);
        o.text.clear();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &o, p, opt.widget);
        if (!idx.data(Qt::UserRole).toBool()) return;
        QRect track = opt.rect.adjusted(6, 6, -6, -6);
        p->fillRect(track, WBColor::Off());
        if (!idx.data(Qt::UserRole + 1).toBool()) return;
        int kind = idx.data(Qt::UserRole + 3).toInt();
        QColor c = kind == 0 ? WBColor::Green() : kind == 1 ? WBColor::Amber() : WBColor::Red();
        double score = idx.data(Qt::UserRole + 2).toDouble();
        if (score < 0) score = 0;
        if (score > 100) score = 100;
        p->fillRect(QRect(track.left(), track.top(), (int)(track.width() * score / 100.0), track.height()), c);
    }
};

}

bool FunctionsPanel::Need(WBNeed& need, QString& what) const
{
    if (!Filters || Filters->checkedId() != FiltOutside) return false;
    need = WBNeed::Recording;
    what = "executed code outside functions";
    return true;
}

FunctionsPanel::FunctionsPanel(WorkbenchWindow* w) : WBPanel(w)
{
    const char* labels[6] = {"Seen", "Unnamed", "Matched", "Partial", "Unmatched", "Not in a function"};
    Filters = new QButtonGroup(this);
    Filters->setExclusive(true);
    QHBoxLayout* counts = new QHBoxLayout();
    for (int i = 0; i < 6; i++)
    {
        FilterBtn[i] = new QPushButton(labels[i], this);
        FilterBtn[i]->setCheckable(true);
        FilterBtn[i]->setFocusPolicy(Qt::NoFocus);
        Filters->addButton(FilterBtn[i], i);
        counts->addWidget(FilterBtn[i]);
    }
    FilterBtn[FiltSeen]->setChecked(true);
    FilterBtn[FiltOutside]->setStyleSheet(QString("QPushButton { color: %1; } QPushButton:checked { background: %2; }").arg(WBColor::Amber().name(), WBColor::Unowned().name()));
    Filter = new QLineEdit(this);
    Filter->setPlaceholderText("filter by name or address");
    Table = MakeTable(this, {"Name", "Address", "Size", "Mode", "Times run", "Match %"});
    Table->setItemDelegateForColumn(5, new MatchDelegate(Table));
    QPushButton* nw = new QPushButton("New", this);
    nw->setToolTip("For code that never ran, an unusual prologue, an overlay, or a branch that is not a call. Entries that ran are created automatically.");
    QPushButton* rn = new QPushButton("Rename", this);
    QPushButton* ed = new QPushButton("Edit / resize", this);
    QHBoxLayout* b = new QHBoxLayout();
    b->addWidget(nw);
    b->addWidget(rn);
    b->addWidget(ed);
    b->addStretch();
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addLayout(counts);
    v->addWidget(Filter);
    v->addWidget(Table);
    v->addLayout(b);
    connect(Filters, &QButtonGroup::idClicked, this, [this](int) { Rebuild(); });
    connect(Filter, &QLineEdit::textChanged, this, [this]() { Rebuild(); });
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this](int r, int)
    {
        W->GoTo((u32)Table->item(r, 1)->data(Qt::UserRole).toUInt());
    });
    W->AddRowMenu(Table, [](const QModelIndex& i, u32& a, u32&, int&)
    {
        a = i.sibling(i.row(), 1).data(Qt::UserRole).toUInt();
        return true;
    }, [this](QMenu& m, const QModelIndex& i)
    {
        u32 a = i.sibling(i.row(), 1).data(Qt::UserRole).toUInt();
        bool known = W->KB().FunctionAt(W->Cpu(), a, W->OverlayContext(a)) != nullptr;
        m.addAction(known ? "Edit function...\tP" : "Define function here\tP", [this, a]() { W->DefineFunctionAt(a); });
    });
    auto sel = [this]() -> u32 { int r = Table->currentRow(); return r < 0 ? W->Cursor() : (u32)Table->item(r, 1)->data(Qt::UserRole).toUInt(); };
    connect(nw, &QPushButton::clicked, this, [this, sel]() { W->DefineFunctionAt(sel()); });
    connect(rn, &QPushButton::clicked, this, [this, sel]() { W->RenameAt(sel()); });
    connect(ed, &QPushButton::clicked, this, [this, sel]() { W->DefineFunctionAt(sel()); });
}

void FunctionsPanel::Rebuild()
{
    WBKB::KnowledgeStore& kb = W->KB();
    QString f = Filter->text().trimmed();
    int which = Filters->checkedId();
    if (which < 0) which = FiltSeen;
    int total = 0, unnamed = 0, matched = 0, partial = 0, unmatched = 0;
    for (const WBKB::Function& fn : kb.Functions())
    {
        total++;
        if (fn.Name.isEmpty() || fn.Name.startsWith("sub_", Qt::CaseInsensitive)) unnamed++;
        if (fn.Status == "matched") matched++;
        else if (fn.Status == "partially_matched") partial++;
        else unmatched++;
    }
    const int counts[6] = {total, unnamed, matched, partial, unmatched, (int)Unknown.size()};
    const char* labels[6] = {"Seen", "Unnamed", "Matched", "Partial", "Unmatched", "Not in a function"};
    for (int i = 0; i < 6; i++) FilterBtn[i]->setText(QString("%1 %2").arg(labels[i]).arg(counts[i]));
    bool scores = !H->Settings().CompareCommand.isEmpty();
    if (QTableWidgetItem* head = Table->horizontalHeaderItem(5))
        head->setForeground(scores ? WBColor::Text() : WBColor::Muted());
    Table->setSortingEnabled(false);
    Table->setRowCount(0);
    auto num = [](qulonglong n) { QTableWidgetItem* it = new QTableWidgetItem(); it->setData(Qt::DisplayRole, n); return it; };
    if (which == FiltOutside)
    {
        for (const WBApi::CoverageRange& u : Unknown)
        {
            if (!f.isEmpty() && !Hex8(u.start).contains(f, Qt::CaseInsensitive) && !u.mode.contains(f, Qt::CaseInsensitive)) continue;
            int r = Table->rowCount();
            Table->insertRow(r);
            QTableWidgetItem* n = Cell("(executed, no function)");
            n->setForeground(WBColor::Amber());
            Table->setItem(r, 0, n);
            QTableWidgetItem* a = Cell(Hex8(u.start));
            a->setData(Qt::UserRole, (uint)u.start);
            a->setForeground(WBColor::Amber());
            Table->setItem(r, 1, a);
            Table->setItem(r, 2, num(u.end - u.start));
            Table->setItem(r, 3, Cell(u.mode));
            Table->setItem(r, 4, num(u.count));
            QTableWidgetItem* sc = new QTableWidgetItem();
            sc->setData(Qt::UserRole, false);
            Table->setItem(r, 5, sc);
        }
    }
    for (const WBKB::Function& fn : kb.Functions())
    {
        if (which == FiltOutside) break;
        QString name = fn.Name.isEmpty() ? QString("sub_%1").arg(fn.Addr, 8, 16, QChar('0')) : fn.Name;
        bool def = fn.Name.isEmpty() || fn.Name.startsWith("sub_", Qt::CaseInsensitive);
        if (which == FiltUnnamed && !def) continue;
        if (which == FiltMatched && fn.Status != "matched") continue;
        if (which == FiltPartial && fn.Status != "partially_matched") continue;
        if (which == FiltUnmatched && (fn.Status == "matched" || fn.Status == "partially_matched")) continue;
        if (!f.isEmpty() && !name.contains(f, Qt::CaseInsensitive) && !Hex8(fn.Addr).contains(f, Qt::CaseInsensitive)) continue;
        int r = Table->rowCount();
        Table->insertRow(r);
        QTableWidgetItem* ni = Cell(name);
        if (def) ni->setForeground(WBColor::Muted());
        Table->setItem(r, 0, ni);
        QTableWidgetItem* a = Cell(Hex8(fn.Addr));
        a->setData(Qt::UserRole, (uint)fn.Addr);
        a->setForeground(WBColor::Muted());
        Table->setItem(r, 1, a);
        const WBKB::Module* mod = kb.ModuleById(fn.ModuleId);
        int cpu = mod ? mod->CpuNum() : 0;
        QTableWidgetItem* sz = num(fn.Size);
        if (H->FunctionHasNotch(cpu, fn.Addr)) sz->setForeground(WBColor::Amber());
        else if (H->FunctionOpen(cpu, fn.Addr)) sz->setForeground(WBColor::Muted());
        Table->setItem(r, 2, sz);
        auto ec = ExecCounts.find(fn.Addr);
        bool ran = ec != ExecCounts.end() && ec->second > 0;
        QTableWidgetItem* mode = Cell(fn.Mode);
        mode->setToolTip(ran ? "Executed in this session" : "Not executed in this session yet");
        Table->setItem(r, 3, mode);
        Table->setItem(r, 4, num(ec == ExecCounts.end() ? 0 : ec->second));
        QTableWidgetItem* sc = new QTableWidgetItem();
        bool has = scores && fn.LastScore >= 0;
        int kind = fn.Status == "matched" ? 0 : fn.Status == "partially_matched" ? 1 : 2;
        if (kind == 2 && fn.LastScore < 0) has = false;
        sc->setData(Qt::UserRole, scores);
        sc->setData(Qt::UserRole + 1, has);
        sc->setData(Qt::UserRole + 2, fn.LastScore);
        sc->setData(Qt::UserRole + 3, kind);
        if (has) sc->setData(Qt::DisplayRole, fn.LastScore);
        Table->setItem(r, 5, sc);
    }
    Table->setSortingEnabled(true);
}

void FunctionsPanel::Refresh()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - LastRefreshMs < 1500 || Busy) return;
    LastRefreshMs = now;
    Busy = true;
    std::vector<std::pair<u32, int>> entries;
    std::vector<WBQ::FuncRange> known;
    for (const WBKB::Function& f : W->KB().Functions())
    {
        const WBKB::Module* m = W->KB().ModuleById(f.ModuleId);
        int cpu = m ? m->CpuNum() : 0;
        entries.push_back({f.Addr, cpu});
        known.push_back({f.Addr, f.Addr + std::max<u32>(f.Size, f.Thumb() ? 2 : 4), cpu});
    }
    struct Res { std::map<u32, u32> Counts; std::vector<WBApi::CoverageRange> Unknown; };
    H->Query<Res>([this, entries, known]()
    {
        Res r;
        Workbench& wb = H->Core();
        for (auto& e : entries)
            if (u32 n = WBQ::ExecCount(wb, (u32)e.second, e.first)) r.Counts[e.first] = n;
        WBApi::CoverageParams p;
        p.kind = "exec";
        p.unknown_only = true;
        p.limit = 2000;
        WBApi::CoverageResult cr;
        QString err;
        WBQ::Coverage(wb, nullptr, p, known, cr, err);
        r.Unknown = cr.ranges;
        return r;
    }, this, [this](Res r)
    {
        Busy = false;
        bool changed = r.Counts != ExecCounts || r.Unknown.size() != Unknown.size();
        ExecCounts = r.Counts;
        Unknown = r.Unknown;
        if (changed || Table->rowCount() == 0) Rebuild();
    });
}

// ================================================================ bookmarks

BookmarksPanel::BookmarksPanel(WorkbenchWindow* w) : WBPanel(w)
{
    List = new QListWidget(this);
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    List->setFont(f);
    QPushButton* add = new QPushButton("Add at cursor", this);
    QPushButton* del = new QPushButton("Delete", this);
    QHBoxLayout* b = new QHBoxLayout();
    b->addWidget(add);
    b->addWidget(del);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(List);
    v->addLayout(b);
    connect(add, &QPushButton::clicked, this, [this]() { W->AddBookmarkAt(W->Cursor()); });
    connect(del, &QPushButton::clicked, this, [this]()
    {
        QListWidgetItem* it = List->currentItem();
        if (!it) return;
        QString err;
        if (!W->KB().DeleteBookmark(it->data(Qt::UserRole).toLongLong(), err)) W->ShowError("Bookmark", err);
    });
    W->AddRowMenu(List, [](const QModelIndex& i, u32& a, u32&, int& cpu)
    {
        a = i.data(Qt::UserRole + 1).toUInt();
        cpu = i.data(Qt::UserRole + 2).toInt();
        return true;
    });
    connect(List, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it)
    {
        int cpu = it->data(Qt::UserRole + 2).toInt();
        if (cpu != W->Cpu()) W->SetCpu(cpu);
        W->GoTo((u32)it->data(Qt::UserRole + 1).toUInt());
    });
}

void BookmarksPanel::Refresh()
{
    const auto& bm = W->KB().Bookmarks();
    if ((size_t)List->count() == bm.size()) return;
    List->clear();
    for (const WBKB::Bookmark& b : bm)
    {
        QListWidgetItem* it = new QListWidgetItem(QString("%1 %2  %3").arg(b.Cpu ? "7" : "9", Hex8(b.Address), b.Name));
        it->setData(Qt::UserRole, b.Id);
        it->setData(Qt::UserRole + 1, (uint)b.Address);
        it->setData(Qt::UserRole + 2, b.Cpu);
        List->addItem(it);
    }
}

// ================================================================ call stack

CallStackPanel::CallStackPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Tabs = new QTabWidget(this);
    for (int c = 0; c < 2; c++)
    {
        Tree[c] = new QTreeWidget(this);
        Tree[c]->setHeaderLabels({"Function", "Entry", "Called from", "Returns to", "Kind"});
        Tree[c]->setRootIsDecorated(false);
        Tabs->addTab(Tree[c], c ? "ARM7" : "ARM9");
        connect(Tree[c], &QTreeWidget::itemDoubleClicked, this, [this, c](QTreeWidgetItem* it, int col)
        {
            if (W->Cpu() != c) W->SetCpu(c);
            W->GoTo((u32)it->data(col == 2 ? 2 : 1, Qt::UserRole).toUInt());
        });
        W->AddRowMenu(Tree[c], [c](const QModelIndex& i, u32& a, u32&, int& cpu)
        {
            QVariant v = i.sibling(i.row(), i.column() == 2 ? 2 : 1).data(Qt::UserRole);
            a = v.toUInt();
            cpu = c;
            return v.isValid();
        });
    }
    Note = new QLabel("The shadow call stack is available while recording.", this);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(Note);
    v->addWidget(Tabs);
}

void CallStackPanel::Refresh()
{
    if (Busy) return;
    Busy = true;
    H->Query<std::pair<WBApi::CallStackResult, WBApi::CallStackResult>>([this]()
    {
        return std::make_pair(WBQ::CallStack(H->Core(), 0), WBQ::CallStack(H->Core(), 1));
    }, this, [this](std::pair<WBApi::CallStackResult, WBApi::CallStackResult> r)
    {
        Busy = false;
        Note->setVisible(false);          // the banner above says when Recording is off
        WBApi::CallStackResult rs[2] = {r.first, r.second};
        for (int c = 0; c < 2; c++)
        {
            Tree[c]->clear();
            for (auto it = rs[c].frames.rbegin(); it != rs[c].frames.rend(); ++it)
            {
                QTreeWidgetItem* i = new QTreeWidgetItem(Tree[c]);
                QString n = W->KB().NameAt(c, it->callee, W->OverlayContext(it->callee));
                i->setText(0, n.isEmpty() ? QString("sub_%1").arg(Hex8(it->callee)) : n);
                i->setText(1, Hex8(it->callee));
                i->setData(1, Qt::UserRole, (uint)it->callee);
                QString cs = W->KB().NameAt(c, it->call_site, W->OverlayContext(it->call_site), true);
                i->setText(2, Hex8(it->call_site) + (cs.isEmpty() ? "" : " " + cs));
                i->setData(2, Qt::UserRole, (uint)it->call_site);
                i->setText(3, Hex8(it->return_addr));
                i->setText(4, it->kind);
            }
        }
    });
}

// ================================================================ call graph

CallGraphPanel::CallGraphPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Tree = new QTreeWidget(this);
    Tree->setHeaderLabels({"Function", "Count", "Call site"});
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(Tree);
    connect(Tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it)
    {
        if (it->childCount() == 1 && it->child(0)->text(0) == "...")
        {
            delete it->takeChild(0);
            u32 f = (u32)it->data(0, Qt::UserRole).toUInt();
            bool callers = it->data(0, Qt::UserRole + 1).toBool();
            Populate(it, f, callers);
        }
    });
    connect(Tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int)
    {
        QVariant v = it->data(0, Qt::UserRole);
        if (v.isValid()) W->GoTo((u32)v.toUInt());
    });
    W->AddRowMenu(Tree, [](const QModelIndex& i, u32& a, u32&, int&)
    {
        QVariant v = i.sibling(i.row(), 0).data(Qt::UserRole);
        a = v.toUInt();
        return v.isValid();
    });
}

void CallGraphPanel::OnNavigate(u32 addr)
{
    if (!IsShown()) return;
    int ov = W->OverlayContext(addr);
    const WBKB::Function* f = W->KB().FunctionAt(W->Cpu(), addr, ov);
    u32 root = f ? f->Addr : addr;
    if (root == Root) return;
    Root = root;
    Tree->clear();
    QString n = W->KB().NameAt(W->Cpu(), root, ov);
    QTreeWidgetItem* top = new QTreeWidgetItem(Tree, {n.isEmpty() ? QString("sub_%1").arg(Hex8(root)) : n});
    top->setData(0, Qt::UserRole, (uint)root);
    QTreeWidgetItem* callers = new QTreeWidgetItem(top, {"Callers"});
    QTreeWidgetItem* callees = new QTreeWidgetItem(top, {"Callees"});
    top->setExpanded(true);
    Populate(callers, root, true);
    Populate(callees, root, false);
    callers->setExpanded(true);
    callees->setExpanded(true);
    Tree->resizeColumnToContents(0);
}

void CallGraphPanel::Populate(QTreeWidgetItem* item, u32 func, bool callers)
{
    int cpu = W->Cpu();
    const WBKB::Function* f = W->KB().FunctionStarting(cpu, func, W->OverlayContext(func));
    u32 size = f ? f->Size : 0;
    QPointer<QTreeWidget> tree(Tree);
    H->Query<WBApi::CallsResult>([this, cpu, func, size]()
    {
        WBApi::CallsResult r;
        WBQ::Calls(H->Core(), H->AllTime(), (u32)cpu, func, size, 200, r);
        return r;
    }, this, [this, item, callers, cpu, tree](WBApi::CallsResult r)
    {
        if (!tree) return;
        // make sure the item still exists
        bool alive = false;
        QTreeWidgetItemIterator it(Tree);
        while (*it) { if (*it == item) { alive = true; break; } ++it; }
        if (!alive) return;
        const auto& list = callers ? r.callers : r.callees;
        item->setText(1, QString::number(list.size()));
        for (const WBApi::CallEdgeInfo& e : list)
        {
            u32 target = callers ? e.caller_func : e.callee;
            if (callers && !target)
            {
                const WBKB::Function* cf = W->KB().FunctionAt(cpu, e.call_site, W->OverlayContext(e.call_site));
                target = cf ? cf->Addr : e.call_site;
            }
            QString n = W->KB().NameAt(cpu, target, W->OverlayContext(target));
            QString label = n.isEmpty() ? QString("sub_%1").arg(Hex8(target)) : n;
            if (e.kind == "tail_call") label += "  (tail call)";
            QTreeWidgetItem* c = new QTreeWidgetItem(item, {label, QString::number(e.count), Hex8(e.call_site)});
            c->setData(0, Qt::UserRole, (uint)target);
            c->setData(0, Qt::UserRole + 1, callers);
            new QTreeWidgetItem(c, {"..."});
        }
        Tree->resizeColumnToContents(0);
    });
}

// ================================================================ cross-references

XrefsPanel::XrefsPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Title = new QLabel(this);
    Table = MakeTable(this, {"Kind", "From", "Function", "Source", "Count"});
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(Title);
    v->addWidget(Table);
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this](int r, int)
    {
        W->GoTo((u32)Table->item(r, 1)->data(Qt::UserRole).toUInt());
    });
    W->AddRowMenu(Table, [](const QModelIndex& i, u32& a, u32&, int&)
    {
        QVariant v = i.sibling(i.row(), 1).data(Qt::UserRole);
        a = v.toUInt();
        return v.isValid();
    });
}

void XrefsPanel::OnSelection(u32 addr, u32)
{
    if (IsShown()) ShowFor(addr);
}

void XrefsPanel::ShowFor(u32 addr)
{
    Addr = addr;
    int cpu = W->Cpu();
    Title->setText("References to " + W->AddrText(addr));
    std::vector<WBQ::XrefRow> rows;
    for (const WBKB::Xref& x : W->KB().XrefsTo(cpu, addr, W->OverlayContext(addr)))
        rows.push_back({x.Kind, x.From, x.Source, 0});
    const WBKB::Function* f = W->KB().FunctionStarting(cpu, addr, W->OverlayContext(addr));
    u32 fsize = f ? f->Size : 0;
    H->Query<std::vector<WBQ::XrefRow>>([this, addr, cpu, fsize, rows]() mutable
    {
        WBQ::RuntimeXrefs(H->Core(), H->AllTime(), (u32)cpu, addr, fsize, rows);
        return rows;
    }, this, [this, cpu](std::vector<WBQ::XrefRow> rows)
    {
        Table->setRowCount(0);
        for (const WBQ::XrefRow& x : rows)
        {
            int r = Table->rowCount();
            Table->insertRow(r);
            Table->setItem(r, 0, Cell(x.Kind));
            QTableWidgetItem* f = Cell(Hex8(x.From));
            f->setData(Qt::UserRole, (uint)x.From);
            Table->setItem(r, 1, f);
            const WBKB::Function* fn = W->KB().FunctionAt(cpu, x.From, W->OverlayContext(x.From));
            Table->setItem(r, 2, Cell(fn ? W->KB().NameAt(cpu, fn->Addr, W->OverlayContext(fn->Addr)) : QString()));
            Table->setItem(r, 3, Cell(x.Source));
            Table->setItem(r, 4, Cell(x.Count ? QString::number(x.Count) : QString()));
        }
        FitColumns(Table);      // addresses and names, not "0200..."
    });
}
