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

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <QMenu>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QStatusBar>
#include <QSplitter>
#include <QStringEncoder>
#include <QTextBlock>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "Panels.h"
#include "Disasm.h"
#include "Dialogs.h"
#include "WBColors.h"
#include "WBHost.h"
#include "WBQueries.h"
#include "NDS.h"

using namespace melonDS;
using namespace melonDS::WB;

QTableWidget* MakeTable(QWidget* parent, const QStringList& headers);
void FitColumns(QTableWidget* t);
QTableWidgetItem* Cell(const QString& text, const QVariant& data = QVariant());
static QString Hex8(u32 v) { return QString("%1").arg(v, 8, 16, QChar('0')); }
static QString ByteCount(u32 n) { return n == 1 ? QString("1 byte") : QString("%1 bytes").arg(n); }

// ================================================================ coverage map

// ================================================================ write history

WriteHistoryPanel::WriteHistoryPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Note = new QLabel(this);
    SessionPick = new QComboBox(this);
    SessionPick->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    SessionPick->setToolTip("Whose write history: this recording's, or a saved session's. A session saved with a history file "
                            "(or with more writes than its session file keeps) has all of its writes; otherwise its newest 4 million");
    Table = MakeTable(this, {"Frame", "CPU", "Writer", "Function", "Address", "Size", "Old", "New"});
    MoreThanOnce = new QCheckBox("Written more than once", this);
    MoreThanOnce->setChecked(true);
    AddrFilter = new QLineEdit(this);
    AddrFilter->setPlaceholderText("address");
    AddrFilter->setClearButtonEnabled(true);
    AddrFilter->setMaximumWidth(110);
    Groups = MakeTable(this, {"Address", "Writes", "Last writer", "Last frame", "Size"});
    Groups->setSortingEnabled(false);
    QHBoxLayout* top = new QHBoxLayout();
    top->addWidget(Note, 1);
    top->addWidget(AddrFilter);
    top->addWidget(MoreThanOnce);
    top->addWidget(new QLabel("History of", this));
    top->addWidget(SessionPick);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addLayout(top);
    v->addWidget(Groups, 1);
    v->addWidget(new QLabel("Writes at the selected address", this));
    v->addWidget(Table, 1);
    connect(MoreThanOnce, &QCheckBox::toggled, this, [this]() { LastGroupMs = 0; GroupedKey.clear(); LastKey.clear(); Refresh(); });
    connect(AddrFilter, &QLineEdit::textChanged, this, [this]() { LastGroupMs = 0; GroupedKey.clear(); LastKey.clear(); Refresh(); });
    connect(Groups, &QTableWidget::cellDoubleClicked, this, [this](int r, int c)
    {
        if (c != 0 && c != 2) return;
        QTableWidgetItem* it = Groups->item(r, c);
        if (!it || it->data(Qt::UserRole + 1).toInt() == 1) return;
        W->GoTo((u32)it->data(Qt::UserRole).toUInt());
    });
    connect(Groups, &QTableWidget::itemSelectionChanged, this, [this]()
    {
        int r = Groups->currentRow();
        if (r < 0 || !Groups->item(r, 0)) return;
        if (Groups->item(r, 0)->data(Qt::UserRole + 1).toInt() == 1) { HaveDetail = false; Table->setRowCount(0); return; }
        HaveDetail = true;
        DetailAddr = Groups->item(r, 0)->data(Qt::UserRole).toUInt();
        DetailSize = std::max<u32>(1, Groups->item(r, 4)->text().toUInt());
        LastKey.clear();
        Refresh();
    });
    FillSessions();
    connect(SessionPick, &QComboBox::currentIndexChanged, this, [this]() { LastKey.clear(); Refresh(); });
    connect(H, &WBHost::sessionSaved, this, [this]() { FillSessions(); });
    connect(H, &WBHost::romChanged, this, [this]() { FillSessions(); });
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this](int r, int)
    {
        QTableWidgetItem* it = Table->item(r, 2);
        if (it) W->GoTo((u32)it->data(Qt::UserRole).toUInt());
    });
    W->AddRowMenu(Table, [](const QModelIndex& i, u32& a, u32& n, int&)
    {
        bool ok;
        a = i.sibling(i.row(), 4).data().toString().toUInt(&ok, 16);
        n = i.sibling(i.row(), 5).data().toString().toUInt();
        return ok;
    }, [this](QMenu& m, const QModelIndex& i)
    {
        u32 pc = i.sibling(i.row(), 2).data(Qt::UserRole).toUInt();
        m.addAction(QString("Go to the writer at %1").arg(Hex8(pc)), [this, pc]() { W->SetLockToPC(false); W->GoTo(pc); W->ShowPanel("dock_disasm"); });
    });
}

void WriteHistoryPanel::FillSessions()
{
    QString cur = SessionPick->currentData().toString();
    QSignalBlocker block(SessionPick);
    SessionPick->clear();
    SessionPick->addItem("this recording", QString());
    for (const WBApi::SessionInfo& s : H->ListSessions())
    {
        QString what = s.history_file ? QString("all %L1 writes%2").arg(s.history_entries).arg(s.history_wrapped ? " kept" : "")
                                      : QString("newest writes");
        SessionPick->addItem(QString("session %1, frames %2-%3 (%4)").arg(s.saved).arg(s.frame_start).arg(s.frame_end).arg(what), s.path);
    }
    int i = SessionPick->findData(cur);
    SessionPick->setCurrentIndex(std::max(i, 0));
}

void WriteHistoryPanel::Refresh()
{
    if (Busy || !IsShown()) return;
    WBApi::WritesListParams p;
    p.addr = HaveDetail ? DetailAddr : W->SelStart();
    p.size = HaveDetail ? std::max<u32>(DetailSize, 1) : W->SelSize();
    p.limit = 500;
    bool more = MoreThanOnce->isChecked();
    bool regroup = QDateTime::currentMSecsSinceEpoch() - LastGroupMs > 1500;
    u32 focus = 0;
    bool focusOn = false;
    {
        QString t = AddrFilter->text().trimmed();
        if (t.startsWith("0x", Qt::CaseInsensitive)) t = t.mid(2);
        bool ok = false;
        uint v = t.toUInt(&ok, 16);
        if (ok && !t.isEmpty()) { focus = v; focusOn = true; }
    }
    int cpu = W->Cpu();
    QString session = SessionPick->currentData().toString();
    if (!session.isEmpty())
    {
        // a saved session doesn't change: search it again only for another selection
        QString gkey = session + (more ? ":more" : ":all") + ":" + AddrFilter->text().trimmed();
        bool regroup = gkey != GroupedKey;
        QString key = QString("%1 %2 %3").arg(gkey).arg(p.addr).arg(p.size);
        if (key == LastKey) return;
        LastKey = key;
        Busy = true;
        QString where = SessionPick->currentText();
        Note->setText(QString("Searching %1...").arg(where));
        struct SavedPack { WBApi::WritesListResult writes; QString error; std::vector<WriteGroup> groups; int omitted = 0; bool grouped = false; };
        H->Background<SavedPack>([this, p, session, more, regroup, focusOn, focus]()
        {
            SavedPack pack;
            pack.grouped = regroup;
            std::shared_ptr<const SavedHistory> h = H->OpenSavedHistory(session, pack.error);
            if (h && WBQ::WritesIn(h->View, p, pack.writes, pack.error)) pack.writes.history_source = h->Source;
            if (h && regroup)
            {
                std::unordered_map<u32, WriteGroup> m;
                h->View.ForEach([&](const WriteHistoryEntry& e)
                {
                    WriteGroup& g = m[e.addr];
                    g.addr = e.addr;
                    g.count++;
                    g.pc = e.pc;
                    g.frame = e.frame;
                    g.cpu = e.cpu;
                    g.size = e.size;
                });
                for (auto& kv : m)
                {
                    if (more && kv.second.count < 2 && !(focusOn && kv.second.addr == focus)) continue;
                    pack.groups.push_back(kv.second);
                }
                std::sort(pack.groups.begin(), pack.groups.end(), [](const WriteGroup& a, const WriteGroup& b) { return a.count > b.count; });
                pack.omitted = TrimGroups(pack.groups, focusOn, focus);
            }
            return pack;
        }, this, [this, p, cpu, where, gkey](SavedPack pack)
        {
            Busy = false;
            if (!pack.error.isEmpty()) { Note->setText(pack.error); Table->setRowCount(0); return; }
            if (pack.grouped)
            {
                GroupedKey = gkey;
                ApplyGroups(pack.groups, pack.omitted);
            }
            Show(pack.writes, p, cpu, where);
        });
        return;
    }
    LastKey.clear();
    Busy = true;
    struct Pack { WBApi::WritesListResult writes; std::vector<WriteGroup> groups; int omitted = 0; bool grouped = false; };
    H->Query<Pack>([this, p, more, regroup, focusOn, focus]()
    {
        Pack pack;
        pack.grouped = regroup;
        QString e;
        WBQ::Writes(H->Core(), p, pack.writes, e);
        if (!regroup) return pack;
        std::unordered_map<u32, WriteGroup> m;
        H->Core().Rec.ForEachHistory([&](const WriteHistoryEntry& e)
        {
            WriteGroup& g = m[e.addr];
            g.addr = e.addr;
            g.count++;
            g.pc = e.pc;
            g.frame = e.frame;
            g.cpu = e.cpu;
            g.size = e.size;
        });
        for (auto& kv : m)
        {
            if (more && kv.second.count < 2 && !(focusOn && kv.second.addr == focus)) continue;
            pack.groups.push_back(kv.second);
        }
        std::sort(pack.groups.begin(), pack.groups.end(), [](const WriteGroup& a, const WriteGroup& b) { return a.count > b.count; });
        pack.omitted = TrimGroups(pack.groups, focusOn, focus);
        return pack;
    }, this, [this, p, cpu](Pack pack)
    {
        Busy = false;
        if (pack.grouped)
        {
            LastGroupMs = QDateTime::currentMSecsSinceEpoch();
            ApplyGroups(pack.groups, pack.omitted);
        }
        Show(pack.writes, p, cpu, QString());
    });
}

int WriteHistoryPanel::TrimGroups(std::vector<WriteGroup>& groups, bool focusOn, u32 focus)
{
    WriteGroup focused{};
    bool have = false;
    if (focusOn)
        for (const WriteGroup& g : groups)
            if (g.addr == focus) { focused = g; have = true; break; }
    int omitted = 0;
    if (groups.size() > 400)
    {
        omitted = (int)groups.size() - 400;
        groups.resize(400);
    }
    if (have)
    {
        bool in = false;
        for (const WriteGroup& g : groups) if (g.addr == focus) in = true;
        if (!in) groups.push_back(focused);
    }
    return omitted;
}

void WriteHistoryPanel::ApplyGroups(const std::vector<WriteGroup>& groups, int omitted)
{
    Groups->blockSignals(true);
    Groups->setRowCount(0);
    for (const WriteGroup& g : groups)
    {
        int row = Groups->rowCount();
        Groups->insertRow(row);
        QTableWidgetItem* a = Cell(Hex8(g.addr));
        a->setData(Qt::UserRole, (uint)g.addr);
        a->setForeground(WBColor::Muted());
        Groups->setItem(row, 0, a);
        QTableWidgetItem* n = new QTableWidgetItem();
        n->setData(Qt::DisplayRole, (qulonglong)g.count);
        Groups->setItem(row, 1, n);
        QTableWidgetItem* wri = Cell(Hex8(g.pc));
        wri->setData(Qt::UserRole, (uint)g.pc);
        wri->setForeground(WBColor::Muted());
        Groups->setItem(row, 2, wri);
        Groups->setItem(row, 3, Cell(QString::number(g.frame)));
        Groups->setItem(row, 4, Cell(QString::number(g.size)));
    }
    if (omitted > 0)
    {
        int row = Groups->rowCount();
        Groups->insertRow(row);
        QTableWidgetItem* a = Cell(QString("%1 more").arg(omitted));
        a->setData(Qt::UserRole + 1, 1);
        a->setForeground(WBColor::Muted());
        Groups->setItem(row, 0, a);
    }
    Groups->blockSignals(false);
    FitColumns(Groups);
}

void WriteHistoryPanel::Show(const WBApi::WritesListResult& r, const WBApi::WritesListParams& p, int cpu, const QString& where)
{
    {
        if (where.isEmpty())
            // with write history off the banner above says so; the last writer still shows
            Note->setText(QString("Writes to %1 (%2)%3").arg(W->AddrText(p.addr)).arg(ByteCount(p.size))
                          .arg(r.history_enabled ? (r.truncated ? ", newest 500 shown" : "") : " - the last writer only"));
        else
            Note->setText(QString("Writes to %1 (%2) in the %3, %L4 in all%5%6").arg(W->AddrText(p.addr)).arg(ByteCount(p.size)).arg(where)
                          .arg(r.total).arg(r.truncated ? ", newest 500 shown" : "")
                          .arg(r.history_wrapped ? QString(", complete from frame %1").arg(r.oldest_frame) : QString()));
        Table->setRowCount(0);
        auto add = [&](u32 frame, const QString& c, u32 pc, u32 addr, int size, const QString& oldv, const QString& newv)
        {
            int row = Table->rowCount();
            Table->insertRow(row);
            Table->setItem(row, 0, Cell(QString::number(frame)));
            Table->setItem(row, 1, Cell(c));
            QTableWidgetItem* wi = Cell(Hex8(pc));
            wi->setData(Qt::UserRole, (uint)pc);
            Table->setItem(row, 2, wi);
            const WBKB::Function* f = c.startsWith("dma") ? nullptr : W->KB().FunctionAt(cpu, pc, W->OverlayContext(pc));
            Table->setItem(row, 3, Cell(f ? W->KB().NameAt(cpu, f->Addr, W->OverlayContext(f->Addr)) : QString()));
            Table->setItem(row, 4, Cell(Hex8(addr)));
            Table->setItem(row, 5, Cell(size > 0 ? QString::number(size) : QString()));
            Table->setItem(row, 6, Cell(oldv));
            Table->setItem(row, 7, Cell(newv));
        };
        for (auto it = r.entries.rbegin(); it != r.entries.rend(); ++it)
            add(it->frame, it->cpu, it->pc, it->addr, it->size, QString::number(it->old_value, 16), QString::number(it->new_value, 16));
        if (r.entries.empty() && !r.last_writer_cpu.isEmpty())
            add(r.last_write_frame, r.last_writer_cpu, r.last_writer_pc, p.addr, 0, "", "(last writer)");
        FitColumns(Table);
    }
}

// ================================================================ data origin

DataOriginPanel::DataOriginPanel(WorkbenchWindow* w) : WBPanel(w)
{
    View = new QTextBrowser(this);
    View->setOpenLinks(false);
    connect(View, &QTextBrowser::anchorClicked, this, [this](const QUrl& u)
    {
        // how:<label> shows that label's whole tree
        if (u.scheme() == "how") { FocusLabel = u.path().toUInt(); LastHtml.clear(); Refresh(); return; }
        FollowLink(W, u);
    });
    Sources = MakeTable(this, {"Id", "Kind", "Enabled", "Source"});     // the source last: it takes the spare width
    QPushButton* track = new QPushButton("Track selection", this);
    QPushButton* trackReg = new QPushButton("Track register...", this);
    QPushButton* remove = new QPushButton("Remove source", this);
    QHBoxLayout* b = new QHBoxLayout();
    b->addWidget(track);
    b->addWidget(trackReg);
    b->addWidget(remove);
    b->addStretch();
    QSplitter* sp = new QSplitter(Qt::Horizontal, this);
    sp->addWidget(View);
    QWidget* right = new QWidget(sp);
    QVBoxLayout* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(Sources);
    rv->addLayout(b);
    sp->addWidget(right);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(sp);
    connect(track, &QPushButton::clicked, this, [this]() { W->TrackValue(W->SelStart(), W->SelSize()); });
    connect(trackReg, &QPushButton::clicked, this, [this]()
    {
        bool ok;
        int r = QInputDialog::getInt(this, "Track register", "Register number (0-15):", 0, 0, 15, 1, &ok);
        if (ok) W->TrackRegister(r);
    });
    connect(remove, &QPushButton::clicked, this, [this]()
    {
        int r = Sources->currentRow();
        if (r < 0) return;
        u32 id = Sources->item(r, 0)->text().toUInt();
        H->RunSync([this, id]() { H->Core().Taint.RemoveSource(id); });
        Refresh();
    });
}

void DataOriginPanel::Refresh()
{
    if (Busy || !IsShown()) return;
    Busy = true;
    WBApi::OriginParams p;
    p.addr = W->SelStart();
    p.size = std::min<u32>(W->SelSize(), 4096);
    p.cpu = W->Cpu() ? "arm7" : "arm9";
    struct Res { WBApi::OriginResult O; std::vector<DataSource> Src; std::map<u32, WBApi::OriginLabelResult> Trees; };
    WBQ::ArchiveCtx arc {H->Archive(), H->ArchiveStatus()};
    WBQ::ContainerCtx cont {H->Containers(), H->ContainerStatus()};
    u32 focus = FocusLabel;
    H->Query<Res>([this, p, arc, cont, focus]()
    {
        Res r;
        QString e;
        WBQ::Origin(H->Core(), p, r.O, e, arc, cont);
        r.Src = H->Core().Taint.Sources();
        // how the merged labels were made: the first few labels, and a label asked for in full
        for (const WBApi::TaintInfo& t : r.O.taint)
            if (!t.how.isEmpty() && !r.Trees.count(t.label) && r.Trees.size() < 8)
            {
                WBApi::OriginLabelParams lp;
                lp.label = t.label;
                lp.max_nodes = 64;
                WBQ::LabelTree(H->Core(), lp, r.Trees[t.label], e);
            }
        if (focus)
        {
            WBApi::OriginLabelParams lp;
            lp.label = focus;
            lp.max_nodes = 512;
            if (!WBQ::LabelTree(H->Core(), lp, r.Trees[focus | 0x80000000u], e)) r.Trees.erase(focus | 0x80000000u);
        }
        return r;
    }, this, [this, p, focus](Res r)
    {
        Busy = false;
        int cpu = W->Cpu();
        auto link = [&](u32 a)
        {
            QString n = W->KB().NameAt(cpu, a, W->OverlayContext(a), true);
            return QString("<a href=\"addr:%1\">%1</a>%2").arg(Hex8(a), n.isEmpty() ? QString() : " &lt;" + n.toHtmlEscaped() + "&gt;");
        };
        QString h = QString("<b>Where %1 (%2) came from</b><br>").arg(link(p.addr)).arg(ByteCount(p.size));
        if (!r.O.archive.empty())
            h += "<p><b>Archived file</b> <span style='color:%MUTED%'>(these bytes equal a file in the game's archive)</span><br>" +
                 ArchiveSegmentsHtml(W, r.O.archive, false) + "<p>";
        else if (r.O.archive_status == "indexing")
            h += "<p style='color:%MUTED%'>The game archive is still being indexed.<p>";
        if (!r.O.containers.empty())
            h += "<p><b>File in a container</b> <span style='color:%MUTED%'>(these bytes equal a file inside a NARC or SDAT)</span><br>" +
                 ArchiveSegmentsHtml(W, r.O.containers, false, true) + "<p>";
        if (r.O.chain.empty()) h += "<i>No provenance recorded for these bytes.</i> Card reads, BIOS decompression and DMA are tracked while recording.<br>";
        int step = 1;
        for (const auto& c : r.O.chain)
        {
            h += QString("<p>%1. <b>%2</b>").arg(step++).arg(c.kind);
            if (!c.file_name.isEmpty()) h += QString(" from ROM file <b>%1</b>").arg(c.file_name.toHtmlEscaped());
            if (c.kind == "card_read" || c.kind == "boot_load") h += QString("<br>ROM 0x%1 (+0x%2) to %3").arg(c.src_addr, 0, 16).arg(c.src_size, 0, 16).arg(link(c.dst_addr));
            else if (c.src_size) h += QString("<br>%1 (+0x%2) to %3 (+0x%4)").arg(link(c.src_addr)).arg(c.src_size, 0, 16).arg(link(c.dst_addr)).arg(c.dst_size, 0, 16);
            else h += QString("<br>to %1 (+0x%2)").arg(link(c.dst_addr)).arg(c.dst_size, 0, 16);     // computed: no source range
            if (c.pc) h += "<br>by code at " + link(c.pc);
            if (c.frame) h += QString(", frame %1").arg(c.frame);
            if (!c.detail.isEmpty()) h += "<br><span style='color:%MUTED%'>" + c.detail.toHtmlEscaped() + "</span>";
            if (!c.archive_member.isEmpty()) h += "<br>archived file: <b>" + c.archive_member.toHtmlEscaped() + "</b>";
        }
        if (!r.O.file_reads.empty())
        {
            h += "<p><b>Where the labelled ROM files were loaded</b><br>";
            for (const auto& fr : r.O.file_reads)
            {
                const WBKB::Function* lf = W->KB().FunctionAt(cpu, fr.pc, W->OverlayContext(fr.pc));
                QString fn = lf ? W->KB().NameAt(cpu, lf->Addr, W->OverlayContext(lf->Addr)) : QString();
                h += QString("<b>%1</b> (%2, %3 bytes) to %4 by %5%6, frame %7<br>")
                    .arg(fr.file_name.toHtmlEscaped(), (fr.archive_member.isEmpty() ? fr.detail : fr.detail + ": " + fr.archive_member).toHtmlEscaped()).arg(fr.src_size)
                    .arg(link(fr.dst_addr)).arg(link(fr.pc)).arg(fn.isEmpty() ? QString() : " in " + fn.toHtmlEscaped()).arg(fr.frame);
            }
        }
        h += "<p><b>Data origin labels</b>";
        if (!r.O.origin_enabled) h += " <span style='color:%MUTED%'>(data origin tracking is off)</span>";
        h += "<br>";
        if (r.O.taint.empty()) h += "<i>no labels</i>";
        if (r.O.state_loaded_frame && r.O.origin_enabled)
            h += QString("<p style='background: palette(alternate-base)'><b>Origin unknown for unlabelled bytes:</b> a savestate was loaded at frame %1, "
                         "and only memory that still matches the ROM (code, overlays, archived files) was labelled again. "
                         "Data loaded or computed before the state was saved has no origin; use Recording &gt; Reset game with tracking "
                         "to follow it from boot.").arg(r.O.state_loaded_frame);
        // how a label naming several files was made: which instruction joined which labels, when
        auto howText = [](const QString& how)
        {
            if (how == "computed") return QString("computed");
            if (how == "table_lookup") return QString("table lookup");
            if (how == "mixed_bytes") return QString("loaded from bytes of different origin");
            if (how == "math_unit") return QString("math unit");
            if (how == "copy") return QString("copied range");
            if (how == "source_added") return QString("source added");
            return how;
        };
        auto tree = [&](const WBApi::OriginLabelResult& tr, int maxDepth)
        {
            QString out;
            std::function<void(u32, int)> node = [&](u32 i, int depth)
            {
                const WBApi::LabelNode& n = tr.nodes[i];
                if (n.how == "source")
                {
                    out += "<li>" + (n.sources.empty() ? QString("?") : n.sources[0].toHtmlEscaped()) + "</li>";
                    return;
                }
                out += QString("<li>%1 files: <b>%2</b> at %3 (%4), frame %5").arg(n.source_count).arg(howText(n.how)).arg(link(n.pc)).arg(n.cpu).arg(n.frame);
                if (n.steps > 1)
                    out += QString(", <i>a running total</i>: that instruction added to its own result %1 times from frame %2").arg(n.steps).arg(n.frame_first);
                if (depth >= maxDepth) out += QString(" <a href=\"how:%1\">show how</a>").arg(n.label);
                else
                {
                    out += "<ul>";
                    int shown = 0;
                    for (u32 part : n.parts)
                    {
                        if (shown++ == 12) break;
                        node(part, depth + 1);
                    }
                    u32 more = (u32)n.parts.size() + n.parts_more - std::min<u32>((u32)n.parts.size(), 12);
                    if (more) out += QString("<li>and %1 more <a href=\"how:%2\">show all</a></li>").arg(more).arg(n.label);
                    out += "</ul>";
                }
                out += "</li>";
            };
            if (!tr.nodes.empty()) node(0, 0);
            return "<ul style='margin-top:0'>" + out + "</ul>";
        };
        auto focused = r.Trees.find(focus | 0x80000000u);
        if (focus && focused != r.Trees.end())
            h += QString("<p><b>How label %1 was made</b> <span style='color:%MUTED%'>(all of it; select again to go back)</span>").arg(focus)
                 + tree(focused->second, 64);
        for (const auto& t : r.O.taint)
        {
            QStringList s;
            for (const QString& x : t.sources) s << x.toHtmlEscaped();
            h += QString("%1 +%2: <b>%3</b><br>").arg(Hex8(t.addr)).arg(t.size).arg(s.join(", "));
            auto it = r.Trees.find(t.label);
            if (it != r.Trees.end()) h += tree(it->second, 3);
        }
        if (h != LastHtml) { LastHtml = h; View->setHtml(WBColor::ApplyHtmlColors(h)); }
        Sources->setRowCount(0);
        for (const DataSource& s : r.Src)
        {
            if (s.Kind == SourceKind::CodeConstant) continue;
            int row = Sources->rowCount();
            Sources->insertRow(row);
            Sources->setItem(row, 0, Cell(QString::number(s.Id)));
            Sources->setItem(row, 1, Cell(SourceKindName((u8)s.Kind)));
            Sources->setItem(row, 2, Cell(s.Enabled ? "yes" : "no"));
            Sources->setItem(row, 3, Cell(QString::fromStdString(s.Name)));
        }
        FitColumns(Sources);
    });
}

// ================================================================ game archive

// one block per run of RAM bytes that equal an archived file (Data origin and Inspector panels)
QString ArchiveSegmentsHtml(WorkbenchWindow* W, const std::vector<WBApi::ArchiveSegment>& segs, bool compact, bool containers)
{
    auto cont = containers ? W->Host()->Containers() : nullptr;
    WBKB::KnowledgeStore& kb = W->KB();
    int cpu = W->Cpu();
    auto link = [&](u32 a)
    {
        QString n = kb.NameAt(cpu, a, W->OverlayContext(a), true);
        return QString("<a href=\"addr:%1\">%1</a>%2").arg(Hex8(a), n.isEmpty() ? QString() : " &lt;" + n.toHtmlEscaped() + "&gt;");
    };
    auto inFunc = [&](u32 pc)
    {
        const WBKB::Function* f = kb.FunctionAt(cpu, pc, W->OverlayContext(pc));
        if (!f) return QString();
        QString n = kb.NameAt(cpu, f->Addr, W->OverlayContext(f->Addr));
        return " in " + (n.isEmpty() ? Hex8(f->Addr) : n.toHtmlEscaped());
    };
    QString h;
    for (const WBApi::ArchiveSegment& s : segs)
    {
        if (!containers) h += QString("<b>%1</b> <a href=\"arc:%2\">[archive]</a><br>").arg(s.path.toHtmlEscaped()).arg(s.member_index);
        else
        {
            int ci = -1, mi = -1;
            int global = cont && cont->Find(s.path.toStdString(), ci, mi) ? (int)cont->Containers()[ci].Members[mi].Global : -1;
            h += "<b>" + s.path.toHtmlEscaped() + "</b>" + (global >= 0 ? QString(" <a href=\"cont:%1\">[containers]</a>").arg(global) : QString()) + "<br>";
        }
        h += QString("<a href=\"sel:%1:%2\">%1-%3</a> = file bytes +0x%4..+0x%5 of 0x%6")
            .arg(Hex8(s.addr)).arg(s.size, 0, 16).arg(Hex8(s.addr + s.size))
            .arg(s.member_offset, 0, 16).arg(s.member_offset + s.size, 0, 16).arg(s.member_size, 0, 16);
        if (!s.alternatives.empty())
        {
            QStringList alts;
            for (const QString& a : s.alternatives) alts << a.toHtmlEscaped();
            if (compact && alts.size() > 2) { int n = (int)alts.size(); alts = alts.mid(0, 2); alts << QString("%1 more").arg(n - 2); }
            h += "<br><span style='color:%MUTED%'>same bytes in: " + alts.join(", ") + "</span>";
        }
        if (s.writer_pc) h += "<br>written to RAM by " + link(s.writer_pc) + inFunc(s.writer_pc);
        for (size_t i = 0; i < s.loads.size() && i < (compact ? 1u : 3u); i++)
        {
            const WBApi::ProvenanceInfo& l = s.loads[i];
            h += QString("<br>read from the card by %1%2, frame %3 <span style='color:%MUTED%'>(%4)</span>")
                .arg(link(l.pc), inFunc(l.pc)).arg(l.frame).arg(l.archive_member.toHtmlEscaped());
        }
        if (s.loads.empty() && !compact) h += "<br><span style='color:%MUTED%'>no card reads of this file recorded (recording was off when it loaded)</span>";
        h += "<br>";
    }
    return h;
}

void ShowGraphics(WorkbenchWindow* W, const QString& path)
{
    auto cat = W->Host()->Graphics();
    if (!cat || cat->Find(path.toStdString()) < 0)
    {
        W->Notice(cat ? path + " is no graphics file (NCLR, NCGR, NSCR, NCER, NSBTX or NSBMD)" : QString("The graphics files are still being indexed"), 5000);
        return;
    }
    W->ShowPanel("dock_graphics");
    if (GraphicsPanel* g = W->FindPanel<GraphicsPanel>()) g->ShowFile(path);
}

// handles addr:, sel:, arc:, cont: and gfx: links of the HTML panels
void FollowLink(WorkbenchWindow* W, const QUrl& u)
{
    QString t = u.toString();
    QStringList p = t.split(':');
    if (p[0] == "addr" && p.size() >= 2) W->GoTo(p[1].toUInt(nullptr, 16));
    else if (p[0] == "sel" && p.size() >= 3)
    {
        u32 a = p[1].toUInt(nullptr, 16), n = p[2].toUInt(nullptr, 16);
        W->GoTo(a);
        W->SetSelection(a, std::max<u32>(n, 1));
    }
    else if (p[0] == "arc" && p.size() >= 2) W->LocateArchived(p[1].toInt());
    else if (p[0] == "cont" && p.size() >= 2) W->LocateContained(p[1].toInt());
    else if (p[0] == "gfx" && p.size() >= 2)
    {
        // gfx:<addr> opens the graphics file whose RAM copy holds addr
        W->ShowPanel("dock_graphics");
        if (GraphicsPanel* g = W->FindPanel<GraphicsPanel>()) g->ShowRam(p[1].toUInt(nullptr, 16), 0);
    }
}

namespace
{
class NumItem : public QTableWidgetItem
{
public:
    NumItem(qulonglong v, bool hex) : QTableWidgetItem(hex ? QString("0x%1").arg(v, 0, 16) : QString::number(v)), V(v) {}
    bool operator<(const QTableWidgetItem& o) const override
    {
        if (const NumItem* n = dynamic_cast<const NumItem*>(&o)) return V < n->V;
        return QTableWidgetItem::operator<(o);
    }
    qulonglong V;
};
}

ArchivePanel::ArchivePanel(WorkbenchWindow* w) : WBPanel(w)
{
    Status = new QLabel(this);
    Status->setWordWrap(true);
    Filter = new QLineEdit(this);
    Filter->setPlaceholderText("filter by path, e.g. cars/stadium or .nsbmd");
    QPushButton* loc = new QPushButton("Locate in RAM", this);
    loc->setToolTip("Find where the selected file is in RAM and which code read it from the card (Enter)");
    QPushButton* track = new QPushButton("Track this file", this);
    track->setToolTip("Data origin: label every value derived from this file's bytes with its name");
    Table = MakeTable(this, {"Path", "Size", "Packed", "Method", "ROM offset", "VFS entry", "Index"});
    Result = new QTextBrowser(this);
    Result->setOpenLinks(false);
    QPushButton* gfx = new QPushButton("Show graphics", this);
    gfx->setToolTip("Open the selected palette, tiles, map, cells or textures in the Graphics panel");
    QHBoxLayout* top = new QHBoxLayout();
    top->addWidget(Filter, 1);
    top->addWidget(loc);
    top->addWidget(track);
    top->addWidget(gfx);
    QSplitter* sp = new QSplitter(Qt::Horizontal, this);
    sp->addWidget(Table);
    sp->addWidget(Result);
    sp->setStretchFactor(0, 3);
    sp->setStretchFactor(1, 2);
    sp->setSizes({600, 400});
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(Status);
    v->addLayout(top);
    v->addWidget(sp, 1);

    connect(Filter, &QLineEdit::textChanged, this, [this]() { ApplyFilter(); });
    connect(Filter, &QLineEdit::returnPressed, this, [this]()
    {
        // Enter in the filter locates the first visible file
        for (int r = 0; r < Table->rowCount(); r++)
            if (!Table->isRowHidden(r)) { Table->selectRow(r); Locate(Table->item(r, 0)->data(Qt::UserRole).toInt()); break; }
    });
    auto current = [this]() { int r = Table->currentRow(); return r < 0 ? -1 : Table->item(r, 0)->data(Qt::UserRole).toInt(); };
    connect(loc, &QPushButton::clicked, this, [this, current]() { if (current() >= 0) Locate(current()); });
    connect(track, &QPushButton::clicked, this, [this, current]()
    {
        auto arc = H->Archive();
        int i = current();
        if (!arc || i < 0) return;
        DataSource ds;
        ds.Kind = SourceKind::RomFile;
        ds.FileId = arc->ArchiveFileId;
        ds.Addr = (u32)i + 1;
        ds.Name = arc->ArchivePath + ":" + arc->Members()[i].Path;
        H->RunSync([this, ds]() { H->Core().Taint.AddSource(ds); });
        W->Notice(H->Status().Origin ? "Tracking " + QString::fromStdString(arc->Members()[i].Path)
                                                       : "Tracking " + QString::fromStdString(arc->Members()[i].Path) + " once Data origin is on", 5000);
    });
    connect(gfx, &QPushButton::clicked, this, [this, current]()
    {
        auto arc = H->Archive();
        int i = current();
        if (!arc || i < 0) return;
        ShowGraphics(W, QString::fromStdString(arc->ArchivePath + ":" + arc->Members()[i].Path));
    });
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this, current](int, int) { if (current() >= 0) Locate(current()); });
    QShortcut* enter = new QShortcut(QKeySequence(Qt::Key_Return), Table, nullptr, nullptr, Qt::WidgetShortcut);
    connect(enter, &QShortcut::activated, this, [this, current]() { if (current() >= 0) Locate(current()); });
    connect(Result, &QTextBrowser::anchorClicked, this, [this](const QUrl& u) { FollowLink(W, u); });
    connect(H, &WBHost::archiveChanged, this, &ArchivePanel::Reload);
    connect(H, &WBHost::romChanged, this, &ArchivePanel::Reload);
    Reload();
}

void ArchivePanel::Reload()
{
    auto arc = H->Archive();
    QString st = H->ArchiveStatus();
    Table->setSortingEnabled(false);
    Table->setRowCount(0);
    Result->clear();
    Shown = -1;
    if (!arc)
    {
        Status->setText(st == "none" ? "No game archive detected in this ROM (looked for TrackMania DS / Octane engine vfs.zip, vfs.bin and vfs.dat)."
                      : st == "indexing" ? "Unpacking and indexing the game archive..." : "Game archive: " + st);
        return;
    }
    Status->setText(QString::fromStdString(arc->Summary));
    const auto& ms = arc->Members();
    Table->setRowCount((int)ms.size());
    for (int r = 0; r < (int)ms.size(); r++)
    {
        const WBArc::Member& m = ms[r];
        QTableWidgetItem* p = Cell(QString::fromStdString(m.Path));
        p->setData(Qt::UserRole, (int)m.Index);
        if (!m.Ok) p->setForeground(WBColor::Red());
        Table->setItem(r, 0, p);
        Table->setItem(r, 1, new NumItem(m.Size, false));
        Table->setItem(r, 2, new NumItem(m.PackedSize, false));
        Table->setItem(r, 3, Cell(m.Method == 0 ? "stored" : m.Method == 8 ? "deflate" : QString::number(m.Method)));
        Table->setItem(r, 4, new NumItem(arc->ArchiveRomStart + m.DataOffset, true));
        Table->setItem(r, 5, m.VfsEntry >= 0 ? new NumItem((qulonglong)m.VfsEntry, false) : Cell(""));
        Table->setItem(r, 6, new NumItem(m.Index, false));
    }
    Table->setSortingEnabled(true);
    Table->sortByColumn(6, Qt::AscendingOrder);
    Table->setColumnWidth(0, 340);
    for (int c = 1; c < 6; c++) Table->setColumnWidth(c, 80);
    ApplyFilter();
}

void ArchivePanel::ApplyFilter()
{
    QString f = Filter->text().trimmed();
    for (int r = 0; r < Table->rowCount(); r++)
        Table->setRowHidden(r, !f.isEmpty() && !Table->item(r, 0)->text().contains(f, Qt::CaseInsensitive));
}

QStringList ArchivePanel::Paths() const
{
    QStringList out;
    if (auto arc = H->Archive())
        for (const WBArc::Member& m : arc->Members()) out << QString::fromStdString(m.Path);
    return out;
}

int ArchivePanel::IndexOf(const QString& path) const
{
    auto arc = H->Archive();
    return arc ? arc->FindPath(path.trimmed().toStdString()) : -1;
}

void ArchivePanel::Locate(int member)
{
    auto arc = H->Archive();
    if (!arc || member < 0 || member >= (int)arc->Members().size()) return;
    Shown = member;
    for (int r = 0; r < Table->rowCount(); r++)
        if (Table->item(r, 0)->data(Qt::UserRole).toInt() == member)
        {
            if (Table->isRowHidden(r)) { Filter->clear(); }
            Table->selectRow(r);
            Table->scrollToItem(Table->item(r, 0));
            break;
        }
    Result->setHtml("<i>Searching RAM...</i>");
    WBQ::ArchiveCtx ctx {arc, H->ArchiveStatus()};
    WBApi::ArchiveLocateParams p;
    p.index = member;
    struct Res { bool Ok; WBApi::ArchiveLocateResult R; QString Err; };
    H->Query<Res>([this, ctx, p]()
    {
        Res r;
        r.Ok = WBQ::ArchiveLocate(H->Core(), ctx, p, r.R, r.Err);
        return r;
    }, this, [this, member](Res r)
    {
        if (member != Shown) return;
        if (!r.Ok) { Result->setHtml("<i>" + r.Err.toHtmlEscaped() + "</i>"); return; }
        const WBApi::ArchiveMemberInfo& m = r.R.member;
        WBKB::KnowledgeStore& kb = W->KB();
        int cpu = W->Cpu();
        auto link = [&](u32 a)
        {
            QString n = kb.NameAt(cpu, a, W->OverlayContext(a), true);
            return QString("<a href=\"addr:%1\">%1</a>%2").arg(Hex8(a), n.isEmpty() ? QString() : " &lt;" + n.toHtmlEscaped() + "&gt;");
        };
        auto inFunc = [&](u32 pc)
        {
            const WBKB::Function* f = kb.FunctionAt(cpu, pc, W->OverlayContext(pc));
            if (!f) return QString();
            QString n = kb.NameAt(cpu, f->Addr, W->OverlayContext(f->Addr));
            return " in " + (n.isEmpty() ? Hex8(f->Addr) : n.toHtmlEscaped());
        };
        QString h = QString("<b>%1</b><br>%2 bytes, %3 (%4 bytes) at ROM 0x%5, file index %6")
            .arg(m.path.toHtmlEscaped()).arg(m.size).arg(m.method).arg(m.packed_size).arg(m.rom_offset, 0, 16).arg(m.index);
        if (m.vfs_entry >= 0) h += QString(", vfs.bin entry %1").arg(m.vfs_entry);
        if (!m.duplicate_of.isEmpty()) h += "<br><span style='color:%MUTED%'>identical to " + m.duplicate_of.toHtmlEscaped() + "</span>";
        if (!m.readable) h += QString("<br><span style='color:%1'>content could not be unpacked</span>").arg(WBColor::Red().name());
        h += "<p><b>In main RAM now</b><br>";
        if (r.R.ram.empty()) h += "<i>not in RAM</i> (not loaded yet, or freed or overwritten)<br>";
        for (const WBApi::ArchiveSegment& s : r.R.ram)
        {
            h += QString("<a href=\"sel:%1:%2\">%1-%3</a> = bytes +0x%4..+0x%5 (%6%)")
                .arg(Hex8(s.addr)).arg(s.size, 0, 16).arg(Hex8(s.addr + s.size))
                .arg(s.member_offset, 0, 16).arg(s.member_offset + s.size, 0, 16)
                .arg(m.size ? (int)((u64)s.size * 100 / m.size) : 100);
            if (s.best_match != s.path) h += ", <span style='color:%MUTED%'>part of a longer match with " + s.best_match.toHtmlEscaped() + "</span>";
            else if (!s.alternatives.empty())
                h += QString(", <span style='color:%MUTED%'>same bytes in %1 other file%2, e.g. %3</span>")
                    .arg(s.alternatives.size()).arg(s.alternatives.size() > 1 ? "s" : "").arg(s.alternatives[0].toHtmlEscaped());
            if (s.writer_pc) h += ", written by " + link(s.writer_pc) + inFunc(s.writer_pc);
            h += "<br>";
        }
        h += "<p><b>Read from the card</b><br>";
        if (r.R.loads.empty()) h += "<i>no card reads of this file recorded</i> (turn recording on before the game loads it)<br>";
        for (const WBApi::ProvenanceInfo& l : r.R.loads)
            h += QString("frame %1: %2%3 read 0x%4 bytes to %5 <span style='color:%MUTED%'>(%6)</span><br>")
                .arg(l.frame).arg(link(l.pc), inFunc(l.pc)).arg(l.src_size, 0, 16).arg(link(l.dst_addr)).arg(l.archive_member.toHtmlEscaped());
        Result->setHtml(WBColor::ApplyHtmlColors(h));
        u32 addr = r.R.ram.empty() ? 0 : r.R.ram[0].addr, size = r.R.ram.empty() ? 0 : r.R.ram[0].size;
        emit located(member, addr, size);
    });
}

// ================================================================ NARC and SDAT containers

ContainersPanel::ContainersPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Status = new QLabel(this);
    Status->setWordWrap(true);
    Filter = new QLineEdit(this);
    Filter->setPlaceholderText("filter the files by name, e.g. .ncgr or SEQ_");
    QPushButton* loc = new QPushButton("Locate in RAM", this);
    loc->setToolTip("Find where the selected file is in RAM and which code read it from the card (Enter)");
    QPushButton* track = new QPushButton("Track this file", this);
    track->setToolTip("Data origin: label every value derived from this file's bytes with its name");
    List = MakeTable(this, {"Container", "Format", "Packed", "Files", "Nested"});
    List->horizontalHeaderItem(2)->setToolTip("The whole file is packed in this BIOS compression: its files are found in RAM by content only");
    List->horizontalHeaderItem(4)->setToolTip("Files inside containers nested in this one");
    Table = MakeTable(this, {"Name", "Kind", "Size", "Stored", "Form", "ROM offset", "Index"});
    Table->horizontalHeaderItem(1)->setToolTip("From the file's first bytes (its magic)");
    Table->horizontalHeaderItem(3)->setToolTip("Size as stored in the container (packed on its own when Form says so)");
    Table->horizontalHeaderItem(5)->setToolTip("ROM offset of the stored bytes; none inside a container packed whole");
    Result = new QTextBrowser(this);
    Result->setOpenLinks(false);
    QPushButton* gfx = new QPushButton("Show graphics", this);
    gfx->setToolTip("Open the selected palette, tiles, map, cells or textures in the Graphics panel");
    QHBoxLayout* top = new QHBoxLayout();
    top->addWidget(Filter, 1);
    top->addWidget(loc);
    top->addWidget(track);
    top->addWidget(gfx);
    QSplitter* left = new QSplitter(Qt::Vertical, this);
    left->addWidget(List);
    left->addWidget(Table);
    left->setStretchFactor(0, 1);
    left->setStretchFactor(1, 2);
    QSplitter* sp = new QSplitter(Qt::Horizontal, this);
    sp->addWidget(left);
    sp->addWidget(Result);
    sp->setStretchFactor(0, 3);
    sp->setStretchFactor(1, 2);
    sp->setSizes({600, 400});
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(Status);
    v->addLayout(top);
    v->addWidget(sp, 1);

    connect(List, &QTableWidget::currentCellChanged, this, [this](int r, int, int, int)
    {
        if (r >= 0 && List->item(r, 0)) ShowContainer(List->item(r, 0)->data(Qt::UserRole).toInt());
    });
    connect(Filter, &QLineEdit::textChanged, this, [this]() { ApplyFilter(); });
    connect(Filter, &QLineEdit::returnPressed, this, [this]()
    {
        // Enter in the filter locates the first visible file
        for (int r = 0; r < Table->rowCount(); r++)
            if (!Table->isRowHidden(r)) { Table->selectRow(r); Locate(Table->item(r, 0)->data(Qt::UserRole).toInt()); break; }
    });
    auto current = [this]() { int r = Table->currentRow(); return r < 0 ? -1 : Table->item(r, 0)->data(Qt::UserRole).toInt(); };
    connect(loc, &QPushButton::clicked, this, [this, current]() { if (current() >= 0) Locate(current()); });
    connect(track, &QPushButton::clicked, this, [this, current]()
    {
        auto cont = H->Containers();
        auto [ci, mi] = cont ? cont->FromGlobal(current()) : std::make_pair(-1, -1);
        if (ci < 0) return;
        DataSource ds;
        ds.Kind = SourceKind::RomFile;
        ds.FileId = cont->Containers()[ci].FileId;
        ds.Addr = (u32)mi + 1;
        ds.Name = cont->FullName(ci, mi);
        H->RunSync([this, ds]() { H->Core().Taint.AddSource(ds); });
        QString name = QString::fromStdString(ds.Name);
        W->Notice(H->Status().Origin ? "Tracking " + name : "Tracking " + name + " once Data origin is on", 5000);
    });
    connect(gfx, &QPushButton::clicked, this, [this, current]()
    {
        auto cont = H->Containers();
        auto [ci, mi] = cont ? cont->FromGlobal(current()) : std::make_pair(-1, -1);
        if (ci >= 0) ShowGraphics(W, QString::fromStdString(cont->FullName(ci, mi)));
    });
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this, current](int, int) { if (current() >= 0) Locate(current()); });
    QShortcut* enter = new QShortcut(QKeySequence(Qt::Key_Return), Table, nullptr, nullptr, Qt::WidgetShortcut);
    connect(enter, &QShortcut::activated, this, [this, current]() { if (current() >= 0) Locate(current()); });
    connect(Result, &QTextBrowser::anchorClicked, this, [this](const QUrl& u) { FollowLink(W, u); });
    connect(H, &WBHost::containersChanged, this, &ContainersPanel::Reload);
    connect(H, &WBHost::romChanged, this, &ContainersPanel::Reload);
    Reload();
}

void ContainersPanel::Reload()
{
    auto cont = H->Containers();
    QString st = H->ContainerStatus();
    List->setSortingEnabled(false);
    List->setRowCount(0);
    Table->setRowCount(0);
    Result->clear();
    Current = Shown = -1;
    if (!cont)
    {
        Status->setText(st == "indexing" ? "Reading the NARC and SDAT files..." : "No NARC or SDAT files in this ROM.");
        return;
    }
    Status->setText(QString::fromStdString(cont->Summary));
    const auto& cs = cont->Containers();
    List->setRowCount((int)cs.size());
    for (int r = 0; r < (int)cs.size(); r++)
    {
        const WBArc::Container& c = cs[r];
        List->setItem(r, 0, Cell(QString::fromStdString(c.Path), r));
        List->setItem(r, 1, Cell(QString::fromStdString(c.Format).toUpper()));
        List->setItem(r, 2, Cell(QString::fromStdString(c.Packed)));
        List->setItem(r, 3, new NumItem(c.Members.size(), false));
        List->setItem(r, 4, new NumItem((qulonglong)c.Nested, false));
    }
    List->setSortingEnabled(true);
    List->sortByColumn(0, Qt::AscendingOrder);
    List->setColumnWidth(0, 300);
    if (List->rowCount()) List->selectRow(0);
}

void ContainersPanel::ShowContainer(int container)
{
    auto cont = H->Containers();
    if (!cont || container < 0 || container >= (int)cont->Containers().size() || container == Current) return;
    Current = container;
    const WBArc::Container& c = cont->Containers()[container];
    Table->setSortingEnabled(false);
    Table->setRowCount((int)c.Members.size());
    for (int r = 0; r < (int)c.Members.size(); r++)
    {
        const WBArc::ContainerMember& m = c.Members[r];
        QTableWidgetItem* p = Cell(QString::fromStdString(m.Name), (int)m.Global);
        if (m.Depth) p->setToolTip(QString("Inside the nested container %1").arg(QString::fromStdString(c.Members[m.Parent].Name)));
        Table->setItem(r, 0, p);
        Table->setItem(r, 1, Cell(QString::fromStdString(m.Kind)));
        Table->setItem(r, 2, new NumItem(m.Size, false));
        Table->setItem(r, 3, new NumItem(m.StoredSize, false));
        Table->setItem(r, 4, Cell(QString::fromStdString(m.Form)));
        Table->setItem(r, 5, m.Offset == WBArc::ContainerMember::NoOffset ? Cell("") : new NumItem(c.RomStart + m.Offset, true));
        Table->setItem(r, 6, new NumItem((qulonglong)r, false));
    }
    Table->setSortingEnabled(true);
    Table->sortByColumn(6, Qt::AscendingOrder);
    Table->setColumnWidth(0, 260);
    for (int col = 1; col < 6; col++) Table->setColumnWidth(col, 72);
    ApplyFilter();
}

void ContainersPanel::ApplyFilter()
{
    QString f = Filter->text().trimmed();
    for (int r = 0; r < Table->rowCount(); r++)
        Table->setRowHidden(r, !f.isEmpty() && !Table->item(r, 0)->text().contains(f, Qt::CaseInsensitive));
}

QStringList ContainersPanel::Paths() const
{
    QStringList out;
    if (auto cont = H->Containers())
        for (size_t ci = 0; ci < cont->Containers().size(); ci++)
            for (size_t mi = 0; mi < cont->Containers()[ci].Members.size(); mi++)
                out << QString::fromStdString(cont->FullName((int)ci, (int)mi));
    return out;
}

int ContainersPanel::IndexOf(const QString& path) const
{
    auto cont = H->Containers();
    int ci = -1, mi = -1;
    if (!cont || !cont->Find(path.trimmed().toStdString(), ci, mi)) return -1;
    return (int)cont->Containers()[ci].Members[mi].Global;
}

void ContainersPanel::Locate(int global)
{
    auto cont = H->Containers();
    auto [ci, mi] = cont ? cont->FromGlobal(global) : std::make_pair(-1, -1);
    if (ci < 0) return;
    Shown = global;
    // show its container, then select it there
    for (int r = 0; r < List->rowCount(); r++)
        if (List->item(r, 0)->data(Qt::UserRole).toInt() == ci) { List->selectRow(r); List->scrollToItem(List->item(r, 0)); break; }
    ShowContainer(ci);
    for (int r = 0; r < Table->rowCount(); r++)
        if (Table->item(r, 0)->data(Qt::UserRole).toInt() == global)
        {
            if (Table->isRowHidden(r)) Filter->clear();
            Table->selectRow(r);
            Table->scrollToItem(Table->item(r, 0));
            break;
        }
    Result->setHtml("<i>Searching RAM...</i>");
    WBQ::ContainerCtx ctx {cont, H->ContainerStatus()};
    WBApi::ContainerLocateParams p;
    p.path = QString::fromStdString(cont->FullName(ci, mi));
    struct Res { bool Ok; WBApi::ContainerLocateResult R; QString Err; };
    H->Query<Res>([this, ctx, p]()
    {
        Res r;
        r.Ok = WBQ::ContainerLocate(H->Core(), ctx, p, r.R, r.Err);
        return r;
    }, this, [this, global](Res r)
    {
        if (global != Shown) return;
        if (!r.Ok) { Result->setHtml("<i>" + r.Err.toHtmlEscaped() + "</i>"); return; }
        const WBApi::ContainerMemberInfo& m = r.R.member;
        WBKB::KnowledgeStore& kb = W->KB();
        int cpu = W->Cpu();
        auto link = [&](u32 a)
        {
            QString n = kb.NameAt(cpu, a, W->OverlayContext(a), true);
            return QString("<a href=\"addr:%1\">%1</a>%2").arg(Hex8(a), n.isEmpty() ? QString() : " &lt;" + n.toHtmlEscaped() + "&gt;");
        };
        auto inFunc = [&](u32 pc)
        {
            const WBKB::Function* f = kb.FunctionAt(cpu, pc, W->OverlayContext(pc));
            if (!f) return QString();
            QString n = kb.NameAt(cpu, f->Addr, W->OverlayContext(f->Addr));
            return " in " + (n.isEmpty() ? Hex8(f->Addr) : n.toHtmlEscaped());
        };
        QString h = QString("<b>%1</b><br>%2 bytes").arg(m.path.toHtmlEscaped()).arg(m.size);
        if (!m.kind.isEmpty()) h += ", " + m.kind.toHtmlEscaped();
        if (m.form != "stored") h += QString(", packed (%1) in %2 bytes").arg(m.form.toHtmlEscaped()).arg(m.stored_size);
        if (m.rom_offset != 0xFFFFFFFF) h += QString(" at ROM 0x%1").arg(m.rom_offset, 0, 16);
        else h += QString(", inside a container packed whole (%1)").arg(r.R.container.packed.toHtmlEscaped());
        if (!m.nested_in.isEmpty()) h += "<br>nested in " + m.nested_in.toHtmlEscaped();
        if (!m.duplicate_of.isEmpty()) h += "<br><span style='color:%MUTED%'>identical to " + m.duplicate_of.toHtmlEscaped() + "</span>";
        if (!m.readable) h += "<br><span style='color:%MUTED%'>too large to search for by content</span>";
        h += "<p><b>In main RAM now</b><br>";
        if (r.R.ram.empty()) h += "<i>not in RAM</i> (not loaded yet, or freed or overwritten)<br>";
        for (const WBApi::ArchiveSegment& s : r.R.ram)
        {
            h += QString("<a href=\"sel:%1:%2\">%1-%3</a> = bytes +0x%4..+0x%5 (%6%)")
                .arg(Hex8(s.addr)).arg(s.size, 0, 16).arg(Hex8(s.addr + s.size))
                .arg(s.member_offset, 0, 16).arg(s.member_offset + s.size, 0, 16)
                .arg(m.size ? (int)((u64)s.size * 100 / m.size) : 100);
            if (s.best_match != s.path) h += ", <span style='color:%MUTED%'>part of a longer match with " + s.best_match.toHtmlEscaped() + "</span>";
            else if (!s.alternatives.empty())
                h += QString(", <span style='color:%MUTED%'>same bytes in %1 other file%2, e.g. %3</span>")
                    .arg(s.alternatives.size()).arg(s.alternatives.size() > 1 ? "s" : "").arg(s.alternatives[0].toHtmlEscaped());
            if (s.writer_pc) h += ", written by " + link(s.writer_pc) + inFunc(s.writer_pc);
            h += "<br>";
        }
        h += m.rom_offset != 0xFFFFFFFF ? "<p><b>Read from the card</b><br>" : "<p><b>Its container read from the card</b><br>";
        if (r.R.loads.empty()) h += "<i>no card reads recorded</i> (turn recording on before the game loads it)<br>";
        for (const WBApi::ProvenanceInfo& l : r.R.loads)
            h += QString("frame %1: %2%3 read 0x%4 bytes to %5 <span style='color:%MUTED%'>(%6)</span><br>")
                .arg(l.frame).arg(link(l.pc), inFunc(l.pc)).arg(l.src_size, 0, 16).arg(link(l.dst_addr))
                .arg((l.archive_member.isEmpty() ? l.detail : l.archive_member).toHtmlEscaped());
        Result->setHtml(WBColor::ApplyHtmlColors(h));
        u32 addr = r.R.ram.empty() ? 0 : r.R.ram[0].addr, size = r.R.ram.empty() ? 0 : r.R.ram[0].size;
        emit located(global, addr, size);
    });
}

// ================================================================ overlays

OverlaysPanel::OverlaysPanel(WorkbenchWindow* w) : WBPanel(w)
{
    // short headers: the table shares the panel with the events at the default size
    Table = MakeTable(this, {"Id", "CPU", "Start", "End", "File", "Packed", "Loaded"});
    Table->horizontalHeaderItem(2)->setToolTip("RAM start");
    Table->horizontalHeaderItem(3)->setToolTip("RAM end");
    Table->horizontalHeaderItem(5)->setToolTip("Compressed in the ROM (backward LZ)");
    Table->horizontalHeaderItem(6)->setToolTip("In RAM now, by the recording's overlay tracking");
    Events = MakeTable(this, {"Frame", "Event", "Overlay", "RAM range"});
    EpochLabel = new QLabel(this);
    QPushButton* view = new QPushButton("View this epoch in the disassembly", this);
    QPushButton* follow = new QPushButton("Follow the loaded overlay", this);
    QHBoxLayout* b = new QHBoxLayout();
    b->addWidget(view);
    b->addWidget(follow);
    b->addWidget(EpochLabel);
    b->addStretch();
    QSplitter* sp = new QSplitter(Qt::Horizontal, this);
    sp->addWidget(Table);
    sp->addWidget(Events);
    sp->setStretchFactor(0, 3);
    sp->setStretchFactor(1, 2);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(sp);
    v->addLayout(b);
    connect(view, &QPushButton::clicked, this, [this]()
    {
        int r = Table->currentRow();
        if (r < 0) return;
        int id = Table->item(r, 0)->text().toInt();
        W->SetEpochOverride(id);
        u32 start = Table->item(r, 2)->data(Qt::UserRole).toUInt();
        W->GoTo(start);
        Refresh();
    });
    connect(follow, &QPushButton::clicked, this, [this]() { W->SetEpochOverride(-1); Refresh(); });
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this](int r, int) { W->GoTo(Table->item(r, 2)->data(Qt::UserRole).toUInt()); });
    W->AddRowMenu(Table, [](const QModelIndex& i, u32& a, u32&, int&)
    {
        QVariant v = i.sibling(i.row(), 2).data(Qt::UserRole);
        a = v.toUInt();
        return v.isValid();
    });
    connect(H, &WBHost::overlayLoaded, this, [this](int, int) { Refresh(); });
}

void OverlaysPanel::Refresh()
{
    EpochLabel->setText(W->EpochOverride() >= 0 ? QString("viewing overlay %1").arg(W->EpochOverride()) : "following the loaded overlay");
    if (Busy || !IsShown()) return;
    Busy = true;
    H->Query<std::vector<OverlayEvent>>([this]() { return H->Core().Rec.OverlayEvents(); }, this, [this](std::vector<OverlayEvent> ev)
    {
        Busy = false;
        const NitroROM& rom = H->Core().Rom;
        size_t n = rom.Overlays9.size() + rom.Overlays7.size();
        bool resized = (size_t)Table->rowCount() != n;
        if (resized) Table->setRowCount((int)n);
        int row = 0;
        for (int c = 0; c < 2; c++)
            for (const OverlayInfo& o : c ? rom.Overlays7 : rom.Overlays9)
            {
                const RomFile* f = rom.FileById(o.FileId);
                Table->setItem(row, 0, Cell(QString::number(o.Id)));
                Table->setItem(row, 1, Cell(c ? "arm7" : "arm9"));
                QTableWidgetItem* s = Cell(Hex8(o.RamStart));
                s->setData(Qt::UserRole, (uint)o.RamStart);
                Table->setItem(row, 2, s);
                Table->setItem(row, 3, Cell(Hex8(o.RamEnd())));
                Table->setItem(row, 4, Cell(f ? QString::fromStdString(f->Path) : QString::number(o.FileId)));
                Table->setItem(row, 5, Cell(o.Compressed() ? "yes" : "no"));
                bool loaded = c == 0 && H->OverlayLoadedAt(o.RamStart) == (int)o.Id;
                QTableWidgetItem* l = Cell(loaded ? "loaded" : "");
                if (loaded) l->setForeground(WBColor::Blue());
                Table->setItem(row, 6, l);
                row++;
            }
        if (resized) FitColumns(Table);
        if (ev.size() != LastEvents)
        {
            LastEvents = ev.size();
            Events->setRowCount(0);
            for (auto it = ev.rbegin(); it != ev.rend(); ++it)
            {
                int r = Events->rowCount();
                Events->insertRow(r);
                Events->setItem(r, 0, Cell(QString::number(it->frame)));
                Events->setItem(r, 1, Cell(it->kind == 1 ? "evicted" : it->kind == 2 ? "load (ran)" : "load"));
                Events->setItem(r, 2, Cell(QString("%1 %2").arg(it->cpu ? "arm7" : "arm9").arg(it->overlay_id)));
                Events->setItem(r, 3, Cell(Hex8(it->ram_start) + "-" + Hex8(it->ram_end)));
            }
            FitColumns(Events);
        }
    });
}

// ================================================================ source and diff

static QString RenderDraft(WBHost* H, WorkbenchWindow* W, int cpu, const WBKB::Function& f)
{
    QString tool;
    QString imported = W->KB().DecompilerText(f.Id, &tool);
    u32 start = f.Addr;
    u32 size = std::max<u32>(f.Size, f.Thumb() ? 2u : 4u);
    bool thumb = f.Thumb();
    QString name = f.Name.isEmpty() ? QString("sub_%1").arg(start, 8, 16, QChar('0')) : f.Name;
    QString body;
    H->RunSync([&]()
    {
        body += QString("/* %1  %2  size 0x%3 */\n").arg(name, thumb ? "thumb" : "arm").arg(size, 0, 16);
        Workbench& wb = H->Core();
        for (u32 a = start; a < start + size && a < start + 0x800; )
        {
            u32 word = 0;
            if (!wb.Peek((u32)cpu, a, 4, word)) break;
            DisasmInsn in;
            Disassemble(a, word, thumb, in);
            body += QString::fromStdString(InsnAsC(in)) + "\n";
            u32 step = in.Size >= 2 ? in.Size : (thumb ? 2u : 4u);
            a += step;
        }
    });
    if (imported.isEmpty()) return body;
    return imported + "\n\n/* rendered from the machine code */\n" + body;
}

SourcePanel::SourcePanel(WorkbenchWindow* w) : WBPanel(w)
{
    Checklist = new CompareChecklist(w, this);
    Failure = new QLabel(this);
    Failure->setWordWrap(true);
    Failure->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Failure->setVisible(false);
    FailureMore = new QToolButton(this);
    FailureMore->setText("Show output");
    FailureMore->setAutoRaise(true);
    FailureMore->setVisible(false);
    FailureLog = new QPlainTextEdit(this);
    FailureLog->setReadOnly(true);
    FailureLog->setMaximumHeight(140);
    FailureLog->setVisible(false);
    connect(FailureMore, &QToolButton::clicked, this, [this]()
    {
        FailureLog->setVisible(!FailureLog->isVisible());
        FailureMore->setText(FailureLog->isVisible() ? "Hide output" : "Show output");
    });
    connect(Checklist, &CompareChecklist::changed, this, [this](bool ready) { if (Compare) Compare->setEnabled(ready && FuncId); });
    Header = new QLabel(this);
    Header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Header->setTextFormat(Qt::RichText);
    Source = new QPlainTextEdit(this);
    Source->setReadOnly(true);
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    Source->setFont(f);
    Diff = MakeTable(this, {"Original", "Compiled", ""});
    Compare = new QPushButton("Compare", this);
    Editor = new QPushButton("Open in editor", this);
    Attempts = new QComboBox(this);
    QHBoxLayout* b = new QHBoxLayout();
    b->addWidget(Compare);
    b->addWidget(Editor);
    b->addWidget(new QLabel("Attempts:", this));
    b->addWidget(Attempts, 1);
    QSplitter* sp = new QSplitter(Qt::Horizontal, this);
    sp->addWidget(Source);
    sp->addWidget(Diff);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(Checklist);
    v->addWidget(Header);
    v->addLayout(b);
    QHBoxLayout* fail = new QHBoxLayout();
    fail->addWidget(Failure, 1);
    fail->addWidget(FailureMore, 0, Qt::AlignTop);
    v->addLayout(fail);
    v->addWidget(FailureLog);
    v->addWidget(sp);
    connect(Compare, &QPushButton::clicked, this, &SourcePanel::RunCompare);
    connect(Editor, &QPushButton::clicked, this, &SourcePanel::OpenEditor);
    connect(Attempts, &QComboBox::currentIndexChanged, this, [this](int idx)
    {
        Diff->setRowCount(0);
        if (idx < 0 || !FuncId) return;
        auto results = W->KB().BuildResults(FuncId, 50);
        if (idx >= (int)results.size()) return;
        QJsonArray rows = QJsonDocument::fromJson(results[idx].Diff.toUtf8()).array();
        if (rows.isEmpty()) rows = QJsonDocument::fromJson(results[idx].Diff.toUtf8()).object().value("rows").toArray();
        for (const QJsonValue& v : rows)
        {
            QJsonObject o = v.toObject();
            int r = Diff->rowCount();
            Diff->insertRow(r);
            QString st = o.value("status").toString();
            QTableWidgetItem* a = Cell(o.value("orig").toString(o.value("original").toString()));
            QTableWidgetItem* b = Cell(o.value("compiled").toString(o.value("target").toString()));
            QColor c = st == "match" || st.isEmpty() ? QColor() : st == "reloc" ? WBColor::Amber() : WBColor::Red();
            if (c.isValid()) { a->setForeground(c); b->setForeground(c); }
            Diff->setItem(r, 0, a);
            Diff->setItem(r, 1, b);
            Diff->setItem(r, 2, Cell(st));
        }
    });
}

void SourcePanel::Refresh()
{
    // GoTo can arrive while this panel is still behind another tab. Apply it once the panel is showing,
    // or the checklist stays on no function and its buttons never name the module.
    if (!NavPending || !IsShown()) return;
    NavPending = false;
    OnNavigate(PendingNav);
}

void SourcePanel::OnNavigate(u32 addr)
{
    PendingNav = addr;
    if (!IsShown()) { NavPending = true; return; }
    NavPending = false;
    const WBKB::Function* f = W->KB().FunctionAt(W->Cpu(), addr, W->OverlayContext(addr));
    qint64 id = f ? f->Id : 0;
    if (id == FuncId) return;
    FuncId = id;
    Load(true);
}

void SourcePanel::Load(bool force)
{
    const WBKB::Function* f = FuncId ? W->KB().FunctionById(FuncId) : nullptr;
    Attempts->blockSignals(true);
    Attempts->clear();
    Attempts->blockSignals(false);
    Diff->setRowCount(0);
    if (!f)
    {
        Header->setText("No function at the cursor. Functions are created when their entry runs; P defines one that never ran.");
        Compare->setEnabled(false);
        if (force) Checklist->Check(0);
        Source->clear();
        FilePath.clear();
        return;
    }
    QString name = W->KB().NameAt(W->Cpu(), f->Addr, W->OverlayContext(f->Addr));
    QString hdr = QString("<b>%1</b> %2 - %3").arg(name.toHtmlEscaped(), Hex8(f->Addr), f->Status);
    if (f->LastScore >= 0) hdr += QString(", last score %1%").arg(f->LastScore, 0, 'f', 1);
    FilePath = H->FunctionSourcePath(*f);
    Compare->setEnabled(Checklist->Ready());
    if (force) Checklist->Check(FuncId);
    if (FilePath.isEmpty()) hdr += "<br><span style='color:%MUTED%'>No source file yet. Open in editor can create one under the source root.</span>";
    else hdr += "<br>" + FilePath.toHtmlEscaped();
    Header->setText(WBColor::ApplyHtmlColors(hdr));
    Source->clear();
    if (!FilePath.isEmpty())
    {
        QFile file(FilePath);
        if (file.open(QIODevice::ReadOnly))
        {
            Source->setPlainText(QString::fromUtf8(file.readAll()));
            QString fname = f->Name;
            if (!fname.isEmpty())
            {
                QTextCursor c = Source->document()->find(QRegularExpression("\\b" + QRegularExpression::escape(fname) + "\\s*\\("));
                if (!c.isNull()) { Source->setTextCursor(c); Source->centerCursor(); }
            }
        }
        else Source->setPlainText("(cannot read " + FilePath + ")");
    }
    auto results = W->KB().BuildResults(FuncId, 50);
    Attempts->blockSignals(true);
    for (const WBKB::BuildResult& r : results)
        Attempts->addItem(QString("%1  %2%  %3 %4").arg(r.Created.left(19).replace('T', ' ')).arg(r.Score, 0, 'f', 1).arg(r.Compiler, r.Matched ? "MATCH" : ""));
    Attempts->blockSignals(false);
    if (!results.empty()) { Attempts->setCurrentIndex(-1); Attempts->setCurrentIndex(0); }
}

void SourcePanel::CompareAt(u32 addr)
{
    const WBKB::Function* f = W->KB().FunctionAt(W->Cpu(), addr, W->OverlayContext(addr));
    if (!f) { W->ShowError("Compare", "No function at the cursor. Define one with P."); return; }
    if (f->Id != FuncId) { FuncId = f->Id; Load(true); }
    RunCompare();
}

void SourcePanel::OpenEditorAt(u32 addr)
{
    const WBKB::Function* f = W->KB().FunctionAt(W->Cpu(), addr, W->OverlayContext(addr));
    if (!f) { W->ShowError("Open in editor", "No function at the cursor."); return; }
    if (f->Id != FuncId) { FuncId = f->Id; Load(true); }
    OpenEditor();
}

bool SourcePanel::EnsureSourceFile(bool offerCreate)
{
    const WBKB::Function* f = FuncId ? W->KB().FunctionById(FuncId) : nullptr;
    if (!f) return false;
    QString root = H->Settings().SourceRoot;
    if (!f->SourceFile.isEmpty())
    {
        FilePath = H->FunctionSourcePath(*f);
        if (QFileInfo::exists(FilePath)) return true;
    }
    if (root.isEmpty())
    {
        W->ShowError("Source file", "This function has no source file, and no source root is set. Set the source root above, then try again.");
        return false;
    }
    QString name = f->Name.isEmpty() ? QString("sub_%1").arg(f->Addr, 8, 16, QChar('0')) : f->Name;
    name.replace(QRegularExpression("[^A-Za-z0-9_]"), "_");
    QString rel = f->SourceFile.isEmpty() ? QString("src/%1.c").arg(name) : f->SourceFile;
    QString path = QFileInfo(rel).isAbsolute() ? rel : QDir(root).filePath(rel);
    if (!QFileInfo::exists(path))
    {
        if (offerCreate && QMessageBox::question(this, "Source file",
                QString("This function has no source file yet. Create %1?").arg(path)) != QMessageBox::Yes)
            return false;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
        {
            W->ShowError("Source file", "Could not create " + path);
            return false;
        }
        file.write(RenderDraft(H, W, W->Cpu(), *f).toUtf8());
    }
    if (f->SourceFile.isEmpty())
    {
        QJsonObject o;
        o["source_file"] = rel;
        QString err;
        if (!W->KB().UpdateFunction(f->Id, o, err)) { W->ShowError("Source file", err); return false; }
    }
    FilePath = path;
    return true;
}

void SourcePanel::ShowFailure(const QString& explanation, const QString& error, const QString& details)
{
    bool any = !explanation.isEmpty() || !error.isEmpty();
    QString h = explanation.isEmpty() ? QString() : "<b>" + explanation.toHtmlEscaped() + "</b><br>";
    if (!error.isEmpty()) h += "<span style='color:%RED%'>" + error.toHtmlEscaped().replace("\n", "<br>") + "</span>";
    Failure->setText(WBColor::ApplyHtmlColors(h));
    Failure->setVisible(any);
    FailureLog->setPlainText(details);
    FailureMore->setVisible(any && !details.trimmed().isEmpty());
    FailureLog->setVisible(false);
    FailureMore->setText("Show output");
}

void SourcePanel::RunCompare()
{
    const WBKB::Function* f = FuncId ? W->KB().FunctionById(FuncId) : nullptr;
    if (!f) return;
    ShowFailure(QString(), QString(), QString());
    if (!EnsureSourceFile(true)) return;
    WBHost::CompareJob job;
    QString err;
    if (!H->EnsureCompareProject(err) || !H->PrepareCompare(f->Id, job, err))
    {
        ShowFailure(QString(), err, QString());
        return;
    }
    if (!ConfirmCompareCommand(this, H, job)) return;
    Compare->setEnabled(false);
    Compare->setText("Comparing...");
    QPointer<SourcePanel> self(this);
    H->RunCompare(job, true, 0, QString(), [self](const WBHost::CompareRun& r)
    {
        if (!self) return;
        self->Compare->setEnabled(true);
        self->Compare->setText("Compare");
        if (!r.Ok)
        {
            QString details = r.Result.value("log").toString();
            if (details.isEmpty()) details = r.Stderr;
            if (!r.Stdout.isEmpty() && r.Result.isEmpty()) details = r.Stdout + "\n" + r.Stderr;
            self->ShowFailure(r.Explanation, r.Error, details);
        }
        self->Load(false);
    });
}

void SourcePanel::OpenEditor()
{
    if (!EnsureSourceFile(true)) return;
    int line = Source->textCursor().blockNumber() + 1;
    QString cmd = H->Settings().EditorCommand;
    if (cmd.isEmpty())
    {
        QDesktopServices::openUrl(QUrl::fromLocalFile(FilePath));
        return;
    }
    cmd.replace("{file}", FilePath);
    cmd.replace("{line}", QString::number(line));
    QStringList args = QProcess::splitCommand(cmd);
    if (args.isEmpty()) return;
    QString prog = args.takeFirst();
    QProcess::startDetached(prog, args);
}

// ================================================================ decompiler

DecompilerPanel::DecompilerPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Header = new QLabel(this);
    Header->setWordWrap(true);
    Text = new QPlainTextEdit(this);
    Text->setReadOnly(true);
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    Text->setFont(f);
    UseSource = new QPushButton("Use as source", this);
    UseSource->setToolTip("Copy this draft into the function's source file.");
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(Header);
    v->addWidget(UseSource);
    v->addWidget(Text);
    connect(UseSource, &QPushButton::clicked, this, &DecompilerPanel::UseAsSource);
}

void DecompilerPanel::OnNavigate(u32 addr)
{
    if (!IsShown()) return;
    int cpu = W->Cpu();
    const WBKB::Function* f = W->KB().FunctionAt(cpu, addr, W->OverlayContext(addr));
    qint64 id = f ? f->Id : 0;
    if (id == FuncId) return;
    FuncId = id;
    if (!f) { Header->setText("No function at the cursor."); Text->clear(); Draft.clear(); UseSource->setEnabled(false); return; }
    u32 start = f->Addr;
    u32 size = std::max<u32>(f->Size, f->Thumb() ? 2u : 4u);
    bool thumb = f->Thumb();
    QString name = f->Name;
    qint64 fid = f->Id;
    QString tool;
    QString imported = W->KB().DecompilerText(f->Id, &tool);
    H->Query<QString>([=]()
    {
        QString body;
        body += QString("/* %1  %2  size 0x%3 */\n").arg(name.isEmpty() ? QString("sub_%1").arg(start, 8, 16, QChar('0')) : name).arg(thumb ? "thumb" : "arm").arg(size, 0, 16);
        Workbench& wb = H->Core();
        for (u32 a = start; a < start + size && a < start + 0x800; )
        {
            u32 word = 0;
            if (!wb.Peek((u32)cpu, a, 4, word)) break;
            DisasmInsn in;
            Disassemble(a, word, thumb, in);
            body += QString::fromStdString(InsnAsC(in)) + "\n";
            u32 step = in.Size >= 2 ? in.Size : (thumb ? 2u : 4u);
            a += step;
        }
        return body;
    }, this, [this, fid, imported, tool, name](QString body)
    {
        if (fid != FuncId) return;
        Draft = imported.isEmpty() ? body : imported;
        Header->setText(imported.isEmpty()
            ? QString("Draft of %1 from the machine code.").arg(name)
            : QString("Draft of %1 from %2. The rendered machine code is below it.").arg(name, tool));
        Text->setPlainText(imported.isEmpty() ? body : imported + "\n\n/* rendered from the machine code */\n" + body);
        UseSource->setEnabled(!Text->toPlainText().trimmed().isEmpty());
    });
}

void DecompilerPanel::UseAsSource()
{
    const WBKB::Function* f = FuncId ? W->KB().FunctionById(FuncId) : nullptr;
    if (!f) { W->ShowError("Use as source", "No function at the cursor."); return; }
    QString root = H->Settings().SourceRoot;
    if (root.isEmpty())
    {
        W->ShowError("Use as source", "Set the source root in the Source and diff panel first.");
        W->ShowPanel("dock_source");
        return;
    }
    QString name = f->Name.isEmpty() ? QString("sub_%1").arg(f->Addr, 8, 16, QChar('0')) : f->Name;
    QString safe = name;
    safe.replace(QRegularExpression("[^A-Za-z0-9_]"), "_");
    QString rel = f->SourceFile.isEmpty() ? QString("src/%1.c").arg(safe) : f->SourceFile;
    QString path = QFileInfo(rel).isAbsolute() ? rel : QDir(root).filePath(rel);
    if (QFileInfo::exists(path) && QFileInfo(path).size() > 0)
    {
        if (QMessageBox::question(this, "Use as source", QString("Replace the contents of %1 with this draft?").arg(path)) != QMessageBox::Yes)
            return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        W->ShowError("Use as source", "Could not write " + path);
        return;
    }
    file.write(Text->toPlainText().toUtf8());
    if (f->SourceFile.isEmpty())
    {
        QJsonObject o;
        o["source_file"] = rel;
        QString err;
        if (!W->KB().UpdateFunction(f->Id, o, err)) W->ShowError("Use as source", err);
    }
    W->Notice("Copied the draft into " + path);
    W->ShowPanel("dock_source");
}

// ================================================================ search

SearchPanel::SearchPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Tabs = new QTabWidget(this);
    QWidget* tb = new QWidget(this);
    BytesEdit = new QLineEdit(tb);
    BytesEdit->setPlaceholderText("hex bytes, ?? for any byte: 12 34 ?? 56");
    QPushButton* bgo = new QPushButton("Find", tb);
    QHBoxLayout* bl = new QHBoxLayout(tb);
    bl->addWidget(BytesEdit);
    bl->addWidget(bgo);
    Tabs->addTab(tb, "Bytes");

    QWidget* ts = new QWidget(this);
    StringEdit = new QLineEdit(ts);
    StringEnc = new QComboBox(ts);
    StringEnc->addItems({"ASCII", "Shift-JIS", "UTF-16LE"});
    QPushButton* sgo = new QPushButton("Find", ts);
    QHBoxLayout* sl = new QHBoxLayout(ts);
    sl->addWidget(StringEdit);
    sl->addWidget(StringEnc);
    sl->addWidget(sgo);
    Tabs->addTab(ts, "String");

    QWidget* tv = new QWidget(this);
    ScanType = new QComboBox(tv);
    ScanType->addItems({"u8", "u16", "u32", "s32", "fx32"});
    ScanType->setCurrentIndex(2);
    ScanCond = new QComboBox(tv);
    ScanCond->addItems({"equal to", "not equal to", "changed", "unchanged", "increased", "decreased", "greater than", "less than", "unknown (first scan)"});
    ScanValue = new QLineEdit(tv);
    ScanValue->setPlaceholderText("value (decimal or 0x hex)");
    ScanRegion = new QComboBox(tv);
    ScanRegion->addItems({"main RAM", "main RAM + WRAM + DTCM"});
    QPushButton* first = new QPushButton("First scan", tv);
    QPushButton* next = new QPushButton("Next scan", tv);
    ScanInfo = new QLabel(tv);
    QGridLayout* vl = new QGridLayout(tv);
    vl->addWidget(ScanType, 0, 0);
    vl->addWidget(ScanCond, 0, 1);
    vl->addWidget(ScanValue, 0, 2);
    vl->addWidget(ScanRegion, 0, 3);
    vl->addWidget(first, 1, 0);
    vl->addWidget(next, 1, 1);
    vl->addWidget(ScanInfo, 1, 2, 1, 2);
    Tabs->addTab(tv, "Value scan");

    Results = new QListWidget(this);
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    Results->setFont(f);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(Tabs);
    v->addWidget(Results);
    connect(bgo, &QPushButton::clicked, this, &SearchPanel::SearchBytes);
    connect(BytesEdit, &QLineEdit::returnPressed, this, &SearchPanel::SearchBytes);
    connect(sgo, &QPushButton::clicked, this, &SearchPanel::SearchString);
    connect(StringEdit, &QLineEdit::returnPressed, this, &SearchPanel::SearchString);
    connect(first, &QPushButton::clicked, this, &SearchPanel::ScanFirst);
    connect(next, &QPushButton::clicked, this, &SearchPanel::ScanNext);
    connect(Results, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it)
    {
        u32 a = (u32)it->data(Qt::UserRole).toUInt();
        W->GoTo(a);
        W->SetSelection(a, (u32)it->data(Qt::UserRole + 1).toUInt());
    });
    W->AddRowMenu(Results, [](const QModelIndex& i, u32& a, u32& n, int&)
    {
        QVariant v = i.data(Qt::UserRole);
        a = v.toUInt();
        n = i.data(Qt::UserRole + 1).toUInt();
        return v.isValid();
    });
}

void SearchPanel::ShowResults(const std::vector<std::pair<u32, QString>>& r)
{
    Results->clear();
    for (const auto& x : r)
    {
        QListWidgetItem* it = new QListWidgetItem(QString("%1  %2  %3").arg(Hex8(x.first), x.second, W->KB().NameAt(W->Cpu(), x.first, W->OverlayContext(x.first), true)));
        it->setData(Qt::UserRole, (uint)x.first);
        it->setData(Qt::UserRole + 1, ScanSize);
        Results->addItem(it);
    }
    if (r.size() >= 2000) Results->addItem("(first 2000 results)");
}

void SearchPanel::Find(const QByteArray& pat, const QByteArray& mask)
{
    ScanSize = pat.size();
    H->Query<std::vector<WBQ::SearchHit>>([this, pat, mask]() { return WBQ::FindPattern(H->Core(), pat, mask, true, 2000); }, this,
        [this](std::vector<WBQ::SearchHit> hits)
    {
        std::vector<std::pair<u32, QString>> r;
        for (const WBQ::SearchHit& h : hits) r.push_back({h.Addr, h.Preview});
        ShowResults(r);
    });
}

void SearchPanel::SearchBytes()
{
    QByteArray pat, mask;
    QString err;
    if (BytesEdit->text().trimmed().isEmpty()) return;
    if (!WBQ::ParseBytePattern(BytesEdit->text(), pat, mask, err)) { W->ShowError("Search", err); return; }
    Find(pat, mask);
}

void SearchPanel::SearchString()
{
    if (StringEdit->text().isEmpty()) return;
    static const char* encodings[] = {"ascii", "sjis", "utf16le"};
    QByteArray pat;
    QString err;
    if (!WBQ::EncodeSearchText(StringEdit->text(), encodings[std::clamp(StringEnc->currentIndex(), 0, 2)], pat, err)) { W->ShowError("Search", err); return; }
    Find(pat, QByteArray(pat.size(), '\1'));
}

void SearchPanel::ShowScan(const WBQ::ScanState& s)
{
    Candidates = s.Addrs;
    Previous = s.Values;
    ScanInfo->setText(QString("%1 candidates").arg(Candidates.size()));
    std::vector<std::pair<u32, QString>> show;
    for (size_t i = 0; i < Candidates.size() && i < 2000; i++) show.push_back({Candidates[i], QString("= %1").arg(Previous[i])});
    ShowResults(show);
}

void SearchPanel::ScanFirst()
{
    WBQ::ScanType type = (WBQ::ScanType)ScanType->currentIndex();
    WBQ::ScanCond cond = (WBQ::ScanCond)ScanCond->currentIndex();
    s64 val = 0;
    if (WBQ::ScanNeedsValue(cond) && !WBQ::ParseScanValue(ScanValue->text(), type, val)) { W->ShowError("Value scan", "enter a value"); return; }
    QString err;
    if (WBQ::ScanNeedsValue(cond) && !WBQ::ScanValueInRange(type, val, err)) { W->ShowError("Value scan", err); return; }
    WBQ::ScanState s;
    s.Type = type;
    ScanSize = (int)s.Size();
    bool wide = ScanRegion->currentIndex() == 1;
    H->Query<WBQ::ScanState>([this, s, cond, val, wide]() mutable { WBQ::ScanFirst(H->Core(), s, cond, val, wide); return s; }, this,
        [this](WBQ::ScanState s) { ShowScan(s); });
}

void SearchPanel::ScanNext()
{
    if (Candidates.empty()) { ScanFirst(); return; }
    WBQ::ScanCond cond = (WBQ::ScanCond)ScanCond->currentIndex();
    WBQ::ScanState s;
    s.Type = (WBQ::ScanType)ScanType->currentIndex();
    s64 val = 0;
    if (WBQ::ScanNeedsValue(cond) && !WBQ::ParseScanValue(ScanValue->text(), s.Type, val)) { W->ShowError("Value scan", "enter a value"); return; }
    QString err;
    if (WBQ::ScanNeedsValue(cond) && !WBQ::ScanValueInRange(s.Type, val, err)) { W->ShowError("Value scan", err); return; }
    s.Addrs = Candidates;
    s.Values = Previous;
    H->Query<WBQ::ScanState>([this, s, cond, val]() mutable { WBQ::ScanNext(H->Core(), s, cond, val); return s; }, this,
        [this](WBQ::ScanState s) { ShowScan(s); });
}

// ================================================================ breakpoints

BreakpointsPanel::BreakpointsPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Table = MakeTable(this, {"Id", "Kind", "CPU", "Address", "Size", "Condition", "Enabled", "Hits"});
    QPushButton* add = new QPushButton("Add...", this);
    QPushButton* en = new QPushButton("Enable", this);
    QPushButton* dis = new QPushButton("Disable", this);
    QPushButton* del = new QPushButton("Delete", this);
    QPushButton* allOn = new QPushButton("Enable all", this);
    QPushButton* allOff = new QPushButton("Disable all", this);
    QPushButton* cond = new QPushButton("Edit condition...", this);
    QPushButton* clr = new QPushButton("Delete all", this);
    QHBoxLayout* b = new QHBoxLayout();
    b->addWidget(add);
    b->addWidget(en);
    b->addWidget(dis);
    b->addWidget(del);
    b->addWidget(allOn);
    b->addWidget(allOff);
    b->addWidget(cond);
    b->addWidget(clr);
    b->addStretch();
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(Table);
    v->addLayout(b);
    auto curId = [this]() -> int
    {
        int r = Table->currentRow();
        if (r < 0 || !Table->item(r, 0)) return 0;
        return Table->item(r, 0)->data(Qt::UserRole).toInt();
    };
    auto editCond = [this, curId]()
    {
        int id = curId();
        if (!id) return;
        int r = Table->currentRow();
        bool ok;
        QString c = QInputDialog::getText(this, "Breakpoint condition",
                                          "Condition (empty for none), e.g. r0 == 0x10 &&&& [sp+4].w > 3:", QLineEdit::Normal,
                                          Table->item(r, 5)->text(), &ok);
        if (!ok) return;
        std::string err;
        bool done = false;
        H->RunSync([&]() { done = H->Core().Dbg.SetBreakpointCondition(id, c.trimmed().toStdString(), err); });
        if (!done) W->ShowError("Breakpoint", QString::fromStdString(err));
        LastKey.clear();
        emit H->breakpointsChanged();
    };
    connect(add, &QPushButton::clicked, this, &BreakpointsPanel::AddDialog);
    // the window refreshes this panel and the disassembly's markers on breakpointsChanged
    connect(en, &QPushButton::clicked, this, [this, curId]() { int id = curId(); if (!id) return; H->RunSync([this, id]() { H->Core().Dbg.SetBreakpointEnabled(id, true); }); LastKey.clear(); emit H->breakpointsChanged(); });
    connect(dis, &QPushButton::clicked, this, [this, curId]() { int id = curId(); if (!id) return; H->RunSync([this, id]() { H->Core().Dbg.SetBreakpointEnabled(id, false); }); LastKey.clear(); emit H->breakpointsChanged(); });
    connect(del, &QPushButton::clicked, this, [this, curId]() { int id = curId(); if (id) H->RunSync([this, id]() { H->Core().Dbg.RemoveBreakpoint(id); }); LastKey.clear(); emit H->breakpointsChanged(); });
    connect(allOn, &QPushButton::clicked, this, [this]() { W->SetAllBreakpoints(true); LastKey.clear(); });
    connect(allOff, &QPushButton::clicked, this, [this]() { W->SetAllBreakpoints(false); LastKey.clear(); });
    connect(clr, &QPushButton::clicked, this, [this]() { H->RunSync([this]() { H->Core().Dbg.RemoveBreakpoint(-1); }); LastKey.clear(); emit H->breakpointsChanged(); });
    connect(cond, &QPushButton::clicked, this, editCond);
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this](int r, int) { W->GoTo(Table->item(r, 3)->data(Qt::UserRole).toUInt()); });
    auto col = [](int c) -> WorkbenchWindow::RowAddress
    {
        return [c](const QModelIndex& i, u32& a, u32&, int&)
        {
            QVariant v = i.sibling(i.row(), c).data(Qt::UserRole);
            a = v.toUInt();
            return v.isValid();
        };
    };
    W->AddRowMenu(Table, col(3), [this, curId, editCond](QMenu& m, const QModelIndex& idx)
    {
        int id = idx.sibling(idx.row(), 0).data(Qt::UserRole).toInt();
        bool on = idx.sibling(idx.row(), 6).data().toString() == "yes";
        u32 addr = idx.sibling(idx.row(), 3).data(Qt::UserRole).toUInt();
        m.addAction(on ? "Disable" : "Enable", [this, id, on]()
        {
            if (!id) return;
            H->RunSync([this, id, on]() { H->Core().Dbg.SetBreakpointEnabled(id, !on); });
            LastKey.clear();
            emit H->breakpointsChanged();
        });
        m.addAction("Delete", [this, id]()
        {
            if (!id) return;
            H->RunSync([this, id]() { H->Core().Dbg.RemoveBreakpoint(id); });
            LastKey.clear();
            emit H->breakpointsChanged();
        });
        m.addAction("Edit condition", editCond);
        m.addAction("Go to disassembly", [this, addr]() { W->SetLockToPC(false); W->GoTo(addr); W->ShowPanel("dock_disasm"); });
        (void)curId;
    });
}

void BreakpointsPanel::AddDialog()
{
    BreakpointDialog d(W, W->Cursor());
    if (d.exec() != QDialog::Accepted) return;
    u32 a;
    if (!W->ParseAddress(d.Addr->text(), a)) { W->ShowError("Breakpoint", "bad address"); return; }
    Breakpoint bp;
    bp.Kind = (BreakKind)d.Kind->currentIndex();
    bp.Addr = bp.Kind == BreakKind::Exec ? (a & ~1u) : a;
    bp.Size = (u32)d.Size->value();
    bp.Cpu = d.Cpu->currentIndex() == 2 ? -1 : d.Cpu->currentIndex();
    bp.Condition = d.Condition->text().trimmed().toStdString();
    bp.Temporary = d.Temporary->isChecked();
    bp.Author = "person";
    std::string err;
    int id = -1;
    H->RunSync([&]() { id = H->Core().Dbg.AddBreakpoint(bp, err); });
    if (id < 0) W->ShowError("Breakpoint", QString::fromStdString(err));
    LastKey.clear();
    emit H->breakpointsChanged();
}

void BreakpointsPanel::Refresh()
{
    if (Busy) { PendingRefresh = true; return; }
    PendingRefresh = false;
    Busy = true;
    H->Query<std::pair<std::vector<Breakpoint>, std::vector<BreakHit>>>([this]()
    {
        return std::make_pair(H->Core().Dbg.List(), H->Core().Dbg.Hits());
    }, this, [this](std::pair<std::vector<Breakpoint>, std::vector<BreakHit>> r)
    {
        Busy = false;
        if (PendingRefresh) { PendingRefresh = false; Refresh(); return; }
        QString key;
        for (const Breakpoint& bp : r.first) key += QString("%1/%2/%3/%4;").arg(bp.Id).arg(bp.HitCount).arg(bp.Enabled).arg(QString::fromStdString(bp.Condition));
        key += QString::number(r.second.size());
        if (key == LastKey) return;
        LastKey = key;
        Table->setRowCount(0);
        for (const Breakpoint& bp : r.first)
        {
            int row = Table->rowCount();
            Table->insertRow(row);
            QTableWidgetItem* idItem = Cell(QString::number(bp.Id));
            idItem->setData(Qt::UserRole, bp.Id);
            Table->setItem(row, 0, idItem);
            Table->setItem(row, 1, Cell(BreakKindName((u8)bp.Kind)));
            Table->setItem(row, 2, Cell(bp.Cpu < 0 ? "any" : bp.Cpu ? "arm7" : "arm9"));
            QTableWidgetItem* a = Cell(W->AddrText(bp.Addr));
            a->setData(Qt::UserRole, (uint)bp.Addr);
            Table->setItem(row, 3, a);
            Table->setItem(row, 4, Cell(QString::number(bp.Size)));
            Table->setItem(row, 5, Cell(QString::fromStdString(bp.Condition)));
            Table->setItem(row, 6, Cell(bp.Enabled ? "yes" : "no"));
            Table->setItem(row, 7, Cell(QString::number(bp.HitCount)));
        }
        FitColumns(Table);
        (void)r.second;
    });
}

// ================================================================ recording

RecordingPanel::RecordingPanel(WorkbenchWindow* w) : WBPanel(w)
{
    RecBtn = new QPushButton("Start recording", this);
    RecBtn->setCheckable(true);
    RecBtn->setMinimumHeight(30);
    Hist = new QCheckBox("Write history", this);
    Origin = new QCheckBox("Data origin", this);
    Cap = new QSpinBox(this);
    Cap->setRange(1, 2000);
    Cap->setSuffix(" M entries cap");
    Cap->setValue((int)std::max<quint64>(1, H->Settings().HistoryCap / 1000000));
    AllRom = new QRadioButton("All ROM files are sources", this);
    Chosen = new QRadioButton("Only chosen sources", this);
    (H->Settings().OriginAllRomFiles ? AllRom : Chosen)->setChecked(true);
    FollowAddr = new QCheckBox("Follow table lookups", this);
    FollowAddr->setToolTip("A loaded value also takes the labels of the registers that formed its address. "
                           "Needed for table-driven decoders (Huffman, inflate); labels spread further through pointer-heavy code.");
    FollowAddr->setChecked(H->Settings().OriginFollowAddresses);
    OptKeypad = new QCheckBox("keypad", this);
    OptTouch = new QCheckBox("touch", this);
    OptRtc = new QCheckBox("RTC", this);
    Indicator = new QLabel(this);
    Indicator->setTextFormat(Qt::RichText);
    JitNote = new QLabel("Recording, breakpoints and stepping need the interpreter. Turn the JIT recompiler off in Config > Emu settings.", this);
    JitNote->setWordWrap(true);
    JitNote->setStyleSheet(QString("color: %1; font-weight: bold;").arg(WBColor::Red().name()));
    Sources = MakeTable(this, {"Id", "Kind", "Enabled", "Source"});     // the source last: it takes the spare width
    Log = new QListWidget(this);
    QPushButton* save = new QPushButton("Save session now", this);
    QPushButton* rmSrc = new QPushButton("Remove source", this);

    QGridLayout* g = new QGridLayout();
    g->addWidget(RecBtn, 0, 0, 1, 2);
    g->addWidget(Indicator, 0, 2, 1, 2);
    g->addWidget(Hist, 1, 0);
    g->addWidget(Cap, 1, 1);
    g->addWidget(Origin, 2, 0);
    g->addWidget(AllRom, 2, 1);
    g->addWidget(Chosen, 2, 2);
    g->addWidget(FollowAddr, 2, 3);
    QHBoxLayout* opt = new QHBoxLayout();
    opt->addWidget(new QLabel("Input sources:", this));
    opt->addWidget(OptKeypad);
    opt->addWidget(OptTouch);
    opt->addWidget(OptRtc);
    opt->addStretch();
    g->addLayout(opt, 3, 0, 1, 4);
    // what write history keeps, and where
    SkipBulk = new QCheckBox("skip bulk copies", this);
    SkipBulk->setToolTip("Keep only the first 1 KB of a sequential run of writes (copies, fills, unpacking)");
    SkipStack = new QCheckBox("skip stack", this);
    SkipStack->setToolTip("Skip writes near the writing CPU's stack pointer (register spills, locals)");
    SkipSame = new QCheckBox("skip unchanged", this);
    SkipSame->setToolTip("Skip writes that store the value already there");
    MainOnly = new QCheckBox("main RAM only", this);
    MainOnly->setToolTip("Keep only writes to main RAM (0x02000000-0x023FFFFF), where game objects live");
    for (QCheckBox* b : {SkipBulk, SkipStack, SkipSame}) b->setChecked(true);
    HistFileMB = new QSpinBox(this);
    HistFileMB->setRange(0, 65536);
    HistFileMB->setSingleStep(256);
    HistFileMB->setSuffix(" MB history file");
    HistFileMB->setSpecialValueText("history in memory");
    HistFileMB->setToolTip("Keep write history in a file of this size instead of memory (about 32,000 writes per MB). "
                           "Changing it starts history afresh.");
    HistFileMB->setValue((int)H->Settings().HistoryFileMB);
    HistNote = new QLabel(this);
    QHBoxLayout* hf = new QHBoxLayout();
    hf->addWidget(new QLabel("Write history:", this));
    for (QWidget* w : {(QWidget*)SkipBulk, (QWidget*)SkipStack, (QWidget*)SkipSame, (QWidget*)MainOnly, (QWidget*)HistFileMB}) hf->addWidget(w);
    hf->addWidget(HistNote, 1);
    g->addLayout(hf, 5, 0, 1, 4);
    for (QCheckBox* b : {SkipBulk, SkipStack, SkipSame, MainOnly})
        connect(b, &QCheckBox::toggled, this, [this]() { if (!Updating) ApplyHistoryFilter(); });
    connect(HistFileMB, &QSpinBox::editingFinished, this, [this]()
    {
        if ((quint32)HistFileMB->value() == H->Settings().HistoryFileMB) return;
        QString e;
        if (!H->SetHistoryFile((quint32)HistFileMB->value(), e, true)) W->ShowError("Write history file", e);
    });
    g->addWidget(JitNote, 4, 0, 1, 4);
    QSplitter* sp = new QSplitter(Qt::Horizontal, this);
    QWidget* left = new QWidget(sp);
    QVBoxLayout* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(new QLabel("Data origin sources", left));
    lv->addWidget(Sources);
    lv->addWidget(rmSrc);
    QWidget* right = new QWidget(sp);
    QVBoxLayout* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(new QLabel("Recording log", right));
    rv->addWidget(Log);
    rv->addWidget(save);
    sp->addWidget(left);
    sp->addWidget(right);
    // in a dock shorter than its switches the panel scrolls; its rows don't draw over each other
    QWidget* body = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(body);
    v->setContentsMargins(4, 4, 4, 4);
    v->addLayout(g);
    v->addWidget(sp, 1);
    sp->setMinimumHeight(120);
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(body);
    QVBoxLayout* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(RecBtn, &QPushButton::toggled, this, [this](bool on) { if (!Updating) H->SetRecording(on); });
    connect(Hist, &QCheckBox::toggled, this, [this](bool on) { if (!Updating) H->SetWriteHistory(on); });
    connect(Origin, &QCheckBox::toggled, this, [this](bool on) { if (!Updating) H->SetDataOrigin(on); });
    connect(Cap, &QSpinBox::editingFinished, this, [this]() { H->SetHistoryCap((quint64)Cap->value() * 1000000ULL); H->SaveSettings(); });
    connect(AllRom, &QRadioButton::toggled, this, [this](bool on)
    {
        if (Updating) return;
        H->Settings().OriginAllRomFiles = on;
        H->SaveSettings();
        H->RunSync([this, on]() { H->Core().Taint.SetAllRomFiles(on); });
    });
    connect(FollowAddr, &QCheckBox::toggled, this, [this](bool on)
    {
        if (Updating) return;
        H->Settings().OriginFollowAddresses = on;
        H->SaveSettings();
        H->RunSync([this, on]() { H->Core().Taint.SetFollowAddresses(on); });
    });
    auto inputSource = [this](SourceKind k, bool on)
    {
        if (Updating) return;
        H->RunSync([this, k, on]()
        {
            melonDS::WB::Origin& o = H->Core().Taint;
            for (const DataSource& s : o.Sources())
                if (s.Kind == k && s.Enabled) { if (!on) o.RemoveSource(s.Id); return; }
            if (on) { DataSource s; s.Kind = k; o.AddSource(s); }
        });
        Refresh();
    };
    connect(OptKeypad, &QCheckBox::toggled, this, [inputSource](bool on) { inputSource(SourceKind::Keypad, on); });
    connect(OptTouch, &QCheckBox::toggled, this, [inputSource](bool on) { inputSource(SourceKind::Touch, on); });
    connect(OptRtc, &QCheckBox::toggled, this, [inputSource](bool on) { inputSource(SourceKind::RTC, on); });
    connect(save, &QPushButton::clicked, this, [this]() { H->SaveSessionNow(); });
    connect(rmSrc, &QPushButton::clicked, this, [this]()
    {
        int r = Sources->currentRow();
        if (r < 0) return;
        u32 id = Sources->item(r, 0)->text().toUInt();
        H->RunSync([this, id]() { H->Core().Taint.RemoveSource(id); });
        Refresh();
    });
}

void RecordingPanel::ApplyHistoryFilter()
{
    bool bulk = SkipBulk->isChecked(), stack = SkipStack->isChecked(), same = SkipSame->isChecked(), main = MainOnly->isChecked();
    H->RunSync([this, bulk, stack, same, main]()
    {
        Recorder::HistoryFilter f = H->Core().Rec.GetHistoryFilter();
        f.BulkThreshold = bulk ? 1024 : 0;
        f.SkipStack = stack;
        f.SkipUnchanged = same;
        f.Include.clear();
        if (main) f.Include.push_back({0x02000000, 0x02400000});
        H->Core().Rec.SetHistoryFilter(f);
    });
}

void RecordingPanel::Refresh()
{
    WBStatus s = H->Status();
    Updating = true;
    RecBtn->setChecked(s.Recording);
    RecBtn->setText(s.Recording ? "Stop recording (saves the session)" : "Start recording");
    RecBtn->setStyleSheet(s.Recording ? QString("QPushButton { background: %1; color: %2; font-weight: bold; }").arg(WBColor::ChipRec().name(), WBColor::Text().name()) : "");
    RecBtn->setEnabled(s.Available && s.EmuState != WBStatus::NoGame);
    Hist->setChecked(s.History);
    Origin->setChecked(s.Origin);
    Hist->setEnabled(s.Available && s.Recording);
    Origin->setEnabled(s.Available && s.Recording);
    JitNote->setVisible(!s.Available);
    QString ind;
    QString on = WBColor::Text().name();
    if (s.Recording) ind += QString("<span style='background:%1;color:%2;font-weight:bold'>&nbsp;REC&nbsp;</span> ").arg(WBColor::ChipRec().name(), on);
    if (s.Recording && s.History) ind += QString("<span style='background:%1;color:%2;font-weight:bold'>&nbsp;HIST&nbsp;</span> ")
                                             .arg(WBColor::Off().name(), s.HistoryWrapped ? WBColor::Amber().name() : on);
    if (s.Recording && s.Origin) ind += QString("<span style='background:%1;color:%2;font-weight:bold'>&nbsp;ORIGIN&nbsp;</span> ").arg(WBColor::ChipOrigin().name(), on);
    if (s.EmuState == WBStatus::Paused || s.EmuState == WBStatus::Break)
        ind += QString("<span style='background:%1;color:%2;font-weight:bold'>&nbsp;PAUSED&nbsp;</span> ").arg(WBColor::Select().name(), on);
    ind += QString("frames %1, history %2/%3, %4 pages, %5 fps").arg(s.FramesRecorded).arg(s.HistoryCount).arg(s.HistoryCap).arg(s.Pages).arg(s.Fps, 0, 'f', 1);
    Indicator->setText(ind);
    HistNote->setText(s.HistoryWrapped
        ? QString("<span style='color:%1'>full: writes before frame %2 were dropped</span>").arg(WBColor::Amber().name()).arg(s.HistoryOldestFrame)
        : QString("<span style='color:%1'>%2 writes skipped by the filters</span>").arg(WBColor::Muted().name()).arg(s.HistorySkipped));
    Updating = false;

    if (Busy || !IsShown()) return;
    Busy = true;
    H->Query<std::pair<std::vector<DataSource>, std::vector<RecEvent>>>([this]()
    {
        return std::make_pair(H->Core().Taint.Sources(), H->Core().Rec.Events());
    }, this, [this](std::pair<std::vector<DataSource>, std::vector<RecEvent>> r)
    {
        Busy = false;
        Updating = true;
        bool kp = false, tc = false, rtc = false;
        Sources->setRowCount(0);
        for (const DataSource& s : r.first)
        {
            if (s.Kind == SourceKind::CodeConstant) continue;
            if (s.Enabled && s.Kind == SourceKind::Keypad) kp = true;
            if (s.Enabled && s.Kind == SourceKind::Touch) tc = true;
            if (s.Enabled && s.Kind == SourceKind::RTC) rtc = true;
            int row = Sources->rowCount();
            Sources->insertRow(row);
            Sources->setItem(row, 0, Cell(QString::number(s.Id)));
            Sources->setItem(row, 1, Cell(SourceKindName((u8)s.Kind)));
            Sources->setItem(row, 2, Cell(s.Enabled ? "yes" : "no"));
            Sources->setItem(row, 3, Cell(QString::fromStdString(s.Name)));
        }
        FitColumns(Sources);
        OptKeypad->setChecked(kp);
        OptTouch->setChecked(tc);
        OptRtc->setChecked(rtc);
        Updating = false;
        if (r.second.size() != LastLog)
        {
            LastLog = r.second.size();
            Log->clear();
            for (auto it = r.second.rbegin(); it != r.second.rend(); ++it)
            {
                QString k = RecEventKindName(it->kind);
                if (it->value && ((RecEventKind)it->kind == RecEventKind::OverlayLoad || (RecEventKind)it->kind == RecEventKind::SourceAdded || (RecEventKind)it->kind == RecEventKind::SourceRemoved))
                    k += QString(" %1").arg(it->value);
                Log->addItem(QString("frame %1: %2").arg(it->frame).arg(k));
            }
        }
    });
}
