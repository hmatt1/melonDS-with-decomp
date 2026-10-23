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

#include <QHBoxLayout>
#include "WBStatusWidget.h"
#include "WBColors.h"
#include "WBHost.h"

static QLabel* Badge(const QString& text, QWidget* parent)
{
    QLabel* l = new QLabel(text, parent);
    l->setAlignment(Qt::AlignCenter);
    return l;
}

static void PaintBadge(QLabel* l, const QColor& bg, const QColor& fg)
{
    l->setStyleSheet(QString("QLabel { background: %1; color: %2; font-weight: bold; padding: 0px 5px; border-radius: 3px; }")
                         .arg(bg.name(), fg.name()));
    l->setVisible(true);
}

WBStatusWidget::WBStatusWidget(WBHost* host, QWidget* parent) : QWidget(parent), Host(host)
{
    Rec = Badge("REC", this);
    Hist = Badge("HIST", this);
    Origin = Badge("ORIGIN", this);
    Pause = Badge("PAUSED", this);
    PaintBadge(Pause, WBColor::Select(), WBColor::Text());
    Pause->setVisible(false);
    Text = new QLabel(this);
    QHBoxLayout* h = new QHBoxLayout(this);
    h->setContentsMargins(2, 0, 2, 0);
    h->setSpacing(4);
    h->addWidget(Rec);
    h->addWidget(Hist);
    h->addWidget(Origin);
    h->addWidget(Pause);
    h->addWidget(Text, 1);
    Timer.setInterval(500);
    connect(&Timer, &QTimer::timeout, this, &WBStatusWidget::refresh);
    Timer.start();
}

void WBStatusWidget::refresh()
{
    if (!Host || !isVisible()) return;
    WBStatus s = Host->Status();
    PaintBadge(Rec, s.Recording ? WBColor::ChipRec() : WBColor::Off(), s.Recording ? WBColor::Text() : WBColor::Muted());
    bool hist = s.Recording && s.History;
    PaintBadge(Hist, WBColor::Off(), !hist ? WBColor::Muted() : s.HistoryWrapped ? WBColor::Amber() : WBColor::Text());
    bool origin = s.Recording && s.Origin;
    PaintBadge(Origin, origin ? WBColor::ChipOrigin() : WBColor::Off(), origin ? WBColor::Text() : WBColor::Muted());
    Rec->setToolTip(!s.Recording && !s.Available ? "JIT" : QString());
    bool paused = s.EmuState == WBStatus::Paused || s.EmuState == WBStatus::Break;
    Pause->setVisible(paused);
    Pause->setText(s.EmuState == WBStatus::Break ? "BREAK" : "PAUSED");
    QString t = QString("%1 frames recorded").arg(s.FramesRecorded);
    if (s.History) t += QString(" | history %1 / %2").arg(s.HistoryCount).arg(s.HistoryCap);
    if (s.EmuState == WBStatus::Running) t += QString(" | %1 fps").arg(s.Fps, 0, 'f', 1);
    Text->setText(t);
}
