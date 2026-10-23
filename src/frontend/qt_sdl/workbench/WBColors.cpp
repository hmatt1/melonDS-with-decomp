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

#include <QFile>

#include "WBColors.h"
#include "Platform.h"

namespace WBColor
{

struct Palette
{
    QColor Text, Muted, Green, Amber, Red, Blue, Teal;
    QColor Off, Select, Unowned, ChipRec, ChipOrigin;
    QColor Window, Base, Coverage, Border, Dark, Hover, TabInactive;
    QColor SelectionFill, Touched, Overlay, Heat, Field, Decode, FunctionBar, FunctionEnd;
};

struct Bind
{
    const char* Key;
    QColor Palette::* Mem;
    const char* Hex;
    const char* Section;
    const char* Comment;
};

static const Bind kBinds[] = {
    // Dracula: https://draculatheme.com  (background, current line, foreground, comment, and the accent set)
    {"window", &Palette::Window, "#282a36", "Surfaces", "window background"},
    {"base", &Palette::Base, "#282a36", nullptr, "editors, lists, disassembly, memory"},
    {"coverage", &Palette::Coverage, "#191a21", nullptr, "coverage map background"},
    {"off", &Palette::Off, "#21222c", nullptr, "raised surfaces, inactive tabs, switches that are off"},
    {"border", &Palette::Border, "#44475a", nullptr, "borders and separators"},
    {"dark", &Palette::Dark, "#191a21", nullptr, "address ruler and shadows"},
    {"hover", &Palette::Hover, "#6272a4", nullptr, "button, menu and row hover"},

    {"text", &Palette::Text, "#f8f8f2", "Text", "primary text"},
    {"muted", &Palette::Muted, "#6272a4", nullptr, "addresses and secondary text"},
    {"tab_inactive", &Palette::TabInactive, "#6272a4", nullptr, "dock tab that is not selected"},

    {"green", &Palette::Green, "#50fa7b", "Meaning", "a compare that matched"},
    {"amber", &Palette::Amber, "#ffb86c", nullptr, "partial, or executed code with no function"},
    {"red", &Palette::Red, "#ff5555", nullptr, "breakpoint, failed compare, or needs attention"},
    {"blue", &Palette::Blue, "#8be9fd", nullptr, "link, cursor, loaded overlay, clickable target"},
    {"teal", &Palette::Teal, "#bd93f9", nullptr, "data origin, as text"},

    {"select", &Palette::Select, "#44475a", "Fills", "buttons, and the row or cell you are on"},
    {"selection", &Palette::SelectionFill, "#bd93f9", nullptr, "coverage selection"},
    {"unowned", &Palette::Unowned, "#59483c", nullptr, "executed code that belongs to no function"},
    {"chip_rec", &Palette::ChipRec, "#5f3038", nullptr, "recording badge and conflict chip"},
    {"chip_origin", &Palette::ChipOrigin, "#4d4265", nullptr, "origin badge"},
    {"touched", &Palette::Touched, "#3e5c67", nullptr, "a covered cell, or a byte that was accessed"},
    {"overlay", &Palette::Overlay, "#ff79c6", nullptr, "a loaded overlay on the coverage map"},
    {"heat", &Palette::Heat, "#f1fa8c", nullptr, "execution heat bar"},
    {"field", &Palette::Field, "#bd93f9", nullptr, "struct field marker in memory"},
    {"decode", &Palette::Decode, "#8be9fd", nullptr, "how a byte decodes, and function outlines"},
    {"function_bar", &Palette::FunctionBar, "#50fa7b", nullptr, "bar beside a function"},
    {"function_end", &Palette::FunctionEnd, "#6272a4", nullptr, "bar on the last instruction of an open function"},
};

static Palette Defaults()
{
    Palette p;
    for (const Bind& b : kBinds)
        p.*(b.Mem) = QColor(b.Hex);
    return p;
}

static Palette Pal = Defaults();

static QString DefaultText()
{
    QString s;
    s += "# Workbench colors, matched to Dracula (draculatheme.com). One solid hex color per line, RRGGBB.\n";
    s += "# This file sits next to melonDS.toml. Saving it updates the open workbench.\n";
    QString section;
    for (const Bind& b : kBinds)
    {
        if (b.Section && section != QLatin1String(b.Section))
        {
            section = QLatin1String(b.Section);
            s += "\n# ";
            s += b.Section;
            s += "\n";
        }
        s += b.Key;
        s += " = \"";
        s += b.Hex;
        s += "\"";
        if (b.Comment && b.Comment[0])
        {
            s += "  # ";
            s += b.Comment;
        }
        s += "\n";
    }
    return s;
}

static bool ApplyLine(Palette& p, const QString& line)
{
    QString s = line.trimmed();
    if (s.isEmpty() || s.startsWith('#')) return false;
    int eq = s.indexOf('=');
    if (eq < 0) return false;
    QString key = s.left(eq).trimmed();
    QString val = s.mid(eq + 1).trimmed();
    if (val.startsWith('"') || val.startsWith('\''))
    {
        const QChar q = val.at(0);
        int end = val.indexOf(q, 1);
        if (end < 0) return false;
        val = val.mid(1, end - 1).trimmed();
    }
    else
    {
        int cut = val.indexOf(" #");
        if (cut >= 0) val = val.left(cut).trimmed();
    }
    if (!val.startsWith('#')) val.prepend('#');
    QColor c(val);
    if (!c.isValid()) return false;
    c.setAlpha(255);
    for (const Bind& b : kBinds)
        if (key == QLatin1String(b.Key)) { p.*(b.Mem) = c; return true; }
    return false;
}

QString FilePath()
{
    return QString::fromStdString(melonDS::Platform::GetLocalFilePath("workbench-colors.toml"));
}

bool Load()
{
    const QString path = FilePath();
    QFile f(path);
    Palette next = Defaults();
    int applied = 0;
    if (!f.exists())
    {
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            f.write(DefaultText().toUtf8());
            f.close();
        }
    }
    else if (f.open(QIODevice::ReadOnly))
    {
        const QString text = QString::fromUtf8(f.readAll());
        Palette parsed = Defaults();
        for (const QString& line : text.split('\n'))
            if (ApplyLine(parsed, line)) applied++;
        // a half-written save has no keys yet: keep the colors already on screen
        if (applied == 0) return false;
        next = parsed;
    }
    else
        return false;

    bool changed = false;
    for (const Bind& b : kBinds)
    {
        QColor c = next.*(b.Mem);
        c.setAlpha(255);
        if (Pal.*(b.Mem) != c) changed = true;
        Pal.*(b.Mem) = c;
    }
    return changed;
}

QString WidgetStyleSheet()
{
    QString s = QStringLiteral(R"ss(
QPushButton, QToolButton {
    background-color: %SELECT%;
    background-image: none;
    color: %TEXT%;
    border: 1px solid %HOVER%;
    border-radius: 2px;
    padding: 3px 8px;
}
QPushButton:hover, QToolButton:hover,
QPushButton:pressed, QPushButton:checked, QToolButton:pressed, QToolButton:checked {
    background-color: %HOVER%;
    background-image: none;
    color: %TEXT%;
}
QPushButton:disabled, QToolButton:disabled {
    background-color: %OFF%;
    color: %MUTED%;
}
QCheckBox, QRadioButton { color: %TEXT%; }
QCheckBox:disabled, QRadioButton:disabled { color: %MUTED%; }
QComboBox, QLineEdit, QAbstractSpinBox, QTextEdit, QPlainTextEdit {
    background-color: %BASE%;
    background-image: none;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    selection-background-color: %SELECT%;
    selection-color: %TEXT%;
}
QComboBox QAbstractItemView {
    background-color: %BASE%;
    color: %TEXT%;
    selection-background-color: %SELECT%;
    selection-color: %TEXT%;
}
QComboBox::drop-down { background-color: %OFF%; border: none; width: 16px; }
QHeaderView::section {
    background-color: %OFF%;
    background-image: none;
    color: %TEXT%;
    border: none;
    border-right: 1px solid %BORDER%;
    border-bottom: 1px solid %BORDER%;
    padding: 3px;
}
QAbstractItemView {
    background-color: %BASE%;
    background-image: none;
    color: %TEXT%;
    alternate-background-color: %OFF%;
    selection-background-color: %SELECT%;
    selection-color: %TEXT%;
    outline: 0;
}
QAbstractItemView::item:hover { background-color: %HOVER%; color: %TEXT%; }
QAbstractItemView::item:selected, QAbstractItemView::item:selected:hover {
    background-color: %SELECT%;
    color: %TEXT%;
}
QMenuBar { background-color: %WINDOW%; color: %TEXT%; }
QMenuBar::item:selected { background-color: %SELECT%; }
QMenu { background-color: %OFF%; color: %TEXT%; border: 1px solid %BORDER%; }
QMenu::item:selected { background-color: %SELECT%; color: %TEXT%; }
QStatusBar, QToolBar { background-color: %WINDOW%; background-image: none; color: %TEXT%; border: none; }
QToolTip { background-color: %OFF%; color: %TEXT%; border: 1px solid %BORDER%; }
QMainWindow, QDockWidget { background-color: %WINDOW%; background-image: none; }
QScrollBar:vertical { background-color: %BASE%; width: 14px; margin: 0; }
QScrollBar::handle:vertical { background-color: %BORDER%; min-height: 24px; }
QScrollBar::handle:vertical:hover { background-color: %HOVER%; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal { background-color: %BASE%; height: 14px; margin: 0; }
QScrollBar::handle:horizontal { background-color: %BORDER%; min-width: 24px; }
QScrollBar::handle:horizontal:hover { background-color: %HOVER%; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
QSplitter::handle, ads--CDockSplitter::handle { background-color: %BORDER%; background-image: none; }
QTabBar::tab {
    background-color: %OFF%;
    background-image: none;
    color: %TAB%;
    border: 1px solid %BORDER%;
    padding: 4px 8px;
}
QTabBar::tab:selected { background-color: %BASE%; color: %TEXT%; }
QProgressBar { background-color: %BASE%; background-image: none; border: 1px solid %BORDER%; color: %TEXT%; }
QProgressBar::chunk { background-color: %BLUE%; background-image: none; }
QGroupBox { background-color: %WINDOW%; background-image: none; color: %TEXT%; border: 1px solid %BORDER%; margin-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; color: %TEXT%; }
ads--CDockWidgetTab { background-color: %OFF%; background-image: none; border: none; }
ads--CDockWidgetTab[activeTab="true"] { background-color: %BASE%; }
ads--CDockWidgetTab QLabel { color: %TAB%; }
ads--CDockWidgetTab[activeTab="true"] QLabel { color: %TEXT%; }
ads--CDockAreaTitleBar, ads--CAutoHideTab { background-color: %WINDOW%; background-image: none; border: none; }
ads--CDockAreaTitleBar QToolButton { background-color: %WINDOW%; background-image: none; color: %TEXT%; border: none; padding: 0; }
ads--CDockAreaTitleBar QToolButton:hover { background-color: %HOVER%; color: %TEXT%; }
/* The docking library ships black glyphs, and a white copy only for a focused close button. */
ads--CDockWidgetTab > #tabCloseButton,
ads--CDockAreaTitleBar #dockAreaCloseButton,
ads--CAutoHideDockContainer #dockAreaCloseButton,
#floatingTitleCloseButton {
    qproperty-icon: url(:/ads/images/close-button-focused.svg);
    background: none;
    border: none;
    padding: 0;
}
ads--CDockAreaTitleBar #detachGroupButton {
    qproperty-icon: url(:/wb/detach.svg);
    background: none;
    border: none;
    padding: 0;
}
ads--CDockAreaTitleBar #tabsMenuButton {
    qproperty-icon: url(:/wb/tabs-menu.svg);
    background: none;
    border: none;
    padding: 0;
}
#dockAreaAutoHideButton {
    qproperty-icon: url(:/wb/pin.svg);
    background: none;
    border: none;
    padding: 0;
}
#dockAreaMinimizeButton {
    qproperty-icon: url(:/ads/images/minimize-button-focused.svg);
    background: none;
    border: none;
    padding: 0;
}
ads--CDockWidgetTab > #tabCloseButton:hover,
ads--CDockWidgetTab > #tabCloseButton:pressed,
ads--CDockAreaTitleBar #dockAreaCloseButton:hover,
ads--CDockAreaTitleBar #dockAreaCloseButton:pressed,
ads--CAutoHideDockContainer #dockAreaCloseButton:hover,
ads--CAutoHideDockContainer #dockAreaCloseButton:pressed,
#floatingTitleCloseButton:hover,
#floatingTitleCloseButton:pressed,
ads--CDockAreaTitleBar #detachGroupButton:hover,
ads--CDockAreaTitleBar #detachGroupButton:pressed,
ads--CDockAreaTitleBar #tabsMenuButton:hover,
ads--CDockAreaTitleBar #tabsMenuButton:pressed,
#dockAreaAutoHideButton:hover,
#dockAreaAutoHideButton:pressed,
#dockAreaMinimizeButton:hover,
#dockAreaMinimizeButton:pressed {
    background-color: %HOVER%;
    border: none;
}
)ss");
    const auto put = [&](const char* token, const QColor& c) { s.replace(token, c.name()); };
    put("%OFF%", Off());
    put("%TEXT%", Text());
    put("%BORDER%", Border());
    put("%HOVER%", Hover());
    put("%SELECT%", Select());
    put("%MUTED%", Muted());
    put("%BASE%", Base());
    put("%WINDOW%", Window());
    put("%TAB%", TabInactive());
    put("%BLUE%", Blue());
    return s;
}

QColor Text() { return Pal.Text; }
QColor Muted() { return Pal.Muted; }
QColor Green() { return Pal.Green; }
QColor Amber() { return Pal.Amber; }
QColor Red() { return Pal.Red; }
QColor Blue() { return Pal.Blue; }
QColor Teal() { return Pal.Teal; }
QColor Off() { return Pal.Off; }
QColor Select() { return Pal.Select; }
QColor Unowned() { return Pal.Unowned; }
QColor ChipRec() { return Pal.ChipRec; }
QColor ChipOrigin() { return Pal.ChipOrigin; }
QColor Window() { return Pal.Window; }
QColor Base() { return Pal.Base; }
QColor Coverage() { return Pal.Coverage; }
QColor Border() { return Pal.Border; }
QColor Dark() { return Pal.Dark; }
QColor Hover() { return Pal.Hover; }
QColor TabInactive() { return Pal.TabInactive; }
QColor SelectionFill() { return Pal.SelectionFill; }
QColor Touched() { return Pal.Touched; }
QColor Overlay() { return Pal.Overlay; }
QColor Heat() { return Pal.Heat; }
QColor Field() { return Pal.Field; }
QColor Decode() { return Pal.Decode; }
QColor FunctionBar() { return Pal.FunctionBar; }
QColor FunctionEnd() { return Pal.FunctionEnd; }

}
