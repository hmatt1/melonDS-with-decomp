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

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

#include "Panels.h"
#include "WBCodeEdit.h"
#include "WBColors.h"

using namespace melonDS;

QTableWidget* MakeTable(QWidget* parent, const QStringList& headers);
QTableWidgetItem* Cell(const QString& text, const QVariant& data = QVariant());

static QFont Mono()
{
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    return f;
}

bool ConfirmCompareCommand(QWidget* parent, WBHost* h, const WBHost::CompareJob& job)
{
    if (h->CompareApproved(job)) return true;
    QDialog d(parent);
    d.setWindowTitle("Run this compare command?");
    QLabel* text = new QLabel(QString("Compare runs this command on your computer. It hasn't run here before, or it (or "
                                      "the project's compare script, %1) changed since it was last approved.")
                                  .arg(h->CompareScriptPath().toHtmlEscaped()), &d);
    text->setWordWrap(true);
    QPlainTextEdit* cmd = new QPlainTextEdit(job.Command, &d);
    cmd->setReadOnly(true);
    cmd->setFont(Mono());
    cmd->setMinimumSize(640, 110);
    QDialogButtonBox* bb = new QDialogButtonBox(&d);
    bb->addButton("Run it", QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    QVBoxLayout* v = new QVBoxLayout(&d);
    v->addWidget(text);
    v->addWidget(cmd);
    v->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return false;
    h->ApproveCompare(job);
    return true;
}

// ================================================================ checklist

CompareChecklist::CompareChecklist(WorkbenchWindow* w, QWidget* parent) : QFrame(parent), W(w), H(w->Host())
{
    setFrameShape(QFrame::StyledPanel);
    Title = new QLabel("<b>Compare setup</b>", this);
    Collapse = new QToolButton(this);
    Collapse->setAutoRaise(true);
    Collapse->setText("Hide");
    QPushButton* open = new QPushButton("Compare setup...", this);
    QHBoxLayout* top = new QHBoxLayout();
    top->addWidget(Title, 1);
    top->addWidget(open);
    top->addWidget(Collapse);
    Body = new QWidget(this);
    Grid = new QGridLayout(Body);
    Grid->setContentsMargins(0, 0, 0, 0);
    Grid->setColumnStretch(2, 1);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 4, 6, 4);
    v->addLayout(top);
    v->addWidget(Body);
    connect(Collapse, &QToolButton::clicked, this, [this]()
    {
        Collapsed = !Collapsed;
        Body->setVisible(!Collapsed);
        Collapse->setText(Collapsed ? "Show" : "Hide");
    });
    connect(open, &QPushButton::clicked, this, [this]() { W->ShowPanel("dock_compare_setup"); });
    if (CompareSetupPanel* sp = W->FindPanel<CompareSetupPanel>())
        connect(sp, &CompareSetupPanel::setupChanged, this, [this]() { Check(FuncId, true); });
}

void CompareChecklist::Check(qint64 functionId, bool force)
{
    FuncId = functionId;
    const WBKB::Function* f = functionId ? H->KB().FunctionById(functionId) : nullptr;
    const WBKB::Module* m = f ? H->KB().ModuleById(f->ModuleId) : nullptr;
    QString module = m ? m->Name : QString();
    WBCmp::CompareConfig cc;
    QString err;
    QString label = H->LoadCompareConfig(cc, err) ? cc.Resolve(module, f ? f->Addr : 0).Label : QString();
    if (!force && Checked && module == Module && label == Label) return;
    Checked = true;
    Module = module;
    Label = label;
    int gen = ++Gen;
    QPointer<CompareChecklist> self(this);
    H->CheckCompareSetup(functionId, this, [self, gen](std::vector<WBHost::SetupRow> rows)
    {
        if (!self || gen != self->Gen) return;
        while (QLayoutItem* it = self->Grid->takeAt(0))
        {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        bool ready = true, all = true;
        int failing = 0;
        for (int i = 0; i < (int)rows.size(); i++)
        {
            const WBHost::SetupRow& r = rows[i];
            if (!r.Ok) { all = false; failing++; if (!r.Optional && r.Id != "self_test") ready = false; }
            QLabel* mark = new QLabel(r.Ok ? QString::fromUtf8("✓") : QString::fromUtf8("✗"));
            QPalette pal = mark->palette();
            pal.setColor(QPalette::WindowText, r.Ok ? WBColor::Green() : (r.Optional ? WBColor::Amber() : WBColor::Red()));
            mark->setPalette(pal);
            QLabel* name = new QLabel(r.Label);
            QLabel* msg = new QLabel(r.Message.toHtmlEscaped());
            msg->setWordWrap(true);
            msg->setTextInteractionFlags(Qt::TextBrowserInteraction);
            msg->setOpenExternalLinks(true);
            if (r.Id == "objdiff" && !r.Ok)
                msg->setText(msg->text() + " Get it from <a href='https://github.com/encounter/objdiff/releases'>github.com/encounter/objdiff</a>.");
            self->Grid->addWidget(mark, i, 0);
            self->Grid->addWidget(name, i, 1);
            self->Grid->addWidget(msg, i, 2);
            if (!r.Ok)
            {
                QString text = r.Fix == "python" ? "Locate python.exe..."
                    : r.Fix == "compiler" ? (self->Label.isEmpty() ? "Compiler settings..." : "Browse for the compiler...")
                    : r.Fix == "objdiff" ? "Browse for objdiff-cli..."
                    : r.Fix == "source_root" ? "Choose folder..."
                    : r.Fix == "settings" ? (self->Module.isEmpty() ? "Edit compare.json..." : QString("Add settings for %1...").arg(self->Module))
                    : r.Fix == "self_test" ? "Run self-test" : "Fix...";
                QPushButton* b = new QPushButton(text);
                QString fix = r.Fix;
                QObject::connect(b, &QPushButton::clicked, self, [self, fix]() { if (self) self->Fix(fix); });
                self->Grid->addWidget(b, i, 3);
            }
        }
        self->Title->setText(all ? "<b>Compare setup</b>: ready"
                                 : QString("<b>Compare setup</b>: %1 to do%2").arg(failing).arg(ready ? " (Compare can run)" : ""));
        self->setVisible(!all);
        self->IsReady = ready;
        emit self->changed(ready);
    });
}

void CompareChecklist::Fix(const QString& fix)
{
    WBSettings& c = H->Settings();
    if (fix == "python")
    {
        QString p = QFileDialog::getOpenFileName(this, "Locate python.exe", c.PythonPath, "Python (python.exe python3.exe);;Programs (*.exe)");
        if (p.isEmpty()) return;
        c.PythonPath = QDir::toNativeSeparators(p);
    }
    else if (fix == "objdiff")
    {
        QString p = QFileDialog::getOpenFileName(this, "Locate objdiff-cli", c.ObjdiffPath, "objdiff-cli (objdiff-cli*.exe);;Programs (*.exe)");
        if (p.isEmpty()) return;
        c.ObjdiffPath = QDir::toNativeSeparators(p);
    }
    else if (fix == "compiler")
    {
        if (Label.isEmpty()) { Fix("settings"); return; }
        QString p = QFileDialog::getOpenFileName(this, QString("Locate the compiler for '%1'").arg(Label), H->CompilerPath(Label),
                                                 "Compilers (mwccarm.exe *gcc*.exe);;Programs (*.exe)");
        if (p.isEmpty()) return;
        H->SetCompilerPath(Label, QDir::toNativeSeparators(p));
    }
    else if (fix == "source_root")
    {
        QString d = QFileDialog::getExistingDirectory(this, "Source tree (the folder holding the C files)", c.SourceRoot);
        if (d.isEmpty()) return;
        c.SourceRoot = QDir::toNativeSeparators(d);
    }
    else if (fix == "settings")
    {
        QString err;
        if (!H->EnsureCompareProject(err)) { W->ShowError("Compare setup", err); return; }
        WBCmp::CompareConfig cc;
        if (H->LoadCompareConfig(cc, err) && !Module.isEmpty() && cc.Resolve(Module, 0).From.isEmpty())
        {
            bool ok = false;
            QString label = QInputDialog::getText(this, "Compiler settings",
                QString("Compiler version label for %1 (its executable is chosen next):").arg(Module), QLineEdit::Normal,
                cc.Default.Compiler.isEmpty() ? QString("mwcc-2.0-sp1p5") : cc.Default.Compiler, &ok);
            if (!ok || label.trimmed().isEmpty()) return;
            WBCmp::CompareConfig::Rule r;
            r.Module = Module;
            r.Compiler = label.trimmed();
            r.Flags = cc.Default.Flags.isEmpty() ? QString("-O4,p -proc arm946e -interworking -enum int -char signed -nostdinc") : cc.Default.Flags;
            cc.Rules.push_back(r);
            QFile f(H->CompareConfigPath());
            if (!f.open(QIODevice::WriteOnly) || f.write(cc.ToJson()) < 0) { W->ShowError("Compare setup", "cannot write " + f.fileName()); return; }
        }
        W->ShowPanel("dock_compare_setup");
        if (CompareSetupPanel* sp = W->FindPanel<CompareSetupPanel>()) sp->ShowTab(1);
        Check(FuncId, true);
        return;
    }
    else if (fix == "self_test")
    {
        W->ShowPanel("dock_compare_setup");
        if (CompareSetupPanel* sp = W->FindPanel<CompareSetupPanel>()) { sp->ShowTab(2); sp->RunSelfTest(); }
        return;
    }
    H->SaveSettings();
    if (CompareSetupPanel* sp = W->FindPanel<CompareSetupPanel>()) emit sp->setupChanged();
    else Check(FuncId, true);
}

// ================================================================ setup panel

CompareSetupPanel::CompareSetupPanel(WorkbenchWindow* w) : WBPanel(w)
{
    Tabs = new QTabWidget(this);

    // script
    QWidget* st = new QWidget(Tabs);
    Script = new CodeEditor(st, CodeEditor::Python);
    ScriptState = new QLabel(st);
    ScriptState->setWordWrap(true);
    QPushButton* save = new QPushButton("Save", st);
    QPushButton* reset = new QPushButton("Reset to template...", st);
    QPushButton* changes = new QPushButton("Show changes from template", st);
    QHBoxLayout* sb = new QHBoxLayout();
    sb->addWidget(save);
    sb->addWidget(changes);
    sb->addWidget(reset);
    sb->addStretch(1);
    QVBoxLayout* sv = new QVBoxLayout(st);
    sv->addWidget(ScriptState);
    sv->addWidget(Script, 1);
    sv->addLayout(sb);
    Tabs->addTab(st, "Script");
    connect(save, &QPushButton::clicked, this, &CompareSetupPanel::SaveScript);
    connect(reset, &QPushButton::clicked, this, &CompareSetupPanel::ResetScript);
    connect(changes, &QPushButton::clicked, this, &CompareSetupPanel::ShowChanges);

    // settings: compare.json (the project's, shared) and this computer's tool paths
    QWidget* cfgTab = new QWidget(Tabs);
    Config = new CodeEditor(cfgTab, CodeEditor::Json);
    ConfigState = new QLabel(cfgTab);
    ConfigState->setWordWrap(true);
    QPushButton* saveCfg = new QPushButton("Save compare.json", cfgTab);
    QWidget* local = new QWidget(cfgTab);
    Python = new QLineEdit(local);
    Python->setPlaceholderText("python (on PATH)");
    Objdiff = new QLineEdit(local);
    Objdiff->setPlaceholderText("objdiff-cli (on PATH)");
    Timeout = new QSpinBox(local);
    Timeout->setRange(5, 3600);
    Timeout->setSuffix(" s");
    Compilers = MakeTable(local, {"Compiler label", "Executable on this computer"});
    Compilers->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    Compilers->horizontalHeader()->setStretchLastSection(true);
    QPushButton* addComp = new QPushButton("Add", local);
    QPushButton* browseComp = new QPushButton("Browse...", local);
    QPushButton* delComp = new QPushButton("Remove", local);
    QPushButton* saveLocal = new QPushButton("Save paths", local);
    auto browseRow = [this](QLineEdit* e, const QString& title, const QString& filter)
    {
        QPushButton* b = new QPushButton("Browse...", e->parentWidget());
        connect(b, &QPushButton::clicked, this, [this, e, title, filter]()
        {
            QString p = QFileDialog::getOpenFileName(this, title, e->text(), filter);
            if (!p.isEmpty()) e->setText(QDir::toNativeSeparators(p));
        });
        QHBoxLayout* h = new QHBoxLayout();
        h->addWidget(e, 1);
        h->addWidget(b);
        return h;
    };
    QFormLayout* form = new QFormLayout(local);
    form->addRow("Python", browseRow(Python, "Locate python.exe", "Python (python.exe python3.exe);;Programs (*.exe)"));
    form->addRow("objdiff-cli", browseRow(Objdiff, "Locate objdiff-cli", "objdiff-cli (objdiff-cli*.exe);;Programs (*.exe)"));
    form->addRow("Timeout", Timeout);
    QHBoxLayout* cb = new QHBoxLayout();
    cb->addWidget(addComp);
    cb->addWidget(browseComp);
    cb->addWidget(delComp);
    cb->addStretch(1);
    cb->addWidget(saveLocal);
    form->addRow(Compilers);
    form->addRow(cb);
    QWidget* shared = new QWidget(cfgTab);
    QVBoxLayout* shv = new QVBoxLayout(shared);
    shv->setContentsMargins(0, 0, 0, 0);
    shv->addWidget(new QLabel("<b>compare.json</b> (this game's project folder; safe to share): the compiler label and flags "
                              "for each module or address range", shared));
    shv->addWidget(Config, 1);
    shv->addWidget(ConfigState);
    shv->addWidget(saveCfg, 0, Qt::AlignLeft);
    QWidget* localBox = new QWidget(cfgTab);
    QVBoxLayout* lv = new QVBoxLayout(localBox);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(new QLabel("<b>This computer</b>: where each tool is (kept in melonDS's settings, not the project)", localBox));
    lv->addWidget(local, 1);
    QSplitter* cs = new QSplitter(Qt::Vertical, cfgTab);
    cs->addWidget(shared);
    cs->addWidget(localBox);
    QVBoxLayout* cv = new QVBoxLayout(cfgTab);
    cv->addWidget(cs);
    Tabs->addTab(cfgTab, "Settings");
    connect(Config, &QPlainTextEdit::textChanged, this, &CompareSetupPanel::ValidateConfig);
    connect(saveCfg, &QPushButton::clicked, this, &CompareSetupPanel::SaveConfig);
    connect(saveLocal, &QPushButton::clicked, this, &CompareSetupPanel::SaveLocal);
    connect(addComp, &QPushButton::clicked, this, [this]()
    {
        int r = Compilers->rowCount();
        Compilers->insertRow(r);
        Compilers->setItem(r, 0, new QTableWidgetItem("mwcc-2.0-sp1p5"));
        Compilers->setItem(r, 1, new QTableWidgetItem(""));
        Compilers->setCurrentCell(r, 0);
        Compilers->editItem(Compilers->item(r, 0));
    });
    connect(browseComp, &QPushButton::clicked, this, [this]()
    {
        int r = Compilers->currentRow();
        if (r < 0) return;
        QString cur = Compilers->item(r, 1) ? Compilers->item(r, 1)->text() : QString();
        QString p = QFileDialog::getOpenFileName(this, "Locate the compiler", cur, "Compilers (mwccarm.exe *gcc*.exe);;Programs (*.exe)");
        if (!p.isEmpty()) Compilers->setItem(r, 1, new QTableWidgetItem(QDir::toNativeSeparators(p)));
    });
    connect(delComp, &QPushButton::clicked, this, [this]() { if (Compilers->currentRow() >= 0) Compilers->removeRow(Compilers->currentRow()); });

    // run: the command, its expansion for the function at the cursor, and what it printed
    QWidget* rt = new QWidget(Tabs);
    Command = new QLineEdit(rt);
    Command->setFont(Mono());
    Command->setPlaceholderText(WBHost::DefaultCompareCommand());
    QPushButton* saveCmd = new QPushButton("Save command", rt);
    Preview = new QPlainTextEdit(rt);
    Preview->setReadOnly(true);
    Preview->setFont(Mono());
    Preview->setMaximumHeight(70);
    Record = new QCheckBox("Record this attempt", rt);
    RunBtn = new QPushButton("Run on this function", rt);
    SelfTestBtn = new QPushButton("Self-test", rt);
    SelfTestBtn->setToolTip("Compiles a tiny built-in function and diffs it against itself: a working toolchain scores 100");
    OutStdout = new QPlainTextEdit(rt);
    OutStdout->setReadOnly(true);
    OutStdout->setFont(Mono());
    OutStderr = new QPlainTextEdit(rt);
    OutStderr->setReadOnly(true);
    OutStderr->setFont(Mono());
    OutResult = new QTextBrowser(rt);
    QHBoxLayout* cmdRow = new QHBoxLayout();
    cmdRow->addWidget(new QLabel("Command", rt));
    cmdRow->addWidget(Command, 1);
    cmdRow->addWidget(saveCmd);
    QHBoxLayout* runRow = new QHBoxLayout();
    runRow->addWidget(RunBtn);
    runRow->addWidget(Record);
    runRow->addStretch(1);
    runRow->addWidget(SelfTestBtn);
    auto titled = [rt](const QString& t, QWidget* wdg)
    {
        QWidget* box = new QWidget(rt);
        QVBoxLayout* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(new QLabel(t, box));
        v->addWidget(wdg, 1);
        return box;
    };
    QSplitter* outs = new QSplitter(Qt::Horizontal, rt);
    outs->addWidget(titled("Result", OutResult));
    outs->addWidget(titled("stdout", OutStdout));
    outs->addWidget(titled("stderr", OutStderr));
    QVBoxLayout* rv = new QVBoxLayout(rt);
    rv->addLayout(cmdRow);
    rv->addWidget(new QLabel("Runs as (for the function at the cursor):", rt));
    rv->addWidget(Preview);
    rv->addLayout(runRow);
    rv->addWidget(outs, 1);
    Tabs->addTab(rt, "Run");
    connect(Command, &QLineEdit::textChanged, this, &CompareSetupPanel::UpdatePreview);
    connect(saveCmd, &QPushButton::clicked, this, &CompareSetupPanel::SaveCommand);
    connect(RunBtn, &QPushButton::clicked, this, [this]() { Run(false); });
    connect(SelfTestBtn, &QPushButton::clicked, this, [this]() { Run(true); });

    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(Tabs);
    connect(H, &WBHost::romChanged, this, [this]() { Loaded = false; if (IsShown()) Load(); });
    connect(Tabs, &QTabWidget::currentChanged, this, [this]() { if (!Loaded) Load(); });
}

void CompareSetupPanel::ShowTab(int index)
{
    if (!Loaded) Load();
    Tabs->setCurrentIndex(index);
}

void CompareSetupPanel::OnNavigate(u32 addr)
{
    if (!IsShown()) return;
    if (!Loaded) Load();
    const WBKB::Function* f = W->KB().FunctionAt(W->Cpu(), addr, W->OverlayContext(addr));
    qint64 id = f ? f->Id : 0;
    if (id == FuncId) return;
    FuncId = id;
    UpdatePreview();
}

void CompareSetupPanel::Load()
{
    const WBSettings& c = H->Settings();
    Python->setText(c.PythonPath);
    Objdiff->setText(c.ObjdiffPath);
    Timeout->setValue(c.CompareTimeoutSec);
    Compilers->setRowCount(0);
    for (const QString& e : c.Compilers)
    {
        int r = Compilers->rowCount();
        Compilers->insertRow(r);
        Compilers->setItem(r, 0, new QTableWidgetItem(e.section('=', 0, 0).trimmed()));
        Compilers->setItem(r, 1, new QTableWidgetItem(e.section('=', 1).trimmed()));
    }
    Command->blockSignals(true);
    Command->setText(c.CompareCommand);
    Command->blockSignals(false);
    if (!W->KB().IsOpen())
    {
        ScriptState->setText("Load a game first: the compare script and compare.json live in the game's project folder.");
        Script->clear();
        Config->clear();
        Loaded = false;
        UpdatePreview();
        return;
    }
    QString err;
    if (!H->EnsureCompareProject(err)) ScriptState->setText(err.toHtmlEscaped());
    QFile sf(H->CompareScriptPath());
    QByteArray script = sf.open(QIODevice::ReadOnly) ? sf.readAll() : QByteArray();
    Script->setPlainText(QString::fromUtf8(script));
    Script->document()->setModified(false);
    bool same = script == WBHost::CompareScriptTemplate();
    ScriptState->setText(QString("%1<br><span style='color:%MUTED%'>%2</span>")
        .arg(H->CompareScriptPath().toHtmlEscaped(),
             same ? "The template this build ships, unchanged." : "Edited: differs from the template this build ships."));
    ScriptState->setText(WBColor::ApplyHtmlColors(ScriptState->text()));
    QFile cf(H->CompareConfigPath());
    Config->setPlainText(cf.open(QIODevice::ReadOnly) ? QString::fromUtf8(cf.readAll()) : QString());
    Loaded = true;
    ValidateConfig();
    UpdatePreview();
}

void CompareSetupPanel::SaveScript()
{
    QFile f(H->CompareScriptPath());
    QDir().mkpath(QFileInfo(f.fileName()).absolutePath());
    if (!f.open(QIODevice::WriteOnly) || f.write(Script->toPlainText().toUtf8()) < 0)
    {
        W->ShowError("Compare setup", "cannot write " + f.fileName());
        return;
    }
    f.close();
    Load();
    emit setupChanged();
}

void CompareSetupPanel::ResetScript()
{
    if (QMessageBox::question(this, "Reset to template",
            "Replace the project's compare script with the template this build ships? Your edits are lost.") != QMessageBox::Yes)
        return;
    Script->setPlainText(QString::fromUtf8(WBHost::CompareScriptTemplate()));
    SaveScript();
}

void CompareSetupPanel::ShowChanges()
{
    auto diff = WBCmp::LineDiff(QString::fromUtf8(WBHost::CompareScriptTemplate()), Script->toPlainText());
    QString html = "<pre style='font-family:Consolas'>";
    int changed = 0, lastShown = -10;
    for (int i = 0; i < (int)diff.size(); i++)
    {
        bool near = false;
        for (int j = std::max(0, i - 3); j < std::min<int>((int)diff.size(), i + 4); j++)
            if (diff[j].first != ' ') { near = true; break; }
        if (!near) continue;
        if (i != lastShown + 1) html += "<span style='color:%MUTED%'>...</span>\n";
        lastShown = i;
        QString line = diff[i].second.toHtmlEscaped();
        if (diff[i].first == '+') { html += "<span style='color:%GREEN%'>+ " + line + "</span>\n"; changed++; }
        else if (diff[i].first == '-') { html += "<span style='color:%RED%'>- " + line + "</span>\n"; changed++; }
        else html += "  " + line + "\n";
    }
    html += "</pre>";
    if (!changed) html = "The script is the same as the template.";
    QDialog d(this);
    d.setWindowTitle("Changes from the template");
    QTextBrowser* t = new QTextBrowser(&d);
    t->setHtml(WBColor::ApplyHtmlColors(html));
    t->setMinimumSize(760, 480);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    QVBoxLayout* v = new QVBoxLayout(&d);
    v->addWidget(t);
    v->addWidget(bb);
    d.exec();
}

void CompareSetupPanel::ValidateConfig()
{
    WBCmp::CompareConfig cc;
    QString err;
    if (Config->toPlainText().trimmed().isEmpty()) ConfigState->setText("Empty: Compare has no compiler settings.");
    else if (cc.Parse(Config->toPlainText().toUtf8(), err))
    {
        QStringList labels;
        if (!cc.Default.Compiler.isEmpty()) labels << cc.Default.Compiler;
        for (auto& r : cc.Rules) if (!labels.contains(r.Compiler) && !r.Compiler.isEmpty()) labels << r.Compiler;
        QStringList missing;
        for (const QString& l : labels) if (H->CompilerPath(l).isEmpty()) missing << l;
        QString s = QString("Valid: %1 rule%2.").arg(cc.Rules.size()).arg(cc.Rules.size() == 1 ? "" : "s");
        if (!missing.isEmpty()) s += " No executable on this computer yet for: " + missing.join(", ") + " (add it below).";
        ConfigState->setText(s.toHtmlEscaped());
    }
    else ConfigState->setText(WBColor::ApplyHtmlColors("<span style='color:%RED%'>" + err.toHtmlEscaped() + "</span>"));
}

void CompareSetupPanel::SaveConfig()
{
    WBCmp::CompareConfig cc;
    QString err;
    if (!cc.Parse(Config->toPlainText().toUtf8(), err)) { W->ShowError("compare.json", err); return; }
    QFile f(H->CompareConfigPath());
    if (!f.open(QIODevice::WriteOnly) || f.write(Config->toPlainText().toUtf8()) < 0)
    {
        W->ShowError("compare.json", "cannot write " + f.fileName());
        return;
    }
    f.close();
    ValidateConfig();
    UpdatePreview();
    emit setupChanged();
}

void CompareSetupPanel::SaveLocal()
{
    WBSettings& c = H->Settings();
    c.PythonPath = Python->text().trimmed();
    c.ObjdiffPath = Objdiff->text().trimmed();
    c.CompareTimeoutSec = Timeout->value();
    c.Compilers.clear();
    for (int r = 0; r < Compilers->rowCount(); r++)
    {
        QString label = Compilers->item(r, 0) ? Compilers->item(r, 0)->text().trimmed() : QString();
        QString path = Compilers->item(r, 1) ? Compilers->item(r, 1)->text().trimmed() : QString();
        if (!label.isEmpty() && !path.isEmpty()) c.Compilers << label + "=" + path;
    }
    H->SaveSettings();
    ValidateConfig();
    UpdatePreview();
    emit setupChanged();
}

void CompareSetupPanel::SaveCommand()
{
    H->Settings().CompareCommand = Command->text().trimmed();
    H->SaveSettings();
    UpdatePreview();
    emit setupChanged();
}

void CompareSetupPanel::UpdatePreview()
{
    if (!FuncId)
    {
        Preview->setPlainText("(no function at the cursor: move the cursor into one to see its command)");
        RunBtn->setEnabled(false);
        return;
    }
    // the command as typed, saved or not
    WBSettings& c = H->Settings();
    QString saved = c.CompareCommand;
    c.CompareCommand = Command->text().trimmed();
    WBHost::CompareJob job;
    QString err;
    bool ok = H->PrepareCompare(FuncId, job, err, false);
    c.CompareCommand = saved;
    Preview->setPlainText(ok ? job.Command : err);
    RunBtn->setEnabled(ok && W->KB().IsOpen());
}

void CompareSetupPanel::Run(bool selfTest)
{
    if (!Loaded) Load();
    if (Command->text().trimmed() != H->Settings().CompareCommand)
    {
        if (QMessageBox::question(this, "Compare setup", "Save the edited command first?") != QMessageBox::Yes) return;
        SaveCommand();
    }
    if (Script->document()->isModified())
    {
        if (QMessageBox::question(this, "Compare setup", "Save the edited script first?") != QMessageBox::Yes) return;
        SaveScript();
    }
    WBHost::CompareJob job;
    QString err;
    if (!H->EnsureCompareProject(err) || !(selfTest ? H->PrepareSelfTest(FuncId, job, err) : H->PrepareCompare(FuncId, job, err)))
    {
        OutResult->setHtml(WBColor::ApplyHtmlColors("<span style='color:%RED%'>" + err.toHtmlEscaped() + "</span>"));
        return;
    }
    if (!ConfirmCompareCommand(this, H, job)) return;
    RunBtn->setEnabled(false);
    SelfTestBtn->setEnabled(false);
    OutResult->setHtml(QString("Running%1...").arg(selfTest ? " the self-test" : ""));
    OutStdout->clear();
    OutStderr->clear();
    QPointer<CompareSetupPanel> self(this);
    H->RunCompare(job, !selfTest && Record->isChecked(), 0, QString(), [self, selfTest](const WBHost::CompareRun& r)
    {
        if (!self) return;
        self->RunBtn->setEnabled(true);
        self->SelfTestBtn->setEnabled(true);
        self->ShowRun(r, selfTest);
        emit self->setupChanged();
    });
}

void CompareSetupPanel::ShowRun(const WBHost::CompareRun& r, bool selfTest)
{
    OutStdout->setPlainText(r.Stdout);
    OutStderr->setPlainText(r.Stderr);
    const QJsonObject& o = r.Result;
    QString h;
    double score = o.value("score").toDouble();
    bool good = r.Ok && r.Error.isEmpty() && (!selfTest || score >= 100.0);
    h += QString("<p><b style='color:%1'>%2</b></p>").arg(good ? "%GREEN%" : "%RED%",
        selfTest ? (good ? "Self-test passed" : "Self-test failed") : (r.Ok ? QString("Score %1%%2").arg(score, 0, 'f', 1).arg(o.value("matched").toBool() ? ", matched" : "") : "Compare failed"));
    if (!r.Explanation.isEmpty()) h += "<p>" + r.Explanation.toHtmlEscaped() + "</p>";
    if (!r.Error.isEmpty()) h += "<p style='color:%RED%'>" + r.Error.toHtmlEscaped().replace("\n", "<br>") + "</p>";
    if (!r.ContractErrors.isEmpty())
    {
        h += "<p><b>Contract errors</b></p><ul>";
        for (const QString& e : r.ContractErrors) h += "<li style='color:%RED%'>" + e.toHtmlEscaped() + "</li>";
        h += "</ul>";
    }
    h += "<table>";
    auto row = [&h](const QString& k, const QString& v) { h += "<tr><td style='color:%MUTED%'>" + k + "&nbsp;</td><td>" + v.toHtmlEscaped() + "</td></tr>"; };
    row("exit code", r.TimedOut ? QString("(stopped: timed out)") : QString::number(r.ExitCode));
    if (o.contains("stage")) row("stage", o.value("stage").toString());
    if (o.contains("engine")) row("engine", o.value("engine").toString());
    if (o.contains("compiler")) row("compiler", o.value("compiler").toString());
    if (o.contains("flags")) row("flags", o.value("flags").toString());
    if (o.contains("diff")) row("diff rows", QString::number(o.value("diff").toArray().size()));
    if (!selfTest) row("recorded", r.Recorded ? "yes" : "no (a dry run)");
    h += "</table>";
    if (o.contains("log") && !o.value("log").toString().isEmpty())
        h += "<p><b>log</b></p><pre>" + o.value("log").toString().toHtmlEscaped() + "</pre>";
    if (o.contains("diff") && o.value("diff").isArray())
    {
        h += "<p><b>diff</b></p><table cellspacing=0 style='font-family:Consolas'>";
        for (const QJsonValue& v : o.value("diff").toArray())
        {
            QJsonObject d = v.toObject();
            QString st = d.value("status").toString();
            QString col = st == "match" ? "%TEXT%" : st == "reloc" ? "%AMBER%" : "%RED%";
            h += QString("<tr style='color:%1'><td>%2</td><td>&nbsp;%3</td><td>&nbsp;%4</td></tr>")
                     .arg(col, d.value("orig").toString().toHtmlEscaped(), d.value("compiled").toString().toHtmlEscaped(), st.toHtmlEscaped());
        }
        h += "</table>";
    }
    OutResult->setHtml(WBColor::ApplyHtmlColors(h));
}
