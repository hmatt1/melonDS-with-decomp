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

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "Dialogs.h"
#include "WorkbenchWindow.h"
#include "WBHost.h"
#include "WBQueries.h"
#include "ApiServer.h"

using namespace melonDS;
using namespace melonDS::WB;

static QString Hex8(u32 v) { return QString("%1").arg(v, 8, 16, QChar('0')); }

static QWidget* Row(QFormLayout* form, const QString& label, QWidget* field)
{
    form->addRow(label, field);
    return field;
}

// ---------------------------------------------------------------- annotations

AnnotationDialog::AnnotationDialog(WorkbenchWindow* w, const WBKB::Annotation* a, u32 addr, u32 size) : QDialog(w), W(w)
{
    setWindowTitle(a ? "Edit annotation" : "Add annotation");
    QFormLayout* form = new QFormLayout();
    Kind = new QComboBox(this);
    Kind->addItems({"address", "range", "function", "data_object", "struct_field", "type", "register", "overlay", "savestate"});
    Addr = new QLineEdit(Hex8(addr), this);
    End = new QLineEdit(Hex8(addr + std::max<u32>(size, 1)), this);
    Reg = new QComboBox(this);
    Reg->addItems({"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"});
    WholeFunction = new QCheckBox("whole function (otherwise from address to end)", this);
    WholeFunction->setChecked(true);
    TypeName = new QComboBox(this);
    TypeName->setEditable(true);
    for (const auto& kv : W->KB().Types()) TypeName->addItem(kv.first);
    Field = new QComboBox(this);
    Field->setEditable(true);
    Overlay = new QSpinBox(this);
    Overlay->setRange(0, 9999);
    Savestate = new QLineEdit(this);
    Resolved = new QLabel(this);
    Comment = new QPlainTextEdit(this);
    Comment->setMinimumHeight(90);
    Comment->setTabChangesFocus(true);
    Tags = new QLineEdit(this);
    Tags->setPlaceholderText("comma separated, e.g. todo, player");

    form->addRow("Target", Kind);
    RowAddr = Row(form, "Address", Addr);
    RowEnd = Row(form, "End (exclusive)", End);
    RowReg = Row(form, "Register", Reg);
    form->addRow("", WholeFunction);
    RowType = Row(form, "Type", TypeName);
    RowField = Row(form, "Field", Field);
    RowOverlay = Row(form, "Overlay", Overlay);
    RowSavestate = Row(form, "Savestate", Savestate);
    form->addRow("", Resolved);
    form->addRow("Comment", Comment);
    form->addRow("Tags", Tags);

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, this, &AnnotationDialog::Save);
    connect(Kind, &QComboBox::currentTextChanged, this, &AnnotationDialog::UpdateFields);
    connect(TypeName, &QComboBox::currentTextChanged, this, [this](const QString& t)
    {
        Field->clear();
        if (const WBKB::Type* ty = W->KB().TypeByName(t))
            for (const WBKB::TypeField& f : ty->Fields) Field->addItem(f.Name);
    });
    connect(Addr, &QLineEdit::textChanged, this, &AnnotationDialog::UpdateFields);

    if (a)
    {
        Id = a->Id;
        Kind->setCurrentText(a->TargetKind);
        if (a->HasAddr) { Addr->setText(Hex8(a->Addr)); End->setText(Hex8(a->EndAddr)); }
        Reg->setCurrentText(a->Register);
        TypeName->setCurrentText(a->TypeName);
        Field->setCurrentText(a->FieldName);
        if (a->OverlayId >= 0) Overlay->setValue(a->OverlayId);
        Savestate->setText(a->Savestate);
        Comment->setPlainText(a->Comment);
        Tags->setText(a->Tags);
        WholeFunction->setChecked(a->TargetKind != "register" || a->Offset < 0);
    }
    else
    {
        Kind->setCurrentText(size > 1 ? "range" : "address");
        if (const WBKB::DataObject* d = W->KB().DataObjectAt(W->Cpu(), addr, W->OverlayContext(addr)))
        {
            if (const WBKB::Type* t = W->KB().TypeByName(d->TypeName))
            {
                TypeName->setCurrentText(t->Name);
                for (const WBKB::TypeField& f : t->Fields)
                    if (addr - d->Addr >= f.Offset && addr - d->Addr < f.Offset + f.Size * f.Count) Field->setCurrentText(f.Name);
            }
        }
    }
    UpdateFields();
    Comment->setFocus();
    resize(520, 420);
}

void AnnotationDialog::UpdateFields()
{
    QString k = Kind->currentText();
    QFormLayout* form = qobject_cast<QFormLayout*>(static_cast<QVBoxLayout*>(layout())->itemAt(0)->layout());
    auto vis = [form](QWidget* f, bool on)
    {
        f->setVisible(on);
        if (form) if (QWidget* l = form->labelForField(f)) l->setVisible(on);
    };
    vis(RowAddr, k == "address" || k == "range" || k == "function" || k == "data_object" || k == "register");
    vis(RowEnd, k == "range" || (k == "register" && !WholeFunction->isChecked()));
    vis(RowReg, k == "register");
    WholeFunction->setVisible(k == "register");
    vis(RowType, k == "struct_field" || k == "type");
    vis(RowField, k == "struct_field");
    vis(RowOverlay, k == "overlay");
    vis(RowSavestate, k == "savestate");

    u32 a;
    QString res;
    if (W->ParseAddress(Addr->text(), a))
    {
        int ov = W->OverlayContext(a);
        if (k == "function" || k == "register")
        {
            const WBKB::Function* f = W->KB().FunctionAt(W->Cpu(), a, ov);
            res = f ? "function: " + W->KB().NameAt(W->Cpu(), f->Addr, ov) : "no function contains this address";
        }
        else if (k == "data_object")
        {
            const WBKB::DataObject* d = W->KB().DataObjectAt(W->Cpu(), a, ov);
            res = d ? "data object: " + (d->Name.isEmpty() ? Hex8(d->Addr) : d->Name) : "no data object at this address";
        }
        else
        {
            const WBKB::Module* m = W->KB().ModuleFor(W->Cpu(), a, ov);
            res = m ? "module " + m->Name + QString(" + 0x%1").arg(a - m->Load, 0, 16) : "outside every module";
        }
    }
    Resolved->setText(res);
}

void AnnotationDialog::Save()
{
    WBKB::KnowledgeStore& kb = W->KB();
    WBKB::Annotation a;
    a.Id = Id;
    a.TargetKind = Kind->currentText();
    a.Comment = Comment->toPlainText().trimmed();
    a.Tags = Tags->text().trimmed();
    QString k = a.TargetKind;
    u32 addr = 0, end = 0;
    bool haveAddr = W->ParseAddress(Addr->text(), addr);
    W->ParseAddress(End->text(), end);
    int ov = W->OverlayContext(addr);
    int cpu = W->Cpu();
    QString err;
    if (k == "struct_field" || k == "type") a.TypeName = TypeName->currentText().trimmed();
    if (k == "struct_field") a.FieldName = Field->currentText().trimmed();
    if (k == "register") a.Register = Reg->currentText();
    if (k == "overlay") a.OverlayId = Overlay->value();
    if (k == "savestate") a.Savestate = Savestate->text().trimmed();
    if (!kb.ResolveAnnotationTarget(a, cpu, addr, haveAddr, end, ov, WholeFunction->isChecked(), err)) { W->ShowError("Annotation", err); return; }
    if (!kb.SaveAnnotation(a, err)) { W->ShowError("Annotation", err); return; }
    accept();
}

// ---------------------------------------------------------------- functions

FunctionDialog::FunctionDialog(WorkbenchWindow* w, u32 addr) : QDialog(w), W(w)
{
    setWindowTitle("Define function");
    QFormLayout* form = new QFormLayout();
    Addr = new QLineEdit(Hex8(addr & ~1u), this);
    Mode = new QComboBox(this);
    Mode->addItems({"arm", "thumb"});
    Size = new QSpinBox(this);
    Size->setRange(0, 0x100000);
    Size->setDisplayIntegerBase(16);
    Size->setPrefix("0x");
    Name = new QLineEdit(this);
    Signature = new QLineEdit(this);
    Signature->setPlaceholderText("e.g. void func(Player* p, int frame)");
    Subsystem = new QComboBox(this);
    Subsystem->addItems({"unknown", "game", "library", "msl", "runtime"});
    SourceFile = new QLineEdit(this);
    SourceFile->setPlaceholderText("path in the decomp tree, e.g. src/player/move.c");
    form->addRow("Address", Addr);
    form->addRow("Mode", Mode);
    QPushButton* det = new QPushButton("Detect size from recording", this);
    QHBoxLayout* sz = new QHBoxLayout();
    sz->addWidget(Size);
    sz->addWidget(det);
    form->addRow("Size", sz);
    form->addRow("Name", Name);
    form->addRow("Signature", Signature);
    form->addRow("Subsystem", Subsystem);
    form->addRow("Source file", SourceFile);

    int ov = W->OverlayContext(addr);
    if (const WBKB::Function* f = W->KB().FunctionAt(W->Cpu(), addr & ~1u, ov))
    {
        Existing = f->Id;
        setWindowTitle("Edit function");
        Addr->setText(Hex8(f->Addr));
        Addr->setReadOnly(true);
        Mode->setCurrentText(f->Mode);
        Size->setValue((int)f->Size);
        Name->setText(f->Name);
        Signature->setText(f->Signature);
        Subsystem->setCurrentText(f->Subsystem);
        SourceFile->setText(f->SourceFile);
        // an automatic sub_ stops at the return. Opening it proposes a size that includes its literal pool.
        if (f->AuthorKind == "system" && f->Name.startsWith("sub_", Qt::CaseInsensitive))
            Detect();
    }
    else
    {
        int cpu = W->Cpu();
        int m = -1;
        W->Host()->RunSync([&]() { m = WBQ::RecordedMode(W->Host()->Core(), (u32)cpu, addr & ~1u); });
        if (m == 1 || (addr & 1)) Mode->setCurrentIndex(1);
        Detect();
    }

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    QPushButton* del = nullptr;
    if (Existing) del = bb->addButton("Delete", QDialogButtonBox::DestructiveRole);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(bb);
    Name->setFocus();
    connect(det, &QPushButton::clicked, this, &FunctionDialog::Detect);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (del) connect(del, &QPushButton::clicked, this, [this]()
    {
        QString err;
        if (!W->KB().DeleteFunction(Existing, err)) { W->ShowError("Delete function", err); return; }
        accept();
    });
    connect(bb, &QDialogButtonBox::accepted, this, [this]()
    {
        u32 a;
        if (!W->ParseAddress(Addr->text(), a)) { W->ShowError("Function", "bad address"); return; }
        QString err;
        qint64 id = Existing;
        WBKB::KnowledgeStore::Group group(W->KB());     // defining is one change to undo, not an insert and an update
        if (!Existing)
        {
            if (!W->KB().UpsertFunction(W->Cpu(), a, (u32)Size->value(), Mode->currentText(), Name->text().trimmed(), W->OverlayContext(a), err, &id))
            { W->ShowError("Function", err); return; }
        }
        QJsonObject o;
        o["size"] = Size->value();
        o["mode"] = Mode->currentText();
        o["name"] = Name->text().trimmed().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(Name->text().trimmed());
        o["signature"] = Signature->text().trimmed().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(Signature->text().trimmed());
        o["subsystem"] = Subsystem->currentText();
        o["source_file"] = SourceFile->text().trimmed().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(SourceFile->text().trimmed());
        if (!W->KB().UpdateFunction(id, o, err)) { W->ShowError("Function", err); return; }
        accept();
    });
    resize(480, 300);
}

void FunctionDialog::Detect()
{
    u32 a;
    if (!W->ParseAddress(Addr->text(), a)) return;
    a &= ~1u;
    int cpu = W->Cpu();
    u32 size = 0;
    // walk executed instructions from the entry; stop at a gap of never-executed code after a return
    W->Host()->RunSync([&]()
    {
        Workbench& wb = W->Host()->Core();
        u32 p = a;
        u32 lastExec = a;
        bool sawReturn = false;
        for (int n = 0; n < 0x4000; n++)
        {
            int m = WBQ::RecordedMode(wb, (u32)cpu, p);
            if (m < 0)
            {
                // literal pools may sit between code; allow a short gap before a return was seen
                if (sawReturn || p - lastExec > 0x40) break;
                p += 2;
                continue;
            }
            bool thumb = m == 1;
            u32 word = 0;
            WBQ::FetchCode(wb, (u32)cpu, p, thumb, word);
            DisasmInsn in;
            Disassemble(p, word, thumb, in);
            lastExec = p + in.Size;
            if (in.IsReturn && !in.Conditional) sawReturn = true;
            else if (in.IsBranch && !in.IsCall && !in.Conditional && in.HasTarget && (in.Target < a || in.Target > p + 0x1000)) sawReturn = true;
            p += in.Size;
            if (sawReturn)
            {
                // include a trailing literal pool (read but never executed words)
                while (true)
                {
                    const ByteRecord* r = wb.Rec.Get(wb.Translate((u32)cpu, p));
                    if (!r || r->exec_count || !(r->flags & ByteFlag::LiteralPool)) break;
                    p += 4;
                }
                if (WBQ::RecordedMode(wb, (u32)cpu, p) < 0) break;
                sawReturn = false;
            }
        }
        size = lastExec > a ? lastExec - a : 0;
        // extend over the literal pool that follows the last instruction
        while (true)
        {
            const ByteRecord* r = wb.Rec.Get(wb.Translate((u32)cpu, a + size));
            if (!r || r->exec_count || !(r->flags & ByteFlag::LiteralPool)) break;
            size = ((a + size + 4) & ~3u) - a;
        }
    });
    if (size) Size->setValue((int)size);
}

// ---------------------------------------------------------------- data objects

DataDialog::DataDialog(WorkbenchWindow* w, u32 addr, u32 size) : QDialog(w), W(w)
{
    setWindowTitle("Define data");
    QFormLayout* form = new QFormLayout();
    Addr = new QLineEdit(Hex8(addr), this);
    Size = new QSpinBox(this);
    Size->setRange(0, 0x1000000);
    Size->setDisplayIntegerBase(16);
    Size->setPrefix("0x");
    Size->setValue((int)size);
    Type = new QComboBox(this);
    Type->setEditable(true);
    Type->addItem("");
    for (const auto& kv : W->KB().Types()) Type->addItem(kv.first);
    Type->addItems({"u8", "u16", "u32", "s8", "s16", "s32", "fx16", "fx32", "char", "void*"});
    Name = new QLineEdit(this);
    form->addRow("Address", Addr);
    form->addRow("Size (0 = from type)", Size);
    form->addRow("Type", Type);
    form->addRow("Name", Name);
    if (const WBKB::DataObject* d = W->KB().DataObjectAt(W->Cpu(), addr, W->OverlayContext(addr)))
    {
        if (d->Addr == addr)
        {
            Existing = d->Id;
            setWindowTitle("Edit data object");
            Size->setValue((int)d->Size);
            Type->setCurrentText(d->TypeName);
            Name->setText(d->Name);
        }
    }
    connect(Type, &QComboBox::currentTextChanged, this, [this](const QString& t)
    {
        if (const WBKB::Type* ty = W->KB().TypeByName(t)) if (ty->Size > 0) Size->setValue(ty->Size);
    });
    Name->setFocus();
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    QPushButton* del = Existing ? bb->addButton("Delete", QDialogButtonBox::DestructiveRole) : nullptr;
    QVBoxLayout* v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (del) connect(del, &QPushButton::clicked, this, [this]()
    {
        QString err;
        if (!W->KB().DeleteDataObject(Existing, err)) { W->ShowError("Delete data", err); return; }
        accept();
    });
    connect(bb, &QDialogButtonBox::accepted, this, [this]()
    {
        u32 a;
        if (!W->ParseAddress(Addr->text(), a)) { W->ShowError("Data", "bad address"); return; }
        u32 sz = (u32)Size->value();
        if (!sz)
        {
            static const std::map<QString, u32> basic = {{"u8", 1}, {"s8", 1}, {"char", 1}, {"u16", 2}, {"s16", 2}, {"fx16", 2}, {"u32", 4}, {"s32", 4}, {"fx32", 4}, {"void*", 4}};
            auto it = basic.find(Type->currentText());
            if (it != basic.end()) sz = it->second;
        }
        QString err;
        if (!W->KB().DefineData(W->Cpu(), a, sz, Type->currentText().trimmed(), Name->text().trimmed(), W->OverlayContext(a), err))
        { W->ShowError("Data", err); return; }
        accept();
    });
}

// ---------------------------------------------------------------- types

TypeEditorDialog::TypeEditorDialog(WorkbenchWindow* w) : QDialog(w), W(w)
{
    setWindowTitle("Types");
    List = new QListWidget(this);
    Edit = new QPlainTextEdit(this);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    Edit->setFont(mono);
    Edit->setPlaceholderText("struct Player {\n    u32 flags;\n    fx32 pos[3];\n    struct Car* car;\n};");
    Layout = new QLabel(this);
    Layout->setFont(mono);
    Layout->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPushButton* bNew = new QPushButton("New", this);
    QPushButton* bSave = new QPushButton("Save", this);
    QPushButton* bDel = new QPushButton("Delete", this);
    QPushButton* bClose = new QPushButton("Close", this);
    QHBoxLayout* btns = new QHBoxLayout();
    btns->addWidget(bNew);
    btns->addWidget(bSave);
    btns->addWidget(bDel);
    btns->addStretch();
    btns->addWidget(bClose);
    QSplitter* sp = new QSplitter(this);
    sp->addWidget(List);
    QWidget* right = new QWidget(sp);
    QVBoxLayout* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(Edit, 3);
    rv->addWidget(Layout, 2);
    sp->addWidget(right);
    sp->setStretchFactor(1, 3);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->addWidget(sp);
    v->addLayout(btns);
    connect(List, &QListWidget::currentTextChanged, this, &TypeEditorDialog::Select);
    connect(Edit, &QPlainTextEdit::textChanged, this, &TypeEditorDialog::Preview);
    connect(bNew, &QPushButton::clicked, this, [this]() { List->clearSelection(); Edit->setPlainText("struct NewType {\n    u32 field0;\n};"); });
    connect(bSave, &QPushButton::clicked, this, [this]()
    {
        QString err;
        if (!W->KB().DefineType(Edit->toPlainText(), err)) { W->ShowError("Type", err); return; }
        Reload();
    });
    connect(bDel, &QPushButton::clicked, this, [this]()
    {
        if (!List->currentItem()) return;
        QString err;
        if (!W->KB().DeleteType(List->currentItem()->text(), err)) W->ShowError("Type", err);
        Reload();
    });
    connect(bClose, &QPushButton::clicked, this, &QDialog::accept);
    Reload();
    resize(820, 520);
}

void TypeEditorDialog::Reload()
{
    QString cur = List->currentItem() ? List->currentItem()->text() : QString();
    List->clear();
    for (const auto& kv : W->KB().Types()) List->addItem(kv.first);
    auto items = List->findItems(cur, Qt::MatchExactly);
    if (!items.isEmpty()) List->setCurrentItem(items.first());
}

void TypeEditorDialog::Select(const QString& name)
{
    if (const WBKB::Type* t = W->KB().TypeByName(name)) Edit->setPlainText(t->Definition);
}

void TypeEditorDialog::Preview()
{
    WBKB::Type t;
    QString err;
    if (!WBKB::ParseTypeDefinition(Edit->toPlainText(), W->KB().Types(), t, err))
    {
        Layout->setText("error: " + err);
        return;
    }
    QString s = QString("%1 %2, size 0x%3\n").arg(t.Kind, t.Name).arg(t.Size, 0, 16);
    for (const WBKB::TypeField& f : t.Fields)
    {
        s += QString("  +%1  %2 %3").arg(f.Offset, 4, 16, QChar('0')).arg(f.CType, -10).arg(f.Name);
        if (f.Count > 1) s += QString("[%1]").arg(f.Count);
        if (f.BitWidth) s += QString(" : %1 (bit %2)").arg(f.BitWidth).arg(f.BitOffset);
        s += "\n";
    }
    Layout->setText(s);
}

// ---------------------------------------------------------------- settings

SettingsDialog::SettingsDialog(WorkbenchWindow* w) : QDialog(w), W(w)
{
    setWindowTitle("Workbench settings");
    WBSettings& c = W->Host()->Settings();
    QFormLayout* form = new QFormLayout();
    KbPath = new QLineEdit(c.KnowledgeStorePath, this);
    KbPath->setPlaceholderText("default: <melonDS dir>/workbench/<game>_<sha1>/knowledge.db");
    SourceRoot = new QLineEdit(c.SourceRoot, this);
    Compare = new QLineEdit(c.CompareCommand, this);
    Compare->setPlaceholderText("empty: the project's compare script (Build > Compare setup)");
    Editor = new QLineEdit(c.EditorCommand, this);
    Editor->setPlaceholderText("e.g. code -g {file}:{line}");
    Author = new QLineEdit(c.AuthorName, this);
    Cap = new QSpinBox(this);
    Cap->setRange(1, 2000);
    Cap->setSuffix(" M entries");
    Cap->setValue((int)std::max<quint64>(1, c.HistoryCap / 1000000));
    // the API as it is now: started from the command line, it runs whatever the saved settings say
    ApiServer* running = W->Host()->Api();
    if (running && !running->Running()) running = nullptr;
    ApiShownOn = running || c.ApiEnabled;
    ApiShownPort = running ? running->ReadPort() : c.ApiPort;
    ApiShownMemWrite = running ? running->MemoryWriteEnabled() : c.ApiMemoryWrite;
    ApiOn = new QCheckBox("Enable the control and query API (localhost only)", this);
    ApiOn->setChecked(ApiShownOn);
    ApiPort = new QSpinBox(this);
    ApiPort->setRange(1024, 65534);
    ApiPort->setValue(ApiShownPort);
    ApiMemWrite = new QCheckBox("Allow memory.write", this);
    ApiMemWrite->setChecked(ApiShownMemWrite);
    AllRom = new QCheckBox("Data origin: every ROM file is a source", this);
    AllRom->setChecked(c.OriginAllRomFiles);
    AutoImport = new QCheckBox("Import session evidence into the knowledge store", this);
    AutoImport->setChecked(c.AutoImportSessions);
    ApiInfo = new QLabel(this);
    ApiInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ApiServer* api = W->Host()->Api();
    ApiInfo->setText(api && api->Running() ? QString("running: read %1, control %2, token file %3").arg(api->ReadPort()).arg(api->ControlPort()).arg(api->InfoFile()) : "not running");
    form->addRow("Knowledge store", KbPath);
    form->addRow("Decomp source tree", SourceRoot);
    form->addRow("Compare command", Compare);
    form->addRow("Editor command", Editor);
    form->addRow("Author name", Author);
    form->addRow("Write history cap", Cap);
    form->addRow("", AllRom);
    form->addRow("", AutoImport);
    form->addRow("", ApiOn);
    form->addRow("API read port (control = +1)", ApiPort);
    form->addRow("", ApiMemWrite);
    form->addRow("API", ApiInfo);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, this, [this]() { Apply(); accept(); });
    resize(640, 420);
}

void SettingsDialog::Apply()
{
    WBHost* h = W->Host();
    WBSettings& c = h->Settings();
    bool kbChanged = c.KnowledgeStorePath != KbPath->text().trimmed();
    c.KnowledgeStorePath = KbPath->text().trimmed();
    c.SourceRoot = SourceRoot->text().trimmed();
    c.CompareCommand = Compare->text().trimmed();
    c.EditorCommand = Editor->text().trimmed();
    c.AuthorName = Author->text().trimmed();
    c.OriginAllRomFiles = AllRom->isChecked();
    c.AutoImportSessions = AutoImport->isChecked();
    // only what the person changed here is saved, and only a change restarts or stops the API
    bool apiOn = ApiOn->isChecked(), apiMemWrite = ApiMemWrite->isChecked();
    int apiPort = ApiPort->value();
    bool apiChanged = apiOn != ApiShownOn || apiPort != ApiShownPort || apiMemWrite != ApiShownMemWrite;
    if (apiOn != ApiShownOn) c.ApiEnabled = apiOn;
    if (apiPort != ApiShownPort) c.ApiPort = apiPort;
    if (apiMemWrite != ApiShownMemWrite) c.ApiMemoryWrite = apiMemWrite;
    h->SetHistoryCap((quint64)Cap->value() * 1000000ULL);
    h->SaveSettings();
    if (apiChanged)
    {
        if (apiOn) h->StartApi(apiPort, apiMemWrite);
        else if (h->Api()) h->Api()->Stop();
    }
    if (kbChanged && h->Core().Rom.Valid())
    {
        QString path = c.KnowledgeStorePath.isEmpty() ? QDir(h->ProjectDir()).filePath("knowledge.db") : c.KnowledgeStorePath;
        QString err;
        if (!h->KB().Open(path, h->Core().Rom, c.AuthorName, err)) W->ShowError("Knowledge store", err);
        emit h->knowledgeChanged();
    }
}

// ---------------------------------------------------------------- breakpoints

BreakpointDialog::BreakpointDialog(WorkbenchWindow* w, u32 addr) : QDialog(w)
{
    setWindowTitle("Add breakpoint");
    QFormLayout* form = new QFormLayout();
    Kind = new QComboBox(this);
    Kind->addItems({"exec", "read", "write", "access"});
    Addr = new QLineEdit(Hex8(addr), this);
    Size = new QSpinBox(this);
    Size->setRange(1, 0x100000);
    Cpu = new QComboBox(this);
    Cpu->addItems({"arm9", "arm7", "any"});
    Cpu->setCurrentIndex(w->Cpu());
    Condition = new QLineEdit(this);
    Condition->setPlaceholderText("optional, e.g. r0 == 0x10 && [sp+4].w > 3, or value == 0");
    Temporary = new QCheckBox("delete after the first hit", this);
    form->addRow("Kind", Kind);
    form->addRow("Address", Addr);
    form->addRow("Size", Size);
    form->addRow("CPU", Cpu);
    form->addRow("Condition", Condition);
    form->addRow("", Temporary);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    resize(480, 240);
}

// ---------------------------------------------------------------- static analysis import

int ImportAnalysisJson(WBKB::KnowledgeStore& kb, const QString& path, QString& error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { error = "cannot open " + path; return -1; }
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (!doc.isObject()) { error = "not a JSON object: " + perr.errorString(); return -1; }
    QJsonObject root = doc.object();
    QString tool = root.value("tool").toString("import");
    auto addr = [](const QJsonValue& v) -> u32 { u32 a = 0; WBApi::ParseAddr(v, a); return a; };
    int n = 0;
    for (const QJsonValue& v : root.value("functions").toArray())
    {
        QJsonObject o = v.toObject();
        u32 a = addr(o.value("addr"));
        QString mode = o.value("mode").toString((a & 1) ? "thumb" : "arm");
        int cpu = o.value("cpu").toString("arm9") == "arm7" ? 1 : 0;
        QString err;
        if (kb.UpsertFunction(cpu, a & ~1u, (u32)o.value("size").toDouble(), mode, o.value("name").toString(), o.value("overlay").toInt(-1), err)) n++;
    }
    for (const QJsonValue& v : root.value("labels").toArray())
    {
        QJsonObject o = v.toObject();
        QString err;
        int cpu = o.value("cpu").toString("arm9") == "arm7" ? 1 : 0;
        if (kb.SetLabel(cpu, addr(o.value("addr")), o.value("name").toString(), o.value("overlay").toInt(-1), err)) n++;
    }
    for (const QJsonValue& v : root.value("xrefs").toArray())
    {
        QJsonObject o = v.toObject();
        int cpu = o.value("cpu").toString("arm9") == "arm7" ? 1 : 0;
        QString err;
        if (kb.AddXref(cpu, addr(o.value("from")), addr(o.value("to")), o.value("kind").toString("data_ref"), "static", err)) n++;
    }
    for (const QJsonValue& v : root.value("decompiled").toArray())
    {
        QJsonObject o = v.toObject();
        int cpu = o.value("cpu").toString("arm9") == "arm7" ? 1 : 0;
        u32 a = addr(o.value("addr")) & ~1u;
        const WBKB::Function* fn = kb.FunctionStarting(cpu, a, o.value("overlay").toInt(-1));
        QString err;
        if (fn && kb.SetDecompiled(fn->Id, tool, o.value("text").toString(), err)) n++;
    }
    kb.Reload();
    return n;
}
