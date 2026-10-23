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

#ifndef WB_COLORS_H
#define WB_COLORS_H

#include <QColor>
#include <QString>

// Workbench palette, loaded from workbench-colors.toml next to melonDS.toml.
// Every color is opaque. A saturated color has one meaning, and it is drawn on a dark surface.
namespace WBColor
{

QString FilePath();
// Reads the file, creating it from the defaults when it is missing. True when a color changed.
bool Load();
QString WidgetStyleSheet();

QColor Text();
QColor Muted();
QColor Green();    // a compare that matched
QColor Amber();    // partial, or executed code with no function
QColor Red();      // a breakpoint, a failed compare, or anything that needs attention
QColor Blue();     // the cursor, a loaded overlay, a clickable target
QColor Teal();     // data origin, as text
QColor Off();      // a raised surface, or a switch that is off
QColor Select();   // the row or cell you are on
QColor Unowned();  // executed code that belongs to no function
QColor ChipRec();
QColor ChipOrigin();

QColor Window();
QColor Base();
QColor Coverage();
QColor Border();
QColor Dark();
QColor Hover();
QColor TabInactive();

QColor SelectionFill();
QColor Touched();
QColor Overlay();
QColor Heat();
QColor Field();
QColor Decode();
QColor FunctionBar();
QColor FunctionEnd();

// %MUTED%, %RED%, %GREEN%, %AMBER% and %TEXT% in rich text, so panel HTML follows the file.
inline QString ApplyHtmlColors(QString html)
{
    html.replace("%MUTED%", Muted().name());
    html.replace("%RED%", Red().name());
    html.replace("%GREEN%", Green().name());
    html.replace("%AMBER%", Amber().name());
    html.replace("%TEXT%", Text().name());
    return html;
}

}

#endif
