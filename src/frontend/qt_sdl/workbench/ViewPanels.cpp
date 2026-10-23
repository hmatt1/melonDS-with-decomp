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

#include <cmath>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QToolButton>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QScrollBar>
#include <QToolTip>

#include "Panels.h"
#include "WBColors.h"
#include "WBHost.h"
#include "WBQueries.h"
#include "Disasm.h"
#include "NDS.h"

using namespace melonDS;
using namespace melonDS::WB;
static QString ByteCount(u32 n) { return n == 1 ? QString("1 byte") : QString("%1 bytes").arg(n); }

static QFont MonoFont()
{
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    return f;
}

struct AddrRange { u32 Start, End; };

static AddrRange ScrollRegion(u32 addr, u32 dtcmBase)
{
    if (addr >= 0x02000000 && addr < 0x03000000) return {0x02000000, 0x02400000 > addr ? 0x02400000u : 0x03000000u};
    if (addr >= 0x01FF8000 && addr < 0x02000000) return {0x01FF8000, 0x02000000};
    if (addr < 0x00008000) return {0, 0x8000};
    if (dtcmBase != 0xFFFFFFFF && addr >= dtcmBase && addr < dtcmBase + 0x4000) return {dtcmBase, dtcmBase + 0x4000};
    if (addr >= 0x03000000 && addr < 0x03800000) return {0x03000000, 0x03008000};
    if (addr >= 0x03800000 && addr < 0x04000000) return {0x03800000, 0x03810000};
    if (addr >= 0x06000000 && addr < 0x07000000) return {0x06000000, 0x06900000};
    if (addr >= 0xFFFF0000) return {0xFFFF0000, 0xFFFF8000};
    u32 s = addr & ~0xFFFFu;
    return {s, s + 0x10000};
}

// ================================================================ disassembly

DisasmPanel::DisasmPanel(WorkbenchWindow* w) : WBPanel(w)
{
    setFont(MonoFont());
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    Opts = new QWidget(this);
    ShowBytes = new QCheckBox("Raw bytes", Opts);
    ShowBytes->setChecked(true);
    ShowData = new QCheckBox("Hex and ASCII for data", Opts);
    ShowData->setChecked(true);
    auto* oh = new QHBoxLayout(Opts);
    oh->setContentsMargins(6, 2, 6, 2);
    oh->addWidget(ShowBytes);
    oh->addWidget(ShowData);
    oh->addStretch();
    connect(ShowBytes, &QCheckBox::toggled, this, [this]() { update(); });
    connect(ShowData, &QCheckBox::toggled, this, [this]() { update(); });
    Bar = new QScrollBar(Qt::Vertical, this);
    connect(Bar, &QScrollBar::actionTriggered, this, [this](int)
    {
        AddrRange r = ScrollRegion(Top, 0xFFFFFFFF);
        u32 a = r.Start + (u32)Bar->sliderPosition() * 4;
        if (W->LockedToPC()) W->SetLockToPC(false);
        Top = a;
        Fetch();
    });
    connect(w, &WorkbenchWindow::cpuChanged, this, [this](int) { Fetch(); });
    QFontMetrics fm(font());
    LineH = fm.height() + 2;
}

int DisasmPanel::Chrome() const
{
    int h = Opts ? Opts->sizeHint().height() : 0;
    if (!Sticky.isEmpty()) h += LineH;
    return h;
}

int DisasmPanel::VisibleRows() const { return std::max(1, (height() - Chrome()) / LineH); }

u32 DisasmPanel::PCForView() const
{
    WBStatus s = H->Status();
    if (s.EmuState == WBStatus::Break && s.BreakInfo.Cpu == (u32)W->Cpu()) return s.BreakInfo.PC;
    return s.PC[W->Cpu()];
}

void DisasmPanel::OnNavigate(u32 addr)
{
    ShowAddress(addr, false);
}

bool DisasmPanel::LookingAtUnknown(u32 addr) const
{
    for (const DisasmRow& r : Rows)
    {
        if (r.Type == DisasmRow::LabelLine) continue;
        if (addr >= r.Addr && addr < r.Addr + r.Size) return r.Class == WBQ::CodeKind::Unknown;
    }
    return false;
}

void DisasmPanel::ShowAddress(u32 addr, bool center)
{
    // keep the address visible; scroll only when it left the view
    bool visible = false;
    int idx = 0;
    for (const DisasmRow& r : Rows)
    {
        if (r.Type != DisasmRow::LabelLine && addr >= r.Addr && addr < r.Addr + r.Size)
        {
            visible = idx >= 1 && idx < VisibleRows() - 2;
            break;
        }
        idx++;
    }
    if (!visible || center)
    {
        int above = std::min(8, VisibleRows() / 4);
        Top = (addr & ~1u) >= (u32)(above * 4) ? (addr & ~1u) - above * 4 : 0;
        // align to the mode we will most likely decode
        Top &= ~1u;
    }
    Fetch();
}

void DisasmPanel::Refresh()
{
    if (W->LockedToPC())
    {
        u32 pc = PCForView();
        ShowAddress(pc, false);
        return;
    }
    Fetch();
}

void DisasmPanel::ScrollRows(int delta)
{
    if (W->LockedToPC()) W->SetLockToPC(false);
    if (delta > 0)
    {
        int n = 0;
        for (const DisasmRow& r : Rows)
        {
            if (r.Type == DisasmRow::LabelLine) continue;
            if (n++ == delta) { Top = r.Addr; break; }
        }
        if (n <= delta) Top += delta * 4;
    }
    else Top -= (u32)(-delta) * 4;
    Fetch();
}

void DisasmPanel::Fetch()
{
    if (Busy) { Pending = true; return; }
    Busy = true;
    int cpu = W->Cpu();
    u32 top = Top;
    int count = VisibleRows() + 8;
    int epoch = W->EpochOverride();
    // what the knowledge store says about these rows (functions, data objects, overrides), for classification
    WBQ::KbHints hints = WBQ::KbHints::Collect(W->KB(), cpu, top, top + count * 4 + 64, W->OverlayContext(top));
    std::vector<WBQ::DataUnit> units = WBQ::DataUnits(W->KB(), cpu, top, top + count * 4 + 64, W->OverlayContext(top));
    WBQ::AnalysisHints guessed = H->AnalysisNear(cpu, top > 8 ? top - 8 : 0, top + count * 4 + 64);
    bool onlyConfirmed = OnlyConfirmed;
    H->Query<std::vector<DisasmRow>>([this, cpu, top, count, epoch, hints, units, guessed, onlyConfirmed]()
    {
        std::vector<DisasmRow> rows;
        Workbench& wb = H->Core();
        NDS* nds = wb.GetNDS();
        if (!nds) return rows;
        const OverlayInfo* ov = epoch >= 0 ? wb.Rom.Overlay(0, (u32)epoch) : nullptr;
        std::vector<u8> ovimage;          // its bytes from the ROM, unpacked when compressed
        bool haveImage = ov && WBQ::OverlayImage(wb.Rom, *ov, ovimage);
        u32 a = top;
        for (int n = 0; n < count; n++)
        {
            // records: the chosen overlay epoch, or whatever is mapped now
            bool fromEpoch = false;
            const ByteRecord* rec = nullptr;
            if (ov && cpu == 0 && a >= ov->RamStart && a < ov->RamEnd() && wb.Rec.OverlayAt(a & nds->MainRAMMask) != (int)ov->Id)
            {
                fromEpoch = true;
                u16 region = (u16)Region::OverlayARM9Base + (u16)ov->Id;
                u32 off = a & nds->MainRAMMask;
                if (RecordPage* p = wb.Rec.Store().Get(region, off >> PageShift)) rec = &p->Rec[off & (PageSize - 1)];
            }
            else rec = wb.Rec.Get(wb.Translate((u32)cpu, a));

            DisasmRow r;
            // decoded by its classification: recorded mode, override, data object, then the function's mode
            WBQ::Classification cls = WBQ::Classify(rec, hints, a, &guessed);
            int mode = -1;
            bool decode = cls.IsCode() && (!onlyConfirmed || cls.Confirmed() || cls.From == WBQ::Evidence::Override);
            if (decode)
            {
                if (cls.Kind == WBQ::CodeKind::Mixed) mode = ((rec->flags & ByteFlag::Thumb) && !(rec->flags & ByteFlag::Arm)) ? 1 : 0;
                else mode = cls.Kind == WBQ::CodeKind::Thumb ? 1 : 0;
            }
            bool dim = !cls.Confirmed();
            r.Class = cls.Kind;
            r.From = cls.From;
            r.Conflict = cls.Conflict;
            r.Why = cls.Why;
            if (cls.IsCode() && !decode) r.Why += " (shown as data: Decode only confirmed code is on)";
            bool thumb = mode == 1;
            // data of a defined type starts at its fields, whatever their alignment
            const WBQ::DataUnit* unit = nullptr;
            if (mode < 0)
            {
                auto it = std::upper_bound(units.begin(), units.end(), a, [](u32 v, const WBQ::DataUnit& u) { return v < u.Addr; });
                if (it != units.begin() && a < std::prev(it)->Addr + std::prev(it)->Size) unit = &*std::prev(it);
            }
            if (unit) a = unit->Addr;
            else if (mode < 0 || !thumb) a &= ~3u;
            else a &= ~1u;
            if (rec == nullptr || (a & 3))
            {
                if (fromEpoch)
                {
                    u16 region = (u16)Region::OverlayARM9Base + (u16)ov->Id;
                    u32 off = a & nds->MainRAMMask;
                    RecordPage* p = wb.Rec.Store().Get(region, off >> PageShift);
                    rec = p ? &p->Rec[off & (PageSize - 1)] : nullptr;
                }
                else rec = wb.Rec.Get(wb.Translate((u32)cpu, a));
            }

            u32 word = 0;
            bool ok;
            if (fromEpoch)
            {
                ok = false;
                u32 off = a - ov->RamStart;
                if (haveImage && off + 4 <= ovimage.size())
                {
                    memcpy(&word, ovimage.data() + off, 4);
                    ok = true;
                }
            }
            else ok = WBQ::FetchCode(wb, (u32)cpu, a, thumb, word);

            r.Addr = a;
            if (rec)
            {
                r.Exec = rec->exec_count;
                r.Read = rec->read_count;
                r.Write = rec->write_count;
                r.Flags = rec->flags;
            }
            if (unit && !fromEpoch)
            {
                r.Type = DisasmRow::Data;
                r.Dim = dim;
                if (unit->Str)
                {
                    // a string field: its text up to the NUL, at most 48 characters
                    QString s;
                    u32 n = std::min<u32>(unit->Size, 48);
                    for (u32 i = 0; i < n; i++)
                    {
                        u32 ch = 0;
                        if (!wb.Peek((u32)cpu, a + i, 1, ch) || !ch) break;
                        s += (ch >= 32 && ch < 127) ? QChar((char)ch) : QChar('.');
                    }
                    r.Size = unit->Size;
                    r.Bytes = QString();
                    r.Mnemonic = ".asciz";
                    r.Operands = "\"" + s + "\"  ; " + unit->Name;
                    r.Ascii = s;
                    r.Explain = "This is data (a string), not an instruction.";
                }
                else
                {
                    u32 v = 0;
                    bool vok = wb.Peek((u32)cpu, a, unit->Size, v);
                    int digits = (int)unit->Size * 2;
                    r.Size = unit->Size;
                    r.Bytes = vok ? QString("%1").arg(v, digits, 16, QChar('0')) : QString(digits, '?');
                    r.Mnemonic = unit->Size == 1 ? ".byte" : unit->Size == 2 ? ".hword" : ".word";
                    r.Operands = (vok ? QString("0x%1").arg(v, digits, 16, QChar('0')) : QString("??")) + "  ; " + unit->Name;
                    r.Explain = "This is data, not an instruction.";
                    if (vok)
                    {
                        r.Ascii = "\"";
                        for (u32 i = 0; i < unit->Size; i++)
                        {
                            u8 ch = (u8)((v >> (8 * i)) & 0xFF);
                            r.Ascii += (ch >= 32 && ch < 127) ? QChar(ch) : QChar('.');
                        }
                        r.Ascii += "\"";
                    }
                    if (vok && unit->Size == 4 && ((v >> 24) == 0x02 || (v >> 24) == 0x03))
                    {
                        r.HasTarget = true;
                        r.Target = v;
                    }
                }
            }
            else if (mode < 0 || !ok)
            {
                r.Type = DisasmRow::Data;
                r.Dim = dim;
                r.Size = 4;
                r.Bytes = ok ? QString("%1").arg(word, 8, 16, QChar('0')) : "????????";
                r.Mnemonic = ".word";
                r.Operands = ok ? QString("0x%1").arg(word, 8, 16, QChar('0')) : "??";
                r.Explain = "This is data, not an instruction.";
                if (ok)
                {
                    r.Ascii = "\"";
                    for (int i = 0; i < 4; i++)
                    {
                        u8 ch = (u8)((word >> (8 * i)) & 0xFF);
                        r.Ascii += (ch >= 32 && ch < 127) ? QChar(ch) : QChar('.');
                    }
                    r.Ascii += "\"";
                }
                if (ok && ((word >> 24) == 0x02 || (word >> 24) == 0x01 || (word >> 24) == 0x03 || (word >> 24) == 0x04 || (word >> 24) == 0x06))
                {
                    r.HasTarget = true;
                    r.Target = word;
                }
            }
            else
            {
                DisasmInsn in;
                Disassemble(a, word, thumb, in);
                r.Type = DisasmRow::Code;
                r.Thumb = thumb;
                r.Dim = dim;
                r.Size = in.Size;
                if (thumb && in.Size == 4) r.Bytes = QString("%1 %2").arg(word & 0xFFFF, 4, 16, QChar('0')).arg(word >> 16, 4, 16, QChar('0'));
                else if (thumb) r.Bytes = QString("%1").arg(word & 0xFFFF, 4, 16, QChar('0'));
                else r.Bytes = QString("%1").arg(word, 8, 16, QChar('0'));
                r.Mnemonic = QString::fromStdString(in.Mnemonic);
                r.Operands = QString::fromStdString(in.Operands);
                r.Explain = QString::fromStdString(ExplainInsn(in));
                r.HasTarget = in.HasTarget;
                r.Target = in.Target;
                r.HasRef = in.HasRef;
                r.Ref = in.Ref;
                r.Literal = in.LiteralLoad;
                r.RegMask = in.RegMask;
                r.IsCall = in.IsCall;
                r.IsReturn = in.IsReturn;
                if (in.LiteralLoad)
                {
                    u32 v;
                    if (wb.Peek((u32)cpu, in.Ref, 4, v)) { r.LiteralValue = v; r.LiteralValid = true; }
                }
            }
            rows.push_back(r);
            a += r.Size;
        }
        return rows;
    }, this, [this](std::vector<DisasmRow> rows)
    {
        Busy = false;
        // label lines for function starts and labels
        Rows.clear();
        WBKB::KnowledgeStore& kb = W->KB();
        for (const DisasmRow& r : rows)
        {
            int ov = W->OverlayContext(r.Addr);
            const WBKB::Function* f = kb.FunctionStarting(W->Cpu(), r.Addr, ov);
            const WBKB::Label* l = kb.LabelAt(W->Cpu(), r.Addr, ov);
            if (f || l)
            {
                DisasmRow lr;
                lr.Type = DisasmRow::LabelLine;
                lr.Addr = r.Addr;
                lr.Size = 0;
                if (f)
                {
                    QString nm = f->Name.isEmpty() ? QString("sub_%1").arg(f->Addr, 8, 16, QChar('0')) : f->Name;
                    lr.LabelText = QString("%1    %2    size 0x%3").arg(nm, f->Mode).arg(f->Size, 0, 16);
                    if (!f->Signature.isEmpty()) lr.LabelText += "    " + f->Signature;
                    lr.InFunction = true;
                    lr.Thumb = f->Thumb();
                    lr.FuncName = nm;
                }
                else lr.LabelText = l->Name + ":";
                Rows.push_back(lr);
            }
            {
                const WBKB::Function* cf = kb.FunctionAt(W->Cpu(), r.Addr, ov);
                DisasmRow row = r;
                if (cf)
                {
                    row.InFunction = true;
                    row.Thumb = cf->Thumb();
                    row.FuncName = cf->Name.isEmpty() ? QString("sub_%1").arg(cf->Addr, 8, 16, QChar('0')) : cf->Name;
                }
                Rows.push_back(row);
            }
        }
        Sticky.clear();
        if (!Rows.empty())
        {
            const WBKB::Function* topf = kb.FunctionAt(W->Cpu(), Top, W->OverlayContext(Top));
            if (topf)
            {
                bool shown = false;
                for (const DisasmRow& row : Rows)
                    if (row.Type == DisasmRow::LabelLine && row.Addr == topf->Addr) shown = true;
                if (!shown)
                {
                    QString nm = topf->Name.isEmpty() ? QString("sub_%1").arg(topf->Addr, 8, 16, QChar('0')) : topf->Name;
                    Sticky = QString("%1    %2    size 0x%3").arg(nm, topf->Mode).arg(topf->Size, 0, 16);
                }
            }
        }
        AddrRange rg = ScrollRegion(Top, 0xFFFFFFFF);
        Bar->blockSignals(true);
        Bar->setRange(0, (int)((rg.End - rg.Start) / 4));
        Bar->setPageStep(VisibleRows());
        Bar->setValue((int)((Top - rg.Start) / 4));
        Bar->blockSignals(false);
        update();
        if (Pending) { Pending = false; Fetch(); }
    });
}

void DisasmPanel::resizeEvent(QResizeEvent*)
{
    int oh = Opts->sizeHint().height();
    Opts->setGeometry(0, 0, width(), oh);
    int bw = Bar->sizeHint().width();
    Bar->setGeometry(width() - bw, oh, bw, height() - oh);
    Fetch();
}

void DisasmPanel::wheelEvent(QWheelEvent* e)
{
    int steps = -e->angleDelta().y() / 40;
    if (!steps) steps = e->angleDelta().y() > 0 ? -1 : 1;
    ScrollRows(steps);
}

int DisasmPanel::RowAt(int y) const
{
    int top = Chrome();
    if (y < top) return -1;
    int i = (y - top) / LineH;
    return (i >= 0 && i < (int)Rows.size()) ? i : -1;
}

void DisasmPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    QColor bg = WBColor::Base();
    QColor fg = WBColor::Text();
    QColor dimc = WBColor::Muted();
    QColor addrc = WBColor::Muted();
    QColor mnemc = WBColor::Text();
    QColor linkc = WBColor::Blue();
    QColor cmtc = WBColor::Text();
    QColor agentc = WBColor::Text();
    QColor here = WBColor::Select();
    QColor labelc = WBColor::Text();
    p.fillRect(rect(), bg);
    p.setFont(font());
    QFontMetrics fm(font());
    int cw = fm.horizontalAdvance('0');
    int x = 4;
    int colGutter = x; x += 14;
    ColMode = x; x += cw * 2 + 6;
    ColHeat = x; x += 44;
    ColAddr = x; x += cw * 10;
    bool raw = ShowBytes->isChecked();
    ColBytes = x;
    if (raw) x += cw * 11;
    ColMnem = x; x += cw * 9;
    ColOps = x; x += cw * 36;
    ColCmt = x;

    u32 pc = PCForView();
    WBStatus st = H->Status();
    bool showPC = st.EmuState != WBStatus::NoGame;
    WBKB::KnowledgeStore& kb = W->KB();
    u32 maxExec = 1;
    for (const DisasmRow& r : Rows) maxExec = std::max(maxExec, r.Exec);
    double lmax = std::log2((double)maxExec + 1);
    Links.clear();

    int vis = VisibleRows();
    int y0 = Chrome();
    int cpu = W->Cpu();
    auto barFor = [&](const DisasmRow& r) -> QColor
    {
        if (r.Type == DisasmRow::LabelLine) return WBColor::FunctionBar();
        const WBKB::Function* f = kb.FunctionAt(cpu, r.Addr, W->OverlayContext(r.Addr));
        if (f && H->FunctionOpen(cpu, f->Addr) && r.Addr + std::max<u32>(r.Size, 1) >= f->Addr + std::max<u32>(f->Size, 1))
            return WBColor::FunctionEnd();
        return WBColor::FunctionBar();
    };
    if (!Sticky.isEmpty())
    {
        int sy = Opts->sizeHint().height();
        p.fillRect(0, sy, width(), LineH, here);
        p.setPen(linkc);
        p.drawText(ColAddr, sy + fm.ascent() + 1, Sticky);
    }
    for (int i = 0; i < (int)Rows.size() && i < vis + 1; i++)
    {
        const DisasmRow& r = Rows[i];
        int y = y0 + i * LineH;
        int base = y + fm.ascent() + 1;
        if (r.InFunction)
            p.fillRect(0, y, 3, LineH, barFor(r));
        if (r.Type == DisasmRow::LabelLine)
        {
            p.setPen(labelc);
            p.drawText(ColAddr, base, r.LabelText);
            continue;
        }
        bool isPC = showPC && pc >= r.Addr && pc < r.Addr + r.Size;
        bool isCur = W->Cursor() >= r.Addr && W->Cursor() < r.Addr + r.Size;
        if (!r.InFunction && r.Exec) p.fillRect(0, y, width(), LineH, WBColor::Unowned());
        if (isPC || isCur) p.fillRect(0, y, width(), LineH, here);
        if (r.InFunction) p.fillRect(0, y, 3, LineH, barFor(r));
        if (H->FunctionNotched(cpu, r.Addr))
            p.fillRect(0, y + 2, 6, LineH - 4, WBColor::Amber());

        // gutter: breakpoint marker (filled when enabled, hollow when disabled) and PC arrow
        for (const WorkbenchWindow::ExecBpMark& b : W->ExecBreakpoints)
            if (b.Addr >= r.Addr && b.Addr < r.Addr + r.Size)
            {
                p.setBrush(b.Enabled ? WBColor::Red() : Qt::NoBrush);
                p.setPen(QPen(WBColor::Red(), 2));
                p.drawEllipse(colGutter, y + LineH / 2 - 5, 10, 10);
            }
        if (isPC)
        {
            p.setPen(WBColor::Text());
            p.drawText(colGutter, base, ">");
        }

        // mode gutter: A, T, M, D or ?. A filled chip is confirmed, an outline is inferred, muted is unknown.
        // A conflict with the recording uses the attention chip.
        {
            QChar letter = WBQ::BadgeLetter(r.Class);
            QRect br(ColMode, y + 2, cw * 2 + 2, LineH - 4);
            if (r.Conflict)
            {
                p.setPen(Qt::NoPen);
                p.setBrush(WBColor::ChipRec());
                p.drawRoundedRect(br, 3, 3);
                p.setPen(WBColor::Text());
            }
            else if (r.From == WBQ::Evidence::Recorded && r.Class != WBQ::CodeKind::Unknown)
            {
                p.setPen(Qt::NoPen);
                p.setBrush(WBColor::Off());
                p.drawRoundedRect(br, 3, 3);
                p.setPen(WBColor::Text());
            }
            else if (r.From == WBQ::Evidence::Analysis && r.Class != WBQ::CodeKind::Unknown)
            {
                // marked by unreached-code analysis, distinct from a knowledge-store inference
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(WBColor::Amber(), 1, Qt::DashLine));
                p.drawRoundedRect(br.adjusted(0, 0, -1, -1), 3, 3);
                p.setPen(WBColor::Text());
            }
            else if (r.Class != WBQ::CodeKind::Unknown)
            {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(WBColor::Text()));
                p.drawRoundedRect(br.adjusted(0, 0, -1, -1), 3, 3);
                p.setPen(WBColor::Text());
            }
            else
                p.setPen(WBColor::Muted());
            p.drawText(br, Qt::AlignCenter, r.Conflict ? QString("!") : QString(letter));
        }

        // heat bar
        if (r.Exec)
        {
            int wbar = (int)std::ceil(40.0 * std::log2((double)r.Exec + 1) / lmax);
            p.fillRect(ColHeat, y + 3, wbar, LineH - 6, WBColor::Heat());
        }
        else if (r.Read || r.Write)
        {
            p.fillRect(ColHeat, y + 3, 4, LineH - 6, WBColor::Heat());
        }

        // confirmed data reads as data; unknown bytes and unconfirmed code are muted
        bool dimRow = r.Dim;
        p.setPen(addrc);
        p.drawText(ColAddr, base, QString("%1").arg(r.Addr, 8, 16, QChar('0')));
        if (raw)
        {
            p.setPen(dimc);
            p.drawText(ColBytes, base, r.Bytes);
        }
        p.setPen(dimRow ? dimc : mnemc);
        p.drawText(ColMnem, base, r.Mnemonic);

        // operands, with the branch target or reference replaced by its name and clickable
        int ov = W->OverlayContext(r.Addr);
        QString ops = r.Operands;
        QString linkText;
        u32 linkAddr = 0;
        if (r.HasTarget)
        {
            QString hex = QString("0x%1").arg(r.Target, 0, 16).toUpper().replace("0X", "0x");
            QString n = kb.NameAt(W->Cpu(), r.Target, W->OverlayContext(r.Target), true);
            QString label = n.isEmpty() ? QString("0x%1").arg(r.Target, 8, 16, QChar('0')) : n;
            int at = ops.indexOf(hex, 0, Qt::CaseInsensitive);
            if (at >= 0)
            {
                ops = ops.left(at);
                linkText = label;
                linkAddr = r.Target;
            }
            else if (r.Type == DisasmRow::Data && !n.isEmpty())
            {
                ops += "  ";
                linkText = n;
                linkAddr = r.Target;
            }
        }
        if (ShowData->isChecked() && !r.Ascii.isEmpty()) ops += "  " + r.Ascii;
        p.setPen(dimRow ? dimc : fg);
        p.drawText(ColOps, base, ops);
        if (!linkText.isEmpty())
        {
            int lx = ColOps + fm.horizontalAdvance(ops);
            p.setPen(linkc);
            p.drawText(lx, base, linkText);
            Links.push_back({QRect(lx, y, fm.horizontalAdvance(linkText), LineH), linkAddr});
        }

        // comment column: annotations, literal values, register hints
        int cx = ColCmt;
        auto drawCmt = [&](const QString& s, const QColor& c, u32 link)
        {
            p.setPen(c);
            p.drawText(cx, base, s);
            if (link) Links.push_back({QRect(cx, y, fm.horizontalAdvance(s), LineH), link});
            cx += fm.horizontalAdvance(s) + cw;
        };
        if (r.HasRef && r.Literal && r.LiteralValid)
        {
            QString n = kb.NameAt(W->Cpu(), r.LiteralValue, W->OverlayContext(r.LiteralValue), true);
            drawCmt(QString("; =0x%1").arg(r.LiteralValue, 8, 16, QChar('0')), cmtc, r.LiteralValue);
            if (!n.isEmpty()) drawCmt("<" + n + ">", linkc, r.LiteralValue);
        }
        else if (r.HasRef && !r.Literal)
        {
            QString n = kb.NameAt(W->Cpu(), r.Ref, W->OverlayContext(r.Ref), true);
            drawCmt(QString("; ->%1").arg(n.isEmpty() ? QString("%1").arg(r.Ref, 8, 16, QChar('0')) : n), cmtc, r.Ref);
        }
        for (const WBKB::Annotation* a : kb.AnnotationsAt(W->Cpu(), r.Addr, ov))
        {
            if (a->TargetKind == "range" && a->Addr != r.Addr) continue;
            QString t = a->Comment.section('\n', 0, 0);
            drawCmt((a->AuthorKind == "agent" ? "; [agent] " : "; ") + t, a->AuthorKind == "agent" ? agentc : cmtc, 0);
        }
        if (r.RegMask)
        {
            const WBKB::Function* f = kb.FunctionAt(W->Cpu(), r.Addr, ov);
            for (const WBKB::Annotation* a : kb.RegisterAnnotations(f, r.Addr))
            {
                static const QStringList regs = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
                int ri = regs.indexOf(a->Register);
                if (ri >= 0 && (r.RegMask & (1 << ri)))
                    drawCmt(QString("; %1=%2").arg(a->Register, a->Comment.section('\n', 0, 0)), a->AuthorKind == "agent" ? agentc : cmtc, 0);
            }
        }
        if (r.Exec && width() > cx + cw * 8)
        {
            p.setPen(dimc);
            QString cnt = QString("x%1").arg(r.Exec);
            p.drawText(width() - Bar->width() - fm.horizontalAdvance(cnt) - 4, base, cnt);
        }
    }
    if (Rows.empty())
    {
        p.setPen(dimc);
        p.drawText(rect(), Qt::AlignCenter, H->Status().EmuState == WBStatus::NoGame ? "No game running" : "Loading...");
    }
}

void DisasmPanel::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    for (const Hit& h : Links)
        if (h.R.contains(e->pos()) && e->button() == Qt::LeftButton)
        {
            W->GoTo(h.Addr);
            return;
        }
    int i = RowAt(e->pos().y());
    if (i < 0) return;
    const DisasmRow& r = Rows[i];
    if (W->LockedToPC()) W->SetLockToPC(false);
    if (e->button() == Qt::LeftButton || W->Cursor() != r.Addr)
    {
        W->GoTo(r.Addr, false);
    }
    if (e->pos().x() < ColMode && r.Type != DisasmRow::LabelLine) W->ToggleBreakpointAt(r.Addr);
}

void DisasmPanel::mouseDoubleClickEvent(QMouseEvent* e)
{
    int i = RowAt(e->pos().y());
    if (i < 0) return;
    const DisasmRow& r = Rows[i];
    if (r.HasTarget) W->GoTo(r.Target);
    else if (r.HasRef) W->GoTo(r.Literal && r.LiteralValid ? r.LiteralValue : r.Ref);
}

void DisasmPanel::keyPressEvent(QKeyEvent* e)
{
    auto curIndex = [this]() -> int
    {
        for (int i = 0; i < (int)Rows.size(); i++)
            if (Rows[i].Type != DisasmRow::LabelLine && W->Cursor() >= Rows[i].Addr && W->Cursor() < Rows[i].Addr + Rows[i].Size) return i;
        return -1;
    };
    int i = curIndex();
    switch (e->key())
    {
    case Qt::Key_Down:
        for (int k = i + 1; k < (int)Rows.size(); k++)
            if (Rows[k].Type != DisasmRow::LabelLine) { W->SetLockToPC(false); W->GoTo(Rows[k].Addr, false); break; }
        return;
    case Qt::Key_Up:
        if (i > 0)
        {
            for (int k = i - 1; k >= 0; k--)
                if (Rows[k].Type != DisasmRow::LabelLine) { W->SetLockToPC(false); W->GoTo(Rows[k].Addr, false); return; }
        }
        W->SetLockToPC(false);
        W->GoTo(W->Cursor() - 4, false);
        return;
    case Qt::Key_PageDown: ScrollRows(VisibleRows() - 2); return;
    case Qt::Key_PageUp: ScrollRows(-(VisibleRows() - 2)); return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (i >= 0 && Rows[i].HasTarget) W->GoTo(Rows[i].Target);
        else if (i >= 0 && Rows[i].HasRef) W->GoTo(Rows[i].Literal && Rows[i].LiteralValid ? Rows[i].LiteralValue : Rows[i].Ref);
        return;
    }
    WBPanel::keyPressEvent(e);
}

void DisasmPanel::SetDecode(u32 start, u32 end, const QString& mode)
{
    QString err;
    if (!W->KB().SetDecodeOverride(W->Cpu(), start, end, mode, W->OverlayContext(start), err)) W->ShowError("Decode override", err);
    Fetch();
}

bool DisasmPanel::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip)
    {
        // the mode gutter says why a row is decoded as it is
        auto* he = static_cast<QHelpEvent*>(e);
        int i = RowAt(he->pos().y());
        if (i >= 0 && Rows[i].Type != DisasmRow::LabelLine && he->pos().x() >= ColMnem && he->pos().x() < ColOps && !Rows[i].Explain.isEmpty())
            QToolTip::showText(he->globalPos(), Rows[i].Explain, this);
        else if (i >= 0 && he->pos().x() >= ColMode && he->pos().x() < ColHeat && Rows[i].Type != DisasmRow::LabelLine)
            QToolTip::showText(he->globalPos(), Rows[i].Why, this);
        else
            QToolTip::hideText();
        return true;
    }
    return WBPanel::event(e);
}

void DisasmPanel::contextMenuEvent(QContextMenuEvent* e)
{
    int i = RowAt(e->pos().y());
    u32 a = i >= 0 ? Rows[i].Addr : W->Cursor();
    const DisasmRow* r = i >= 0 ? &Rows[i] : nullptr;
    QMenu m(this);
    m.addAction("Rename\tN", [=]() { W->RenameAt(a); });
    m.addAction("Comment\t;", [=]() { W->CommentAt(a); });
    m.addAction("Define function\tP", [=]() { W->DefineFunctionAt(a); });
    m.addAction("Define data\tD", [=]() { W->DefineDataAt(a); });
    m.addAction("Add bookmark", [=]() { W->AddBookmarkAt(a); });
    m.addSeparator();
    m.addAction("Toggle breakpoint\tF9", [=]() { W->ToggleBreakpointAt(a); });
    bool hasBp = false, bpOn = false;
    for (const WorkbenchWindow::ExecBpMark& b : W->ExecBreakpoints)
        if (r && b.Addr >= r->Addr && b.Addr < r->Addr + r->Size) { hasBp = true; bpOn = b.Enabled; }
    if (hasBp)
    {
        m.addAction(bpOn ? "Disable breakpoint" : "Enable breakpoint", [=]() { W->MutateBreakpointAt(a, bpOn ? 2 : 1); });
        m.addAction("Delete breakpoint", [=]() { W->MutateBreakpointAt(a, 0); });
    }
    if (r && !r->Explain.isEmpty())
        m.addAction("Explain instruction", [this, text = r->Explain]() { QMessageBox::information(this, "Instruction", text); });
    m.addAction("Run to here\tF4", [=]() { W->RunToAddr(a); });
    m.addAction("Cross-references\tX", [=]() { W->ShowXrefs(a); });
    if (r && r->HasTarget) m.addAction(QString("Follow to %1").arg(r->Target, 8, 16, QChar('0')), [=]() { W->GoTo(r->Target); });
    if (r && r->Literal && r->LiteralValid) m.addAction(QString("Follow literal %1").arg(r->LiteralValue, 8, 16, QChar('0')), [=, v = r->LiteralValue]() { W->GoTo(v); });
    m.addSeparator();
    // overrides apply to the selection when the row is in it, else to the row
    u32 s = a, n = r ? r->Size : 4;
    if (W->SelSize() > 1 && a >= W->SelStart() && a < W->SelStart() + W->SelSize()) { s = W->SelStart(); n = W->SelSize(); }
    QMenu* treat = m.addMenu(n > (r ? r->Size : 4) ? QString("Treat %1 bytes as").arg(n) : QString("Treat as"));
    treat->addAction("ARM", [=]() { SetDecode(s, s + n, "arm"); });
    treat->addAction("Thumb", [=]() { SetDecode(s, s + n, "thumb"); });
    treat->addAction("Data", [=]() { SetDecode(s, s + n, "data"); });
    QAction* clr = treat->addAction("Clear override", [=]() { SetDecode(s, s + n, "clear"); });
    clr->setEnabled(r && r->From == WBQ::Evidence::Override);
    QAction* only = m.addAction("Decode only confirmed code", [this](bool on) { OnlyConfirmed = on; Fetch(); });
    only->setCheckable(true);
    only->setChecked(OnlyConfirmed);
    m.addAction("Mode gutter legend...", [this]()
    {
        QMessageBox::information(this, "Mode gutter",
            "A  ARM      T  Thumb      M  both (rare)      D  data      ?  unknown\n\n"
            "Solid: confirmed by the recording (executed in that mode; or read, or a literal pool, and never executed).\n"
            "Outlined: inferred from a knowledge-store function, defined as a data object, or set by an override.\n"
            "Dashed amber: from analysis. While paused, Debug > Mark unreached code follows code that has\n"
            "already run and marks the instructions and literal pools it has not reached. The marks stay\n"
            "until they are cleared or the command is run again. They are not written to the knowledge store.\n"
            "Red !: an override contradicts what was recorded.\n\n"
            "Hover a badge for the evidence. Right-click > Treat as sets an override.");
    });
    m.addSeparator();
    m.addAction("Copy address", [=]() { QApplication::clipboard()->setText(QString("%1").arg(a, 8, 16, QChar('0'))); });
    if (r) m.addAction("Copy line", [=, t = r->Mnemonic + " " + r->Operands]() { QApplication::clipboard()->setText(QString("%1  %2").arg(a, 8, 16, QChar('0')).arg(t)); });
    m.exec(e->globalPos());
}

// ================================================================ memory

MemoryPanel::MemoryPanel(WorkbenchWindow* w) : WBPanel(w)
{
    setFont(MonoFont());
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    Bar = new QScrollBar(Qt::Vertical, this);
    connect(Bar, &QScrollBar::actionTriggered, this, [this](int)
    {
        AddrRange r = ScrollRegion(Top, 0xFFFFFFFF);
        Top = r.Start + (u32)Bar->sliderPosition() * 16;
        Fetch();
    });
    connect(w, &WorkbenchWindow::cpuChanged, this, [this](int) { Fetch(); });
    QFontMetrics fm(font());
    LineH = fm.height() + 2;
    CharW = fm.horizontalAdvance('0');
    Layers = new QToolButton(this);
    Layers->setText("Layers");
    Layers->setAutoRaise(true);
    Layers->setPopupMode(QToolButton::InstantPopup);
    Layers->setToolTip("What the Memory view marks under the bytes");
    QMenu* lm = new QMenu(Layers);
    auto layer = [this, lm](const QString& name, const QString& tip, bool& flag)
    {
        QAction* a = lm->addAction(name);
        a->setCheckable(true);
        a->setChecked(flag);
        a->setToolTip(tip);
        connect(a, &QAction::toggled, this, [this, &flag](bool on) { flag = on; Fetch(); update(); });
    };
    layer("Data objects", "The fields of defined data objects, underlined, with their names", ShowData);
    layer("File structure", "In RAM copies of graphics files: header, palettes, tiles, map entries, cells and textures", ShowFiles);
    lm->setToolTipsVisible(true);
    Layers->setMenu(lm);
    connect(H, &WBHost::graphicsChanged, this, [this]() { Fetch(); });
}

int MemoryPanel::VisibleRows() const { return std::max(1, height() / LineH); }

void MemoryPanel::OnNavigate(u32 addr)
{
    u32 end = Top + VisibleRows() * 16;
    if (addr < Top || addr >= end) Top = (addr & ~15u) >= 0x40 ? (addr & ~15u) - 0x40 : 0;
    Fetch();
}

void MemoryPanel::OnSelection(u32, u32)
{
    update();
}

void MemoryPanel::Refresh()
{
    Fetch();
}

void MemoryPanel::Fetch()
{
    if (Busy) { Pending = true; return; }
    Busy = true;
    int cpu = W->Cpu();
    u32 top = Top;
    u32 n = (u32)(VisibleRows() + 1) * 16;
    struct Res { std::vector<MemByte> Bytes; QString Region; std::vector<WBAsset::RamRun> Files; };
    WBQ::KbHints hints = WBQ::KbHints::Collect(W->KB(), cpu, top, top + n, W->OverlayContext(top));
    WBQ::AnalysisHints guessed = H->AnalysisNear(cpu, top > 8 ? top - 8 : 0, top + n);
    std::shared_ptr<const WBAsset::Catalog> cat = ShowFiles ? H->Graphics() : nullptr;
    H->Query<Res>([this, cpu, top, n, hints, guessed, cat]()
    {
        Res res;
        Workbench& wb = H->Core();
        if (cat) res.Files = WBAsset::RamRuns(wb, *cat, top, n, 8);
        res.Bytes.resize(n);
        if (!wb.GetNDS()) return res;
        u32 maxc = 1;
        std::vector<u32> counts(n);
        for (u32 i = 0; i < n; i++)
        {
            u32 a = top + i;
            PhysAddr pa = wb.Translate((u32)cpu, a);
            MemByte& b = res.Bytes[i];
            if (u8* ptr = wb.PhysPtr(pa)) { b.Value = *ptr; b.Valid = true; }
            const ByteRecord* cr = wb.Rec.Get(pa);
            WBQ::Classification cls = WBQ::Classify(cr, hints, a, &guessed);
            b.Class = cls.Kind;
            b.From = cls.From;
            b.Confirmed = cls.Confirmed() || cls.From == WBQ::Evidence::Override;
            if (const ByteRecord* r = cr)
            {
                u32 c = std::max({r->exec_count, r->read_count, r->write_count, r->dma_read_count + r->dma_write_count});
                counts[i] = c;
                maxc = std::max(maxc, c);
                if (r->exec_count) b.Kind = 1;
                else if (r->write_count >= r->read_count && r->write_count) b.Kind = 3;
                else if (r->read_count) b.Kind = 2;
                else if (r->dma_read_count || r->dma_write_count) b.Kind = 4;
            }
            if (wb.Taint.Enabled() || wb.Taint.MemLabels().PageCount())
                b.Tainted = wb.Taint.GetMemLabel(pa) != 0;
        }
        double lm = std::log2((double)maxc + 1);
        for (u32 i = 0; i < n; i++)
            if (counts[i]) res.Bytes[i].Heat = (u8)std::min(255.0, 60 + 195.0 * std::log2((double)counts[i] + 1) / lm);
        PhysAddr pa = wb.Translate((u32)cpu, top);
        QString rl = QString::fromStdString(RegionLabel(pa.Region));
        u32 canon = WBQ::CanonicalAddr(wb, pa.Region, pa.Offset);
        res.Region = pa.Region ? QString("%1+%2").arg(rl).arg(pa.Offset, 0, 16) : "unmapped";
        if (pa.Region && canon != top && pa.Region != (u16)Region::IO9 && pa.Region != (u16)Region::IO7)
            res.Region += QString(" (mirror of %1)").arg(canon, 8, 16, QChar('0'));
        if (pa.Region == (u16)Region::DTCM) res.Region += " [DTCM mapped here]";
        if (pa.Region == (u16)Region::ITCM) res.Region += " [ITCM]";
        return res;
    }, this, [this, top, cat](Res r)
    {
        Busy = false;
        Bytes = std::move(r.Bytes);
        BytesBase = top;
        FileCat = cat;
        FileRuns = std::move(r.Files);
        RegionText = r.Region;
        AddrRange rg = ScrollRegion(Top, 0xFFFFFFFF);
        Bar->blockSignals(true);
        Bar->setRange(0, (int)((rg.End - rg.Start) / 16));
        Bar->setPageStep(VisibleRows());
        Bar->setValue((int)((Top - rg.Start) / 16));
        Bar->blockSignals(false);
        update();
        if (Pending) { Pending = false; Fetch(); }
    });
}

bool MemoryPanel::FilePartAt(u32 addr, const WBAsset::File*& file, u32& fileOffset, const WBGfx::Part*& part) const
{
    if (!FileCat) return false;
    for (const WBAsset::RamRun& r : FileRuns)
    {
        if (addr < r.Addr || addr >= r.Addr + r.Size || r.File < 0 || r.File >= (int)FileCat->Files().size()) continue;
        file = &FileCat->Files()[r.File];
        fileOffset = r.FileOffset + (addr - r.Addr);
        // the narrowest part holding it (a palette inside PLTT, before PLTT)
        part = nullptr;
        for (const WBGfx::Part& p : file->Desc.Parts)
            if (fileOffset >= p.Offset && fileOffset < p.Offset + p.Size && (!part || p.Size < part->Size)) part = &p;
        return part != nullptr;
    }
    return false;
}

void MemoryPanel::resizeEvent(QResizeEvent*)
{
    int bw = Bar->sizeHint().width();
    Bar->setGeometry(width() - bw, 0, bw, height());
    QSize ls = Layers->sizeHint();
    Layers->setGeometry(width() - bw - ls.width() - 2, 0, ls.width(), std::min(ls.height(), LineH + 2));
    Fetch();
}

void MemoryPanel::wheelEvent(QWheelEvent* e)
{
    int steps = -e->angleDelta().y() / 40;
    if (!steps) steps = e->angleDelta().y() > 0 ? -1 : 1;
    Top += steps * 16;
    Fetch();
}

bool MemoryPanel::ByteAtPos(const QPoint& pt, u32& addr) const
{
    int row = pt.y() / LineH - 1;
    if (row < 0) return false;
    int col = -1;
    if (pt.x() >= ColHex && pt.x() < ColAscii - CharW)
    {
        int rel = pt.x() - ColHex;
        col = rel / (CharW * 3);
        if (col > 7) col = (rel - CharW) / (CharW * 3);
    }
    else if (pt.x() >= ColAscii && pt.x() < ColAscii + 16 * CharW) col = (pt.x() - ColAscii) / CharW;
    if (col < 0 || col > 15) return false;
    addr = Top + row * 16 + col;
    return true;
}

void MemoryPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    QColor bg = WBColor::Base();
    QColor fg = WBColor::Text();
    QColor dimc = WBColor::Muted();
    QColor addrc = WBColor::Muted();
    QColor selbg = WBColor::Select();
    QColor fieldc = WBColor::Field();
    p.fillRect(rect(), bg);
    p.setFont(font());
    QFontMetrics fm(font());
    ColAddr = 4;
    ColHex = ColAddr + CharW * 10;
    ColAscii = ColHex + CharW * (16 * 3 + 2);
    ColFields = ColAscii + CharW * 17;

    p.setPen(dimc);
    p.drawText(ColAddr, fm.ascent() + 1, RegionText);

    WBKB::KnowledgeStore& kb = W->KB();
    int cpu = W->Cpu();
    u32 s0 = W->SelStart(), s1 = W->SelStart() + W->SelSize();
    auto heatColor = [](const MemByte& b) -> QColor
    {
        if (!b.Heat) return QColor();
        return WBColor::Touched();
    };
    int rows = VisibleRows();
    for (int row = 0; row < rows; row++)
    {
        int y = (row + 1) * LineH;
        int base = y + fm.ascent() + 1;
        u32 ra = Top + row * 16;
        p.setPen(addrc);
        p.drawText(ColAddr, base, QString("%1").arg(ra, 8, 16, QChar('0')));
        QStringList fieldNames;
        for (int c = 0; c < 16; c++)
        {
            u32 a = ra + c;
            u32 idx = a - BytesBase;
            if (idx >= Bytes.size()) continue;
            const MemByte& b = Bytes[idx];
            int hx = ColHex + c * CharW * 3 + (c >= 8 ? CharW : 0);
            int ax = ColAscii + c * CharW;
            QRect hr(hx - 1, y, CharW * 2 + 2, LineH);
            QRect ar(ax, y, CharW, LineH);
            if (a >= s0 && a < s1) { p.fillRect(hr, selbg); p.fillRect(ar, selbg); }
            else if (QColor hc = heatColor(b); hc.isValid()) { p.fillRect(hr, hc); p.fillRect(ar, hc); }

            // graphics file parts: a tint per part, alternating between neighbours (tiles, palettes)
            const WBAsset::File* gf;
            u32 fo;
            const WBGfx::Part* gp;
            if (ShowFiles && FilePartAt(a, gf, fo, gp))
            {
                if (!(a >= s0 && a < s1))
                {
                    QColor tint = gp->Kind == "palette" || gp->Kind == "tex_palette" ? WBColor::Teal()
                                : gp->Kind == "tiles" || gp->Kind == "texture" || gp->Kind == "texture_4x4" ? WBColor::Amber()
                                : gp->Kind == "map" || gp->Kind == "cells" || gp->Kind == "oam" ? WBColor::Blue()
                                : WBColor::Muted();
                    int unit = 0;
                    if (gp->Kind == "tiles") unit = (int)((fo - gp->Offset) / (8u * gf->Desc.CharBpp));
                    else if (gp->Index >= 0) unit = gp->Index;
                    tint.setAlpha(unit & 1 ? 34 : 60);
                    p.fillRect(hr, tint);
                    p.fillRect(ar, tint);
                }
                if (fo == gp->Offset) fieldNames << QString::fromStdString(gf->Kind + " " + gp->Label);
            }
            // struct field shading
            int ov = W->OverlayContext(a);
            const WBKB::DataObject* d = ShowData ? kb.DataObjectAt(cpu, a, ov) : nullptr;
            if (d)
            {
                const WBKB::Type* t = kb.TypeByName(d->TypeName);
                u32 off = a - d->Addr;
                int fi = -1;
                if (t)
                    for (int k = 0; k < (int)t->Fields.size(); k++)
                    {
                        const WBKB::TypeField& f = t->Fields[k];
                        if (off >= f.Offset && off < f.Offset + f.Size * f.Count) { fi = k; if (off == f.Offset) fieldNames << f.Name; break; }
                    }
                if (off == 0) fieldNames.prepend((d->Name.isEmpty() ? QString("data_%1").arg(d->Addr, 8, 16, QChar('0')) : d->Name) + ":");
                p.fillRect(QRect(hx - 1, y + LineH - 3, CharW * 2 + 2, 2), fieldc);
            }
            // how the byte decodes (the Disassembly's mode gutter): solid when confirmed, dashed when inferred
            if (b.Class != WBQ::CodeKind::Unknown)
            {
                QColor line = b.From == WBQ::Evidence::Analysis ? WBColor::Amber() : WBColor::Decode();
                if (b.Confirmed || b.From == WBQ::Evidence::Analysis || (a & 1) == 0)
                    p.fillRect(QRect(hx - 1, y, CharW * 2 + 2, 2), line);
            }
            bool annotated = !kb.AnnotationsAt(cpu, a, ov).empty();

            QColor tc = !b.Valid ? dimc : (b.Value == 0 ? dimc : fg);
            p.setPen(tc);
            QString hs = b.Valid ? QString("%1").arg(b.Value, 2, 16, QChar('0')) : "??";
            QFont f = font();
            f.setUnderline(annotated);
            f.setItalic(b.Tainted);
            p.setFont(f);
            p.drawText(hx, base, hs);
            p.setFont(font());
            QChar ch = (b.Valid && b.Value >= 0x20 && b.Value < 0x7F) ? QChar(b.Value) : QChar('.');
            p.setPen(b.Valid && b.Value ? fg : dimc);
            p.drawText(ax, base, QString(ch));
        }
        if (!fieldNames.isEmpty())
        {
            p.setPen(fieldc);
            p.drawText(ColFields, base, fieldNames.join(" "));
        }
    }
}

bool MemoryPanel::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip)
    {
        QHelpEvent* he = static_cast<QHelpEvent*>(e);
        u32 a;
        if (ByteAtPos(he->pos(), a))
        {
            WBKB::KnowledgeStore& kb = W->KB();
            int ov = W->OverlayContext(a);
            QString t = QString("%1").arg(a, 8, 16, QChar('0'));
            QString n = kb.NameAt(W->Cpu(), a, ov, true);
            if (!n.isEmpty()) t += "  " + n;
            if (const WBKB::DataObject* d = kb.DataObjectAt(W->Cpu(), a, ov))
                if (const WBKB::Type* ty = kb.TypeByName(d->TypeName))
                    for (const WBKB::TypeField& f : ty->Fields)
                        if (a - d->Addr >= f.Offset && a - d->Addr < f.Offset + f.Size * f.Count)
                            t += QString("\n%1 %2 (+0x%3)").arg(f.CType, f.Name).arg(f.Offset, 0, 16);
            for (const WBKB::Annotation* an : kb.AnnotationsAt(W->Cpu(), a, ov))
                t += "\n" + (an->AuthorKind == "agent" ? QString("[agent] ") : QString()) + an->Comment;
            const WBAsset::File* gf;
            u32 fo;
            const WBGfx::Part* gp;
            if (ShowFiles && FilePartAt(a, gf, fo, gp))
                t += QString("\n%1 +0x%2\n%3").arg(QString::fromStdString(gf->Path)).arg(fo, 0, 16)
                         .arg(QString::fromStdString(WBGfx::DescribeAt(gf->Desc, gf->Data, gf->Size, fo).Text));
            QToolTip::showText(he->globalPos(), t, this);
        }
        else QToolTip::hideText();
        return true;
    }
    return WBPanel::event(e);
}

void MemoryPanel::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    u32 a;
    if (!ByteAtPos(e->pos(), a)) return;
    if (e->button() == Qt::LeftButton)
    {
        Dragging = true;
        DragStart = a;
        W->SetSelection(a, 1);
    }
    else if (e->button() == Qt::RightButton && !(a >= W->SelStart() && a < W->SelStart() + W->SelSize()))
        W->SetSelection(a, 1);
}

void MemoryPanel::mouseMoveEvent(QMouseEvent* e)
{
    if (!Dragging) return;
    u32 a;
    if (!ByteAtPos(e->pos(), a)) return;
    u32 s = std::min(a, DragStart), en = std::max(a, DragStart);
    W->SetSelection(s, en - s + 1);
}

void MemoryPanel::mouseReleaseEvent(QMouseEvent*)
{
    Dragging = false;
}

void MemoryPanel::keyPressEvent(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Down: Top += 16; Fetch(); return;
    case Qt::Key_Up: Top -= 16; Fetch(); return;
    case Qt::Key_PageDown: Top += (VisibleRows() - 2) * 16; Fetch(); return;
    case Qt::Key_PageUp: Top -= (VisibleRows() - 2) * 16; Fetch(); return;
    }
    WBPanel::keyPressEvent(e);
}

void MemoryPanel::contextMenuEvent(QContextMenuEvent* e)
{
    u32 a = W->SelStart(), n = W->SelSize();
    QMenu m(this);
    m.addAction(QString("Track this value (%1)").arg(ByteCount(n)), [=]() { W->TrackValue(a, n); });
    m.addAction("Write breakpoint", [=]() { W->AddWatchpoint(a, n, (int)BreakKind::Write); });
    m.addAction("Read breakpoint", [=]() { W->AddWatchpoint(a, n, (int)BreakKind::Read); });
    m.addAction("Access breakpoint", [=]() { W->AddWatchpoint(a, n, (int)BreakKind::Access); });
    m.addSeparator();
    m.addAction("Define data\tD", [=]() { W->DefineDataAt(a, n > 1 ? n : 0); });
    m.addAction("Comment\t;", [=]() { W->CommentAt(a, n); });
    m.addAction("Rename / label\tN", [=]() { W->RenameAt(a); });
    m.addAction("Cross-references\tX", [=]() { W->ShowXrefs(a); });
    m.addAction("Disassemble here", [=]() { W->GoTo(a); });
    m.addAction("View as graphics", [=]()
    {
        // the graphics file whose copy holds the selection, else the selected bytes as tiles
        const WBAsset::File* gf;
        u32 fo;
        const WBGfx::Part* gp;
        bool file = FilePartAt(a, gf, fo, gp);
        W->ShowPanel("dock_graphics");
        if (GraphicsPanel* g = W->FindPanel<GraphicsPanel>()) g->ShowRam(a, file ? 0 : (n > 1 ? n : 0x800));
    });
    m.addSeparator();
    m.addAction("Copy address", [=]() { QApplication::clipboard()->setText(QString("%1").arg(a, 8, 16, QChar('0'))); });
    m.addAction("Copy bytes", [=]()
    {
        QString s;
        for (u32 i = 0; i < n; i++)
        {
            u32 idx = a + i - BytesBase;
            if (idx < Bytes.size()) s += QString("%1").arg(Bytes[idx].Value, 2, 16, QChar('0'));
        }
        QApplication::clipboard()->setText(s);
    });
    m.exec(e->globalPos());
}
