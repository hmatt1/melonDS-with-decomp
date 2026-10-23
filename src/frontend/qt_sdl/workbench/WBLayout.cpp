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

// The window's layout: the panel table (groups, descriptions, default places), the View menu, the
// Panels palette, and workspaces (named layouts the user can switch between, save, share and reset).

#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <DockManager.h>
#include <DockWidget.h>
#include <DockWidgetTab.h>
#include <DockAreaWidget.h>
#include <FloatingDockContainer.h>

#include "WorkbenchWindow.h"
#include "WBHost.h"
#include "Config.h"
#include <QHBoxLayout>
#include <QPushButton>

namespace
{
struct PanelInfo
{
    const char* Name;          // the dock's object name
    const char* Group;         // View menu group
    const char* Description;   // one line: tab tooltip, Panels palette
    const char* Anchor;        // its default place: tabbed with this panel
};

const PanelInfo PanelTable[] = {
    {"dock_disasm", "Code", "Code at the cursor in its recorded ARM or Thumb mode, with execution counts", "dock_disasm"},
    {"dock_decompiler", "Code", "Decompiler output stored for the function at the cursor", "dock_callstack"},
    {"dock_source", "Code", "The function's source file, and how it compares with the original", "dock_callstack"},
    {"dock_compare_setup", "Code", "The compare script, compiler settings and a test run (Build > Compare setup)", "dock_callstack"},
    {"dock_functions", "Code", "Functions in the knowledge store, and executed code outside them", "dock_functions"},
    {"dock_xrefs", "Code", "Code and data that refer to an address", "dock_callstack"},
    {"dock_callgraph", "Code", "Callers and callees of the function at the cursor (needs Recording or saved sessions)", "dock_callstack"},
    {"dock_registers", "Runtime", "ARM9 and ARM7 registers, with annotations", "dock_registers"},
    {"dock_callstack", "Runtime", "Calls in progress, from the recorded call stack (needs Recording)", "dock_callstack"},
    {"dock_breakpoints", "Runtime", "Breakpoints and watchpoints, and their hits", "dock_callstack"},
    {"dock_memory", "Runtime", "Bytes at the cursor, coloured by how they were accessed", "dock_memory"},
    {"dock_inspector", "Runtime", "The value at the cursor in every type, its accesses and its origin", "dock_registers"},
    {"dock_history", "Tracing", "Every recorded write to the selection: frame, code, old and new value (needs Write history)", "dock_callstack"},
    {"dock_origin", "Tracing", "Where the selected bytes came from: ROM file, card read, unpacking or code (needs Data origin)", "dock_callstack"},
    {"dock_coverage", "Tracing", "Which bytes were executed, read, written or copied by DMA (needs Recording)", "dock_callstack"},
    {"dock_recording", "Tracing", "What is recorded: write history filters and capacity, data origin sources", "dock_callstack"},
    {"dock_bookmarks", "Project", "Saved addresses", "dock_functions"},
    {"dock_annotations", "Project", "Comments, tags and register notes by people and agents", "dock_registers"},
    {"dock_overlays", "Project", "The ROM's overlays, and when each was loaded", "dock_callstack"},
    {"dock_archive", "Project", "Files inside the game archive (TrackMania's vfs.zip), and where they are in RAM", "dock_callstack"},
    {"dock_containers", "Project", "Files inside NARC and SDAT containers (nested and packed ones too), and where they are in RAM", "dock_callstack"},
    {"dock_graphics", "Project", "Palettes, tiles, maps, cells and textures of the game's graphics files, in the ROM or in RAM", "dock_callstack"},
    {"dock_search", "Project", "Search memory for bytes, strings or changing values", "dock_callstack"},
    {"dock_game", "Emulator", "The game itself, inside the workbench (View > Game screen in the workbench)", "dock_disasm"},
    {"dock_start", "Project", "What Recording, Write history and Data origin give you, and whether they are on", "dock_callstack"},
};

const PanelInfo* InfoFor(const QString& name)
{
    for (const PanelInfo& p : PanelTable)
        if (name == p.Name) return &p;
    return nullptr;
}

} // anonymous namespace, reopened below for the rest of the file's helpers

bool WorkbenchWindow::ResolvePanel(const QString& name, QString& dock, QString& err)
{
    QString n = name.trimmed().toLower();
    if (n.isEmpty()) { dock.clear(); return true; }
    n.remove(' ');
    n.remove('_');
    n.remove('-');
    if (n.startsWith("dock")) n = n.mid(4);
    // short names and the panel titles, after spaces and underscores are dropped
    struct Alias { const char* Key; const char* Dock; };
    static const Alias aliases[] = {
        {"disasm", "dock_disasm"}, {"disassembly", "dock_disasm"},
        {"memory", "dock_memory"},
        {"functions", "dock_functions"},
        {"bookmarks", "dock_bookmarks"},
        {"registers", "dock_registers"},
        {"inspector", "dock_inspector"},
        {"annotations", "dock_annotations"},
        {"callstack", "dock_callstack"}, {"callgraph", "dock_callgraph"},
        {"xrefs", "dock_xrefs"}, {"crossreference", "dock_xrefs"}, {"crossreferences", "dock_xrefs"},
        {"coverage", "dock_coverage"}, {"coveragemap", "dock_coverage"},
        {"history", "dock_history"}, {"writehistory", "dock_history"}, {"writes", "dock_history"},
        {"origin", "dock_origin"}, {"dataorigin", "dock_origin"},
        {"overlay", "dock_overlays"}, {"overlays", "dock_overlays"},
        {"source", "dock_source"}, {"diff", "dock_source"}, {"sourceanddiff", "dock_source"},
        {"comparesetup", "dock_compare_setup"}, {"compare", "dock_compare_setup"},
        {"decompiler", "dock_decompiler"},
        {"search", "dock_search"},
        {"graphics", "dock_graphics"}, {"gfx", "dock_graphics"}, {"textures", "dock_graphics"},
        {"archive", "dock_archive"},
        {"containers", "dock_containers"}, {"narc", "dock_containers"}, {"sdat", "dock_containers"},
        {"breakpoint", "dock_breakpoints"}, {"breakpoints", "dock_breakpoints"},
        {"recording", "dock_recording"},
        {"start", "dock_start"}, {"gettingstarted", "dock_start"},
        {"game", "dock_game"}, {"gamescreen", "dock_game"},
    };
    for (const Alias& a : aliases)
        if (n == a.Key) { dock = QString::fromLatin1(a.Dock); return true; }
    err = "unknown panel '" + name.trimmed() + "'";
    return false;
}

namespace
{

struct BuiltIn
{
    const char* Name;
    std::vector<const char*> Panels;    // empty: every panel
};

const BuiltIn BuiltIns[] = {
    {"Default", {}},
    {"Decomp", {"dock_functions", "dock_disasm", "dock_decompiler", "dock_source", "dock_xrefs", "dock_inspector"}},
    {"Debug", {"dock_game", "dock_disasm", "dock_registers", "dock_callstack", "dock_breakpoints", "dock_memory"}},
    {"Data tracing", {"dock_game", "dock_memory", "dock_history", "dock_origin", "dock_archive", "dock_containers", "dock_inspector", "dock_graphics"}},
    {"Coverage", {"dock_game", "dock_coverage", "dock_functions", "dock_overlays", "dock_disasm"}},
    {"Minimal", {"dock_game", "dock_disasm"}},
};

// type a few letters of a panel's name or description, Enter opens it
class PanelPalette : public QDialog
{
public:
    PanelPalette(WorkbenchWindow* w, const std::vector<ads::CDockWidget*>& docks) : QDialog(w), W(w)
    {
        setWindowTitle("Panels");
        setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
        resize(560, 380);
        auto* lay = new QVBoxLayout(this);
        Filter = new QLineEdit(this);
        Filter->setPlaceholderText("Panel name or what it shows");
        List = new QListWidget(this);
        lay->addWidget(Filter);
        lay->addWidget(List);
        for (ads::CDockWidget* d : docks)
        {
            const PanelInfo* pi = InfoFor(d->objectName());
            auto* it = new QListWidgetItem(QString("%1  -  %2").arg(d->windowTitle(), pi ? pi->Description : ""), List);
            it->setData(Qt::UserRole, d->objectName());
            it->setData(Qt::UserRole + 1, (d->windowTitle() + " " + (pi ? pi->Description : "") + " " + (pi ? pi->Group : "")).toLower());
        }
        List->setCurrentRow(0);
        connect(Filter, &QLineEdit::textChanged, this, [this](const QString& t)
        {
            QStringList words = t.toLower().split(' ', Qt::SkipEmptyParts);
            int first = -1;
            for (int i = 0; i < List->count(); i++)
            {
                QString hay = List->item(i)->data(Qt::UserRole + 1).toString();
                bool show = true;
                for (const QString& w : words) if (!hay.contains(w)) { show = false; break; }
                List->item(i)->setHidden(!show);
                if (show && first < 0) first = i;
            }
            if (first >= 0) List->setCurrentRow(first);
        });
        connect(List, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { Open(); });
        Filter->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == Filter && e->type() == QEvent::KeyPress)
        {
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up)
            {
                int step = k->key() == Qt::Key_Down ? 1 : -1;
                for (int r = List->currentRow() + step; r >= 0 && r < List->count(); r += step)
                    if (!List->item(r)->isHidden()) { List->setCurrentRow(r); break; }
                return true;
            }
            if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) { Open(); return true; }
        }
        return QDialog::eventFilter(o, e);
    }

private:
    void Open()
    {
        QListWidgetItem* it = List->currentItem();
        if (!it || it->isHidden()) return;
        QString name = it->data(Qt::UserRole).toString();
        accept();
        W->ShowPanel(name);
    }
    WorkbenchWindow* W;
    QLineEdit* Filter;
    QListWidget* List;
};
}

// ---------------------------------------------------------------- prerequisite banners

NeedBanner::NeedBanner(WBHost* h, QWidget* parent) : QFrame(parent), H(h)
{
    setObjectName("needBanner");
    setStyleSheet("#needBanner { background: palette(alternate-base); border: 1px solid palette(mid); border-radius: 3px; }");
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 4, 8, 4);
    Text = new QLabel(this);
    Text->setWordWrap(true);
    Text->setTextFormat(Qt::RichText);
    Fix = new QPushButton(this);
    lay->addWidget(Text, 1);
    lay->addWidget(Fix);
    connect(Fix, &QPushButton::clicked, this, [this]()
    {
        QString err;
        switch ((WBNeed)FixKind)
        {
        case WBNeed::Interpreter:
            if (QMessageBox::question(window(), "Turn off the JIT", "Turn the JIT recompiler off and reset the game?\n\n" + WBHost::Covers(WBNeed::Interpreter)) != QMessageBox::Yes) return;
            if (!H->TurnOffJitAndReset(err)) QMessageBox::warning(window(), "Turn off the JIT", err);
            break;
        case WBNeed::Recording: H->SetRecording(true); break;
        case WBNeed::WriteHistory: H->SetWriteHistory(true); break;
        case WBNeed::DataOrigin: H->SetDataOrigin(true); break;
        default: break;
        }
    });
    setVisible(false);
}

void NeedBanner::Update(WBPanel* p)
{
    WBNeed need;
    QString what;
    QString missing = p->Need(need, what) ? H->Missing(need) : QString();
    if (missing.isEmpty()) { setVisible(false); return; }
    // the fix: whatever is missing first (the JIT, then the feature itself)
    WBNeed fix = need;
    QString button;
    bool rom = need == WBNeed::Archive || need == WBNeed::Containers;     // needs only the ROM
    if (!H->Missing(WBNeed::Interpreter).isEmpty() && !rom) { fix = WBNeed::Interpreter; button = "Turn off the JIT..."; }
    else if (!H->Missing(WBNeed::Game).isEmpty() && !rom) button.clear();
    else if (need == WBNeed::Recording) button = "Start recording";
    else if (need == WBNeed::WriteHistory) button = "Turn on write history";
    else if (need == WBNeed::DataOrigin) button = "Turn on data origin";
    QString feature = need == WBNeed::Recording ? "Recording" : need == WBNeed::WriteHistory ? "Write history"
                      : need == WBNeed::DataOrigin ? "Data origin" : need == WBNeed::Interpreter ? "the interpreter" : need == WBNeed::Containers ? "NARC or SDAT files" : "a game archive";
    QString covers = WBHost::Covers(fix == WBNeed::Interpreter ? WBNeed::Interpreter : need);
    Text->setText(QString("<b>Needs %2:</b> %1. %3%4").arg(what.toHtmlEscaped(), feature, missing.toHtmlEscaped(),
                  covers.isEmpty() ? QString() : "<br><span style='color: palette(mid)'>" + covers.toHtmlEscaped() + "</span>"));
    Fix->setText(button);
    Fix->setVisible(!button.isEmpty());
    FixKind = (int)fix;
    setVisible(true);
}

void WorkbenchWindow::UpdateBanners()
{
    for (auto& b : Banners)
        if (b.first->isVisible() || b.second->isVisible()) b.second->Update(b.first);
}

void WorkbenchWindow::DescribePanels()
{
    for (ads::CDockWidget* d : Docks)
        if (const PanelInfo* pi = InfoFor(d->objectName()))
        {
            d->setTabToolTip(QString("%1: %2").arg(d->windowTitle(), pi->Description));
            if (d->widget()) d->widget()->setToolTip(QString());
        }
}

void WorkbenchWindow::BuildViewMenu(QMenu* view)
{
    // panels by what they are for; each item shows or hides one, and its tooltip says what it shows
    for (const char* group : {"Code", "Runtime", "Tracing", "Project", "Emulator"})
    {
        view->addSection(group);
        for (const PanelInfo& pi : PanelTable)
        {
            if (strcmp(pi.Group, group)) continue;
            for (ads::CDockWidget* d : Docks)
                if (d->objectName() == pi.Name)
                {
                    QAction* a = d->toggleViewAction();
                    a->setToolTip(pi.Description);
                    a->setStatusTip(pi.Description);
                    view->addAction(a);
                }
        }
    }
    view->setToolTipsVisible(true);
}

void WorkbenchWindow::ShowPanelPalette()
{
    PanelPalette pal(this, Docks);
    pal.exec();
}

// ---------------------------------------------------------------- restoring a layout

bool WorkbenchWindow::RestoreLayout(const QByteArray& state)
{
    if (state.isEmpty() || !DockMgr->restoreState(state, LayoutVersion)) return false;

    // panels the saved layout doesn't know (added since) go to their default place
    QSet<QString> known;
    QXmlStreamReader xml(state);
    while (!xml.atEnd())
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("Widget"))
            known.insert(xml.attributes().value("Name").toString());
    for (ads::CDockWidget* d : Docks)
    {
        if (known.contains(d->objectName())) continue;
        const PanelInfo* pi = InfoFor(d->objectName());
        ads::CDockWidget* anchor = pi ? DockMgr->findDockWidget(pi->Anchor) : nullptr;
        if (anchor && anchor != d && !anchor->isClosed() && anchor->dockAreaWidget())
            DockMgr->addDockWidgetTabToArea(d, anchor->dockAreaWidget());
        else
            DockMgr->addDockWidget(ads::BottomDockWidgetArea, d);
        d->toggleView(true);
    }

    // floating panels left on a monitor that is gone come back to this window's screen
    QScreen* home = screen();
    for (ads::CFloatingDockContainer* f : DockMgr->floatingWidgets())
    {
        QRect g = f->geometry();
        if (QGuiApplication::screenAt(g.center()) || !home) continue;
        QRect avail = home->availableGeometry();
        g.setSize(g.size().boundedTo(avail.size() * 0.8));
        g.moveCenter(avail.center());
        f->setGeometry(g);
    }
    return true;
}

// ---------------------------------------------------------------- workspaces

void WorkbenchWindow::InitWorkspaces()
{
    // built-in workspaces: the default layout with only their panels shown
    for (const BuiltIn& b : BuiltIns)
    {
        DockMgr->restoreState(DefaultState, LayoutVersion);
        if (!b.Panels.empty())
            for (ads::CDockWidget* d : Docks)
            {
                bool want = false;
                for (const char* n : b.Panels) if (d->objectName() == n) want = true;
                d->toggleView(want);
            }
        Workspace w;
        w.Name = b.Name;
        w.BuiltIn = true;
        w.Default = DockMgr->saveState(LayoutVersion);
        w.State = w.Default;
        Workspaces.push_back(w);
    }

    // the user's: changed built-ins and their own, from the config
    Config::Table cfg = Config::GetGlobalTable();
    QJsonArray saved = QJsonDocument::fromJson(cfg.GetQString("Workbench.Workspaces").toUtf8()).array();
    for (const QJsonValue& v : saved) MergeWorkspace(v.toObject(), false);
    // and a project's own, shared with the project (workspaces.json in the project folder)
    QFile pf(H->ProjectDir() + "/workspaces.json");
    if (pf.open(QIODevice::ReadOnly))
        for (const QJsonValue& v : QJsonDocument::fromJson(pf.readAll()).array()) MergeWorkspace(v.toObject(), true);

    CurWorkspace = 0;
    QString cur = cfg.GetQString("Workbench.Workspace");
    for (size_t i = 0; i < Workspaces.size(); i++)
        if (Workspaces[i].Name == cur) CurWorkspace = (int)i;
    // a layout saved before workspaces existed belongs to the default workspace
    QByteArray legacy = QByteArray::fromBase64(cfg.GetQString("Workbench.DockState").toLatin1());
    if (cur.isEmpty() && !legacy.isEmpty()) Workspaces[0].State = legacy;
    if (!RestoreLayout(Workspaces[CurWorkspace].State)) RestoreLayout(Workspaces[CurWorkspace].Default);
}

void WorkbenchWindow::MergeWorkspace(const QJsonObject& o, bool project)
{
    Workspace w;
    w.Name = o.value("name").toString();
    w.State = QByteArray::fromBase64(o.value("state").toString().toLatin1());
    w.Default = QByteArray::fromBase64(o.value("default").toString().toLatin1());
    w.Geometry = QByteArray::fromBase64(o.value("geometry").toString().toLatin1());
    w.Screen = o.value("game_screen").toInt(-1);
    if (w.Name.isEmpty() || w.State.isEmpty()) return;
    for (Workspace& x : Workspaces)
        if (x.Name == w.Name)
        {
            // a built-in keeps its own default; a project's file doesn't override the user's changes
            if (!project) { x.State = w.State; x.Geometry = w.Geometry; x.Screen = w.Screen; }
            if (!x.BuiltIn && !w.Default.isEmpty()) x.Default = w.Default;
            return;
        }
    if (w.Default.isEmpty()) w.Default = w.State;
    Workspaces.push_back(w);
}

void WorkbenchWindow::StoreWorkspaces()
{
    QJsonArray arr;
    for (const Workspace& w : Workspaces)
    {
        if (w.BuiltIn && w.State == w.Default && w.Screen < 0 && w.Geometry.isEmpty()) continue;    // unused built-ins are rebuilt at start
        QJsonObject o;
        o["name"] = w.Name;
        o["state"] = QString::fromLatin1(w.State.toBase64());
        if (!w.Geometry.isEmpty()) o["geometry"] = QString::fromLatin1(w.Geometry.toBase64());
        if (w.Screen >= 0) o["game_screen"] = w.Screen;
        if (!w.BuiltIn) o["default"] = QString::fromLatin1(w.Default.toBase64());
        arr.append(o);
    }
    Config::Table cfg = Config::GetGlobalTable();
    cfg.SetQString("Workbench.Workspaces", QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    cfg.SetQString("Workbench.Workspace", Workspaces[CurWorkspace].Name);
}

int WorkbenchWindow::ScreenMode() const
{
    if (!ActEmbed || !ActEmbed->isChecked()) return 0;
    return ActHideMain && ActHideMain->isChecked() ? 2 : 1;
}

void WorkbenchWindow::CaptureWorkspace(Workspace& w)
{
    w.State = DockMgr->saveState(LayoutVersion);
    w.Geometry = saveGeometry();
    w.Screen = ScreenMode();
}

void WorkbenchWindow::ApplyWorkspace(int idx, bool layout)
{
    Workspace& w = Workspaces[idx];
    CurWorkspace = idx;
    if (layout && !RestoreLayout(w.State)) RestoreLayout(w.Default);
    if (!w.Geometry.isEmpty() && !isFullScreen()) restoreGeometry(w.Geometry);
    // the game screen mode, through the menu actions so the config and the menus follow
    if (w.Screen >= 0 && ActEmbed && ActHideMain)
    {
        if (ActEmbed->isChecked() != (w.Screen > 0)) ActEmbed->trigger();
        if (w.Screen > 0 && ActEmbed->isChecked() && ActHideMain->isEnabled() && ActHideMain->isChecked() != (w.Screen == 2))
            ActHideMain->trigger();
    }
}

void WorkbenchWindow::SwitchWorkspace(int idx)
{
    if (idx < 0 || idx >= (int)Workspaces.size()) return;
    if (idx != CurWorkspace) CaptureWorkspace(Workspaces[CurWorkspace]);
    ApplyWorkspace(idx, true);
    SaveLayout();
    RebuildWorkspaceMenu();
    Notice("Workspace: " + Workspaces[idx].Name, 3000);
}

void WorkbenchWindow::RebuildWorkspaceMenu()
{
    if (!WorkspaceMenu || Workspaces.empty()) return;
    WorkspaceMenu->clear();
    for (size_t i = 0; i < Workspaces.size(); i++)
    {
        QAction* a = WorkspaceMenu->addAction(Workspaces[i].Name, this, [this, i]() { SwitchWorkspace((int)i); });
        a->setCheckable(true);
        a->setChecked((int)i == CurWorkspace);
        if (i < 9) a->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | (Qt::Key_1 + (int)i)));
        a->setShortcutContext(Qt::WindowShortcut);
    }
    WorkspaceMenu->addSeparator();
    WorkspaceMenu->addAction("Save current as...", this, [this]()
    {
        bool ok;
        QString name = QInputDialog::getText(this, "Save workspace", "Name:", QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        CaptureWorkspace(Workspaces[CurWorkspace]);
        int idx = -1;
        for (size_t i = 0; i < Workspaces.size(); i++) if (Workspaces[i].Name == name) idx = (int)i;
        if (idx >= 0 && Workspaces[idx].BuiltIn) { ShowError("Save workspace", "\"" + name + "\" is a built-in workspace: pick another name"); return; }
        if (idx < 0) { Workspaces.push_back(Workspace{name, {}, {}, false}); idx = (int)Workspaces.size() - 1; }
        CaptureWorkspace(Workspaces[idx]);
        Workspaces[idx].Default = Workspaces[idx].State;
        CurWorkspace = idx;
        SaveLayout();
        RebuildWorkspaceMenu();
    });
    QAction* a = WorkspaceMenu->addAction("Rename...", this, [this]()
    {
        bool ok;
        QString name = QInputDialog::getText(this, "Rename workspace", "Name:", QLineEdit::Normal, Workspaces[CurWorkspace].Name, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        for (const Workspace& w : Workspaces) if (w.Name == name) { ShowError("Rename workspace", "There is already a workspace called \"" + name + "\""); return; }
        Workspaces[CurWorkspace].Name = name;
        SaveLayout();
        RebuildWorkspaceMenu();
    });
    a->setEnabled(!Workspaces[CurWorkspace].BuiltIn);
    a = WorkspaceMenu->addAction("Delete", this, [this]()
    {
        if (QMessageBox::question(this, "Delete workspace", "Delete the workspace \"" + Workspaces[CurWorkspace].Name + "\"?") != QMessageBox::Yes) return;
        Workspaces.erase(Workspaces.begin() + CurWorkspace);
        ApplyWorkspace(0, true);
        SaveLayout();
        RebuildWorkspaceMenu();
    });
    a->setEnabled(!Workspaces[CurWorkspace].BuiltIn);
    WorkspaceMenu->addAction("Reset to default", this, [this]()
    {
        RestoreLayout(Workspaces[CurWorkspace].Default);
        SaveLayout();
    });
    WorkspaceMenu->addSeparator();
    WorkspaceMenu->addAction("Export...", this, [this]()
    {
        QString p = QFileDialog::getSaveFileName(this, "Export workspace", H->ProjectDir() + "/" + Workspaces[CurWorkspace].Name + ".json", "Workspace (*.json)");
        if (p.isEmpty()) return;
        QJsonObject o;
        o["name"] = Workspaces[CurWorkspace].Name;
        o["state"] = QString::fromLatin1(DockMgr->saveState(LayoutVersion).toBase64());
        o["game_screen"] = ScreenMode();      // not the window geometry: it belongs to this computer's screens
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(QJsonArray{o}).toJson()) < 0) ShowError("Export workspace", f.errorString());
    });
    WorkspaceMenu->addAction("Import...", this, [this]()
    {
        QString p = QFileDialog::getOpenFileName(this, "Import workspace", H->ProjectDir(), "Workspace (*.json)");
        if (p.isEmpty()) return;
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) { ShowError("Import workspace", f.errorString()); return; }
        QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
        if (arr.isEmpty()) { ShowError("Import workspace", "No workspace in " + p); return; }
        CaptureWorkspace(Workspaces[CurWorkspace]);
        size_t before = Workspaces.size();
        QString first = arr[0].toObject().value("name").toString();
        for (const QJsonValue& v : arr) MergeWorkspace(v.toObject(), false);
        for (size_t i = 0; i < Workspaces.size(); i++)
            if (Workspaces[i].Name == first) { SwitchWorkspace((int)i); break; }
        Notice(QString("Imported %1 workspace(s)").arg(std::max<size_t>(Workspaces.size() - before, 1)), 3000);
    });
}
