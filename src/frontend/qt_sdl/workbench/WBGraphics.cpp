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
#include <QDateTime>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

#include "Panels.h"
#include "WBColors.h"
#include "NDS.h"

using namespace melonDS;
using namespace melonDS::WB;

// the image, scaled without smoothing, with an optional 8-pixel grid and marked cells
class GfxImageView : public QWidget
{
public:
    explicit GfxImageView(QWidget* parent) : QWidget(parent) { setMouseTracking(true); }
    void SetImage(const QImage& img) { Img = img; Resize(); update(); }
    void SetZoom(int z) { Zoom = std::max(1, z); Resize(); update(); }
    void SetGrid(int cell) { GridCell = cell; update(); }
    void SetMarks(const std::vector<QRect>& m) { Marks = m; update(); }
    std::function<void(QPoint)> OnClick;
    std::function<QString(QPoint)> OnHover;
    const QImage& Image() const { return Img; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        // a checkerboard shows transparency
        for (int y = 0; y < height(); y += 8)
            for (int x = 0; x < width(); x += 8)
                p.fillRect(x, y, 8, 8, ((x ^ y) & 8) ? QColor(0x99, 0x99, 0x99) : QColor(0x66, 0x66, 0x66));
        if (Img.isNull()) return;
        p.drawImage(QRect(0, 0, Img.width() * Zoom, Img.height() * Zoom), Img);
        if (GridCell > 0 && Zoom >= 2)
        {
            p.setPen(QColor(0, 0, 0, 70));
            for (int x = GridCell; x < Img.width(); x += GridCell) p.drawLine(x * Zoom, 0, x * Zoom, Img.height() * Zoom);
            for (int y = GridCell; y < Img.height(); y += GridCell) p.drawLine(0, y * Zoom, Img.width() * Zoom, y * Zoom);
        }
        p.setPen(QPen(WBColor::Blue(), 2));
        p.setBrush(Qt::NoBrush);
        for (const QRect& r : Marks) p.drawRect(r.x() * Zoom, r.y() * Zoom, r.width() * Zoom, r.height() * Zoom);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        QPoint px(e->position().toPoint() / Zoom);
        if (OnClick && QRect(QPoint(0, 0), Img.size()).contains(px)) OnClick(px);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        QPoint px(e->position().toPoint() / Zoom);
        if (OnHover && QRect(QPoint(0, 0), Img.size()).contains(px)) setToolTip(OnHover(px));
    }

private:
    void Resize() { setFixedSize(std::max(1, Img.width() * Zoom), std::max(1, Img.height() * Zoom)); }
    QImage Img;
    int Zoom = 2;
    int GridCell = 0;
    std::vector<QRect> Marks;
};

GraphicsPanel::GraphicsPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Filter = new QLineEdit(this);
    Filter->setPlaceholderText("Filter graphics files");
    Files = new QTreeWidget(this);
    Files->setHeaderLabels({"File", "Kind", "Size"});
    Files->setRootIsDecorated(false);
    Files->header()->setStretchLastSection(false);
    Files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    QWidget* left = new QWidget(this);
    QVBoxLayout* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(Filter);
    lv->addWidget(Files, 1);

    Source = new QLabel(this);
    Source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Part = new QComboBox(this);
    Item = new QComboBox(this);
    Item->setMinimumContentsLength(14);
    PaletteChoice = new QComboBox(this);
    PaletteChoice->setMinimumContentsLength(18);
    PaletteChoice->setToolTip("Automatic: the same-named palette in the same container, else the one in palette RAM now, else the first one");
    PalNumber = new QSpinBox(this);
    PalNumber->setRange(-1, 255);
    PalNumber->setValue(-1);
    PalNumber->setSpecialValueText("auto");
    PalNumber->setToolTip("4bpp: which 16-colour palette; textures: which TEX0 palette");
    Zoom = new QComboBox(this);
    for (int z : {1, 2, 3, 4, 6, 8}) Zoom->addItem(QString("%1x").arg(z), z);
    Zoom->setCurrentIndex(1);
    Grid = new QCheckBox("Grid", this);
    QToolButton* exp = new QToolButton(this);
    exp->setText("Export");
    exp->setPopupMode(QToolButton::InstantPopup);
    QMenu* em = new QMenu(exp);
    em->addAction("Save this image as PNG...", this, [this]() { Export(false); });
    em->addAction("Save every texture as PNG...", this, [this]() { Export(true); });
    exp->setMenu(em);
    QHBoxLayout* bar = new QHBoxLayout();
    bar->addWidget(Part);
    bar->addWidget(Item);
    bar->addWidget(new QLabel("Palette", this));
    bar->addWidget(PaletteChoice, 1);
    bar->addWidget(PalNumber);
    bar->addWidget(Zoom);
    bar->addWidget(Grid);
    bar->addWidget(exp);

    RawRow = new QWidget(this);
    Bpp = new QComboBox(RawRow);
    for (int b : {2, 4, 8, 16}) Bpp->addItem(QString("%1 bpp").arg(b), b);
    Bpp->setCurrentIndex(1);
    Tiled = new QCheckBox("8x8 tiles", RawRow);
    Tiled->setChecked(true);
    RawWidth = new QSpinBox(RawRow);
    RawWidth->setRange(8, 1024);
    RawWidth->setSingleStep(8);
    RawWidth->setValue(128);
    RawWidth->setSuffix(" px wide");
    RawPalOffset = new QSpinBox(RawRow);
    RawPalOffset->setRange(0, 255);
    RawPalOffset->setPrefix("colours from ");
    QHBoxLayout* rr = new QHBoxLayout(RawRow);
    rr->setContentsMargins(0, 0, 0, 0);
    rr->addWidget(new QLabel("View as:", RawRow));
    rr->addWidget(Bpp);
    rr->addWidget(Tiled);
    rr->addWidget(RawWidth);
    rr->addWidget(RawPalOffset);
    rr->addStretch(1);

    View = new GfxImageView(this);
    Scroll = new QScrollArea(this);
    Scroll->setWidget(View);
    Scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    Info = new QLabel(this);
    Info->setWordWrap(true);
    Info->setTextInteractionFlags(Qt::TextBrowserInteraction);
    Info->setOpenExternalLinks(false);

    QWidget* right = new QWidget(this);
    QVBoxLayout* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(Source);
    rv->addLayout(bar);
    rv->addWidget(RawRow);
    rv->addWidget(Scroll, 1);
    rv->addWidget(Info);
    QSplitter* sp = new QSplitter(Qt::Horizontal, this);
    sp->addWidget(left);
    sp->addWidget(right);
    sp->setStretchFactor(1, 3);
    sp->setSizes({220, 600});
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(sp);

    connect(Filter, &QLineEdit::textChanged, this, [this]() { ReloadFiles(); });
    connect(Files, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it)
    {
        if (it && it->data(0, Qt::UserRole).toString() != SrcPath) ShowFile(it->data(0, Qt::UserRole).toString());
    });
    auto rerender = [this]() { Render(); };
    connect(Part, &QComboBox::currentIndexChanged, this, [this]()
    {
        RawRow->setVisible(Part->currentData().toString() == "raw");
        FillControls();
        Render();
    });
    connect(Item, &QComboBox::currentIndexChanged, this, rerender);
    connect(PaletteChoice, &QComboBox::currentIndexChanged, this, rerender);
    connect(PalNumber, &QSpinBox::valueChanged, this, rerender);
    connect(Bpp, &QComboBox::currentIndexChanged, this, rerender);
    connect(Tiled, &QCheckBox::toggled, this, rerender);
    connect(RawWidth, &QSpinBox::valueChanged, this, rerender);
    connect(RawPalOffset, &QSpinBox::valueChanged, this, rerender);
    connect(Zoom, &QComboBox::currentIndexChanged, this, [this]() { View->SetZoom(Zoom->currentData().toInt()); });
    connect(Grid, &QCheckBox::toggled, this, [this](bool on) { View->SetGrid(on ? 8 : 0); });
    connect(Info, &QLabel::linkActivated, this, [this](const QString& l) { FollowLink(W, QUrl(l)); });
    View->OnClick = [this](QPoint p) { Clicked(p); };
    View->OnHover = [this](QPoint p) -> QString
    {
        u32 off, size;
        QRect cell;
        if (!PixelBytes(p, off, size, cell)) return QString();
        WBGfx::FieldInfo f = WBGfx::DescribeAt(Cur.Desc, Cur.Bytes.data(), (u32)Cur.Bytes.size(), off);
        return QString("(%1, %2) +0x%3: %4").arg(p.x()).arg(p.y()).arg(off, 0, 16).arg(QString::fromStdString(f.Text));
    };
    connect(H, &WBHost::graphicsChanged, this, [this]() { ReloadFiles(); });
    connect(H, &WBHost::romChanged, this, [this]() { HaveSource = false; Cur = WBAsset::Asset(); View->SetImage(QImage()); ReloadFiles(); });
    RawRow->setVisible(false);
    View->SetZoom(2);
    ReloadFiles();
}

void GraphicsPanel::ReloadFiles()
{
    auto cat = H->Graphics();
    Files->blockSignals(true);
    Files->clear();
    QString f = Filter->text().trimmed().toLower();
    if (cat)
        for (const WBAsset::File& x : cat->Files())
        {
            QString path = QString::fromStdString(x.Path);
            QString kind = QString::fromStdString(x.Kind);
            if (!f.isEmpty() && !path.toLower().contains(f) && kind.toLower() != f) continue;
            QTreeWidgetItem* it = new QTreeWidgetItem(Files, {path, kind, QString::number(x.Size)});
            it->setData(0, Qt::UserRole, path);
            if (path == SrcPath) Files->setCurrentItem(it);
        }
    Files->blockSignals(false);
    // palettes to choose from
    QString keep = PaletteChoice->currentData().toString();
    PaletteChoice->blockSignals(true);
    PaletteChoice->clear();
    PaletteChoice->addItem("Automatic", QString());
    PaletteChoice->addItem("Palette RAM: BG main", "@bg");
    PaletteChoice->addItem("Palette RAM: OBJ main", "@obj");
    if (cat)
        for (const WBAsset::File& x : cat->Files())
            if (x.Kind == "NCLR") PaletteChoice->addItem(QString::fromStdString(x.Path), QString::fromStdString(x.Path));
    int k = PaletteChoice->findData(keep);
    PaletteChoice->setCurrentIndex(k >= 0 ? k : 0);
    PaletteChoice->blockSignals(false);
    if (!cat)
    {
        QString st = H->GraphicsStatus();
        Info->setText(st == "indexing" ? "Finding the graphics files..." : HaveSource ? Info->text() : "No graphics files found in this ROM. View as: select bytes in the Memory view, then right-click > View as graphics.");
    }
}

void GraphicsPanel::ShowFile(const QString& path)
{
    SrcPath = path;
    SrcAddr = SrcSize = 0;
    HaveSource = true;
    Format.clear();
    Load();
}

void GraphicsPanel::ShowRam(u32 addr, u32 size)
{
    SrcPath.clear();
    SrcAddr = addr;
    SrcSize = size;
    HaveSource = true;
    Format.clear();
    Load();
}

void GraphicsPanel::Load()
{
    if (!HaveSource || Busy) { Pending = HaveSource; return; }
    Busy = true;
    auto cat = H->Graphics();
    std::string path = SrcPath.toStdString();
    u32 addr = SrcAddr, size = SrcSize;
    struct Loaded { bool Ok = false; std::string Error; WBAsset::Asset A; std::vector<u8> Pal; std::vector<std::string> Video; std::vector<u32> Copies; };
    QPointer<GraphicsPanel> self(this);
    H->Query<std::shared_ptr<Loaded>>([this, cat, path, addr, size]()
    {
        auto r = std::make_shared<Loaded>();
        Workbench& wb = H->Core();
        r->Ok = WBAsset::Load(wb, cat.get(), path, addr, size, r->A, r->Error);
        r->Pal = WBAsset::ReadPaletteRam(wb);
        if (r->Ok && cat && r->A.File >= 0)
        {
            const WBAsset::File& f = cat->Files()[r->A.File];
            NDS* nds = wb.GetNDS();
            if (r->A.RamAddr) r->Copies.push_back(r->A.RamAddr);
            else if (nds && nds->MainRAM)
                for (const WBArc::Match& m : cat->Locate(r->A.File, nds->MainRAM, 0x02000000, nds->MainRAMMask + 1, 4))
                    if (m.Size >= f.Size / 2) r->Copies.push_back(m.Addr - m.MemberOffset);
            r->Video = WBAsset::FindInVideoMemory(wb, f, r->A.Bytes.data(), (u32)r->A.Bytes.size());
        }
        return r;
    }, this, [self](std::shared_ptr<Loaded> r)
    {
        if (!self) return;
        self->Busy = false;
        self->LastLive = QDateTime::currentMSecsSinceEpoch();
        if (!r->Ok)
        {
            self->Source->setText(QString::fromStdString(r->Error));
            self->View->SetImage(QImage());
            return;
        }
        self->Cur = std::move(r->A);
        self->LivePal = std::move(r->Pal);
        self->Video = std::move(r->Video);
        self->Copies = std::move(r->Copies);
        QString fmt = QString::fromStdString(self->Cur.Desc.Format);
        if (fmt != self->Format || self->Part->count() == 0)
        {
            self->Format = fmt;
            self->Part->blockSignals(true);
            self->Part->clear();
            const WBGfx::Description& d = self->Cur.Desc;
            if (d.Format == "NCLR") self->Part->addItem("Palettes", "palette");
            if (d.Format == "NCGR") self->Part->addItem("Tiles", "tiles");
            if (d.Format == "NSCR") self->Part->addItem("Map", "map");
            if (d.Format == "NCER") self->Part->addItem("Cells", "cell");
            if (!d.Textures.empty()) self->Part->addItem("Textures", "texture");
            if (!d.TexPalettes.empty()) self->Part->addItem("Texture palettes", "palette");
            self->Part->addItem("View as...", "raw");
            self->Part->setCurrentIndex(0);
            self->Part->blockSignals(false);
            self->RawRow->setVisible(self->Part->currentData().toString() == "raw");
            self->FillControls();
        }
        self->Render();
        if (self->Pending) { self->Pending = false; self->Load(); }
    });
}

void GraphicsPanel::FillControls()
{
    QString part = Part->currentData().toString();
    const WBGfx::Description& d = Cur.Desc;
    int keep = Item->currentIndex();
    Item->blockSignals(true);
    Item->clear();
    if (part == "cell")
        for (size_t i = 0; i < d.Cells.size(); i++) Item->addItem(QString("cell %1 (%2 OAM)").arg(i).arg(d.Cells[i].Oams.size()));
    else if (part == "texture")
        for (const WBGfx::Texture& t : d.Textures)
            Item->addItem(QString("%1 (%2 %3x%4)").arg(QString::fromStdString(t.Name), WBGfx::TextureFormatName(t.Format)).arg(t.Width).arg(t.Height));
    else if (part == "palette" && !d.TexPalettes.empty())
        for (const WBGfx::TexPalette& tp : d.TexPalettes) Item->addItem(QString::fromStdString(tp.Name));
    Item->setVisible(Item->count() > 0);
    if (keep >= 0 && keep < Item->count()) Item->setCurrentIndex(keep);
    Item->blockSignals(false);
    bool usesPalette = part == "tiles" || part == "map" || part == "cell" || part == "raw";
    PaletteChoice->setEnabled(usesPalette);
    PalNumber->setEnabled(part == "tiles" || part == "texture");
}

WBAsset::RenderOptions GraphicsPanel::Options() const
{
    WBAsset::RenderOptions o;
    o.Part = Part->currentData().toString().toStdString();
    o.Index = std::max(0, Item->currentIndex());
    o.Palette = PalNumber->value();
    o.PalettePath = PaletteChoice->currentData().toString().toStdString();
    o.Bpp = Bpp->currentData().toInt();
    o.Tiled = Tiled->isChecked();
    o.Width = RawWidth->value();
    o.PaletteOffset = RawPalOffset->value();
    return o;
}

void GraphicsPanel::Render()
{
    if (!HaveSource || Cur.Bytes.empty()) return;
    if (Rendering) { Pending = true; return; }
    Rendering = true;
    // decoding runs on a pool thread, on copies
    auto cat = H->Graphics();
    WBAsset::Asset a = Cur;
    WBAsset::RenderOptions o = Options();
    std::vector<u8> pal = LivePal;
    struct Out { QImage Img; WBAsset::RenderInfo Info; std::string Error; };
    QPointer<GraphicsPanel> self(this);
    H->Background<std::shared_ptr<Out>>([cat, a, o, pal]()
    {
        auto r = std::make_shared<Out>();
        r->Img = WBAsset::Render(cat.get(), a, o, pal, r->Info, r->Error);
        return r;
    }, this, [self](std::shared_ptr<Out> r)
    {
        if (!self) return;
        self->Rendering = false;
        self->View->SetImage(r->Img);
        self->Drawn = r->Info;
        if (!r->Error.empty()) self->ClickText = QString::fromStdString(r->Error);
        self->UpdateInfo();
        self->UpdateHighlight();
        if (self->Pending) { self->Pending = false; self->Render(); }
    });
}

void GraphicsPanel::Refresh()
{
    // a RAM source follows the game: read again a few times a second while the panel is shown
    if (!IsShown() || !HaveSource || !Cur.RamAddr || Busy) return;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - LastLive < 250) return;
    LastLive = now;
    if (SrcPath.isEmpty() && SrcSize == 0) SrcAddr = Cur.RamAddr;   // keep following the same copy
    Load();
}

bool GraphicsPanel::PixelBytes(QPoint px, u32& off, u32& size, QRect& cell) const
{
    const WBGfx::Description& d = Cur.Desc;
    QString part = Part->currentData().toString();
    const QImage& img = View->Image();
    if (img.isNull() || px.x() < 0 || px.y() < 0) return false;
    if (part == "palette" && d.Format == "NCLR")
    {
        u32 i = (u32)(px.y() / 8 * 16 + px.x() / 8);
        if (i * 2 >= d.PalSize) return false;
        off = d.PalOffset + i * 2;
        size = 2;
        cell = QRect(px.x() / 8 * 8, px.y() / 8 * 8, 8, 8);
        return true;
    }
    if (part == "palette" && !d.TexPalettes.empty())
    {
        int pi = std::clamp(Item->currentIndex(), 0, (int)d.TexPalettes.size() - 1);
        u32 i = (u32)(px.y() / 8 * 16 + px.x() / 8);
        if (i * 2 >= d.TexPalettes[pi].Size) return false;
        off = d.TexPalettes[pi].Offset + i * 2;
        size = 2;
        cell = QRect(px.x() / 8 * 8, px.y() / 8 * 8, 8, 8);
        return true;
    }
    if (part == "tiles")
    {
        int perRow = std::max(1, img.width() / 8);
        int t = px.y() / 8 * perRow + px.x() / 8;
        cell = QRect(px.x() / 8 * 8, px.y() / 8 * 8, 8, 8);
        return WBGfx::TileBytes(d, t, off, size);
    }
    if (part == "map")
    {
        int cols = std::max(1, d.MapWidth / 8);
        u32 k = (u32)(px.y() / 8 * cols + px.x() / 8);
        u32 es = d.MapFormat == 1 ? 1 : 2;
        if ((k + 1) * es > d.MapSize) return false;
        off = d.MapOffset + k * es;
        size = es;
        cell = QRect(px.x() / 8 * 8, px.y() / 8 * 8, 8, 8);
        return true;
    }
    if (part == "texture")
    {
        int ti = std::max(0, Item->currentIndex());
        if (!WBGfx::TexelBytes(d, ti, px.x(), px.y(), off, size)) return false;
        bool cmp = ti < (int)d.Textures.size() && d.Textures[ti].Format == 5;
        cell = cmp ? QRect(px.x() / 4 * 4, px.y() / 4 * 4, 4, 4) : QRect(px, QSize(1, 1));
        return true;
    }
    if (part == "cell")
    {
        int ci = std::max(0, Item->currentIndex());
        if (ci >= (int)d.Cells.size()) return false;
        const WBGfx::Cell& c = d.Cells[ci];
        int x0 = 1 << 20, y0 = 1 << 20;
        for (const WBGfx::Oam& o : c.Oams) { x0 = std::min(x0, o.X); y0 = std::min(y0, o.Y); }
        for (size_t k = 0; k < c.Oams.size(); k++)
        {
            const WBGfx::Oam& o = c.Oams[k];
            QRect r(o.X - x0, o.Y - y0, o.Width, o.Height);
            if (!r.contains(px)) continue;
            off = c.OamOffset + (u32)k * 6;
            size = 6;
            cell = r;
            return true;
        }
        return false;
    }
    if (part == "raw")
    {
        int bpp = Bpp->currentData().toInt();
        int w = img.width();
        u64 i;
        if (Tiled->isChecked())
        {
            int perRow = std::max(1, w / 8);
            i = (u64)((px.y() / 8) * perRow + px.x() / 8) * 64 + (px.y() % 8) * 8 + px.x() % 8;
            cell = QRect(px.x() / 8 * 8, px.y() / 8 * 8, 8, 8);
        }
        else { i = (u64)px.y() * w + px.x(); cell = QRect(px, QSize(1, 1)); }
        off = (u32)(i * bpp / 8);
        size = std::max(1, bpp / 8);
        if (Tiled->isChecked()) { off = (u32)(i / 64 * 8 * bpp); size = 8u * bpp; }
        return off < Cur.Bytes.size();
    }
    return false;
}

void GraphicsPanel::Clicked(QPoint px)
{
    u32 off, size;
    QRect cell;
    if (!PixelBytes(px, off, size, cell)) return;
    WBGfx::FieldInfo f = WBGfx::DescribeAt(Cur.Desc, Cur.Bytes.data(), (u32)Cur.Bytes.size(), off);
    ClickText = QString("+0x%1: %2").arg(off, 0, 16).arg(QString::fromStdString(f.Text).toHtmlEscaped());
    // its bytes: in RAM when there is a copy, else the file in the Containers or Archive panel
    u32 base = Cur.RamAddr ? Cur.RamAddr : Copies.empty() ? 0 : Copies[0];
    if (base)
    {
        W->GoTo(base + off);
        W->SetSelection(base + off, size);
    }
    else
    {
        auto cat = H->Graphics();
        if (cat && Cur.File >= 0)
        {
            const WBAsset::File& file = cat->Files()[Cur.File];
            if (file.From == WBAsset::File::Container) W->LocateContained(file.Global);
            else if (file.From == WBAsset::File::Archive) W->LocateArchived(file.ArchiveMember);
        }
        ClickText += QString(" <span style='color:%MUTED%'>(not in RAM now: file offset 0x%1)</span>").arg(off, 0, 16);
    }
    View->SetMarks({cell});
    UpdateInfo();
}

void GraphicsPanel::OnSelection(u32 addr, u32 size)
{
    SelA = addr;
    SelN = size;
    if (IsShown()) UpdateHighlight();
}

void GraphicsPanel::UpdateHighlight()
{
    u32 base = Cur.RamAddr ? Cur.RamAddr : Copies.empty() ? 0 : Copies[0];
    const QImage& img = View->Image();
    std::vector<QRect> marks;
    if (base && SelN && !img.isNull() && SelA < base + Cur.Bytes.size() && SelA + SelN > base)
    {
        u32 lo = SelA > base ? SelA - base : 0, hi = std::min<u32>(SelA + SelN - base, (u32)Cur.Bytes.size());
        int step = Part->currentData().toString() == "texture" ? 4 : 8;
        std::set<std::pair<int, int>> seen;
        for (int y = 0; y < img.height() && marks.size() < 512; y += step)
            for (int x = 0; x < img.width() && marks.size() < 512; x += step)
            {
                u32 off, size;
                QRect cell;
                if (!PixelBytes(QPoint(x, y), off, size, cell)) continue;
                if (off < hi && off + size > lo && seen.insert({cell.x(), cell.y()}).second) marks.push_back(cell);
            }
    }
    View->SetMarks(marks);
}

void GraphicsPanel::UpdateInfo()
{
    QString h;
    QString src = Cur.Path.empty() ? QString("RAM %1-%2").arg(Cur.RamAddr, 8, 16, QChar('0')).arg(Cur.RamAddr + (u32)Cur.Bytes.size(), 8, 16, QChar('0'))
                                   : QString::fromStdString(Cur.Path);
    Source->setText(QString("<b>%1</b> %2").arg(src.toHtmlEscaped(), Cur.Desc.Format.empty() ? QString("(no graphics file: view as tiles)")
                                                                                              : QString::fromStdString(Cur.Desc.Format)));
    if (Cur.Desc.UnreadableFrom)
        h += QString("<span style='color:%RED%'>Understood up to +0x%1: %2</span><br>").arg(Cur.Desc.UnreadableFrom, 0, 16)
                 .arg(QString::fromStdString(Cur.Desc.Problem).toHtmlEscaped());
    if (Cur.RamAddr && !Cur.Path.empty())
        h += QString("Read from RAM at <a href=\"sel:%1:%2\">%1</a> (updates while the game runs). ").arg(Cur.RamAddr, 8, 16, QChar('0')).arg(Cur.Bytes.size(), 0, 16);
    else if (!Copies.empty())
    {
        QStringList l;
        for (u32 c : Copies) l << QString("<a href=\"sel:%1:%2\">%1</a>").arg(c, 8, 16, QChar('0')).arg(Cur.Bytes.size(), 0, 16);
        h += "In RAM at " + l.join(", ") + ". ";
    }
    else if (!Cur.Path.empty()) h += "Not in RAM now. ";
    if (!Video.empty())
    {
        QStringList l;
        for (const std::string& v : Video) l << QString::fromStdString(v).toHtmlEscaped();
        h += "Now in " + l.join("; ") + ". ";
    }
    if (!Drawn.Palette.empty()) h += "Colours: " + QString::fromStdString(Drawn.Palette).toHtmlEscaped() + ". ";
    if (!Drawn.Tiles.empty()) h += "Tiles: " + QString::fromStdString(Drawn.Tiles).toHtmlEscaped() + ". ";
    if (!ClickText.isEmpty()) h += "<br>" + ClickText;
    Info->setText(WBColor::ApplyHtmlColors(h));
}

void GraphicsPanel::Export(bool allTextures)
{
    if (!allTextures)
    {
        if (View->Image().isNull()) return;
        QString name = Cur.Path.empty() ? QString("ram_%1").arg(Cur.RamAddr, 8, 16, QChar('0'))
                                        : QFileInfo(QString::fromStdString(Cur.Path).section(':', -1)).completeBaseName();
        QString p = QFileDialog::getSaveFileName(this, "Save image", name + ".png", "PNG (*.png)");
        if (!p.isEmpty() && !View->Image().save(p, "PNG")) W->ShowError("Export", "cannot write " + p);
        return;
    }
    const WBGfx::Description& d = Cur.Desc;
    if (d.Textures.empty()) { W->ShowError("Export", "This file has no textures."); return; }
    QString dir = QFileDialog::getExistingDirectory(this, "Save every texture into");
    if (dir.isEmpty()) return;
    int saved = 0;
    for (size_t i = 0; i < d.Textures.size(); i++)
    {
        QImage img = WBGfx::RenderTexture(d, Cur.Bytes.data(), (u32)Cur.Bytes.size(), (int)i);
        QString n = QString::fromStdString(d.Textures[i].Name);
        n.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
        if (!img.isNull() && img.save(QDir(dir).filePath(n + ".png"), "PNG")) saved++;
    }
    W->Notice(QString("Saved %1 textures in %2").arg(saved).arg(dir), 5000);
}
