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

#ifndef WB_STATUSWIDGET_H
#define WB_STATUSWIDGET_H

#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class WBHost;

// Recording indicator for the emulator window's status bar:
// REC / HIST / ORIGIN / PAUSED badges, frames recorded, history fill and speed.
class WBStatusWidget : public QWidget
{
    Q_OBJECT
public:
    WBStatusWidget(WBHost* host, QWidget* parent);
    void refresh();

private:
    QPointer<WBHost> Host;     // the instance deletes its host before its windows
    QLabel* Rec;
    QLabel* Hist;
    QLabel* Origin;
    QLabel* Pause;
    QLabel* Text;
    QTimer Timer;
};

#endif
