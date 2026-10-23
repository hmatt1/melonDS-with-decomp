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

// Getting started and Help > Feature requirements (UI_REQUIREMENTS PR-6): what Recording, Write
// history and Data origin give you, what each needs, and whether it is on now.

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "Panels.h"
#include "WBHost.h"
#include "Config.h"

namespace
{
// one row of the feature table: what it needs, what turning it on late loses, and what uses it
struct FeatureRow
{
    const char* Feature;
    const char* Needs;
    const char* Covers;
    const char* UsedBy;
    WBNeed Need;            // whose state the "Now" column shows
};

const FeatureRow FeatureTable[] = {
    {"Recording", "A game running, and the interpreter (JIT off)", "What runs from the moment it starts",
     "Coverage map, execution counts, ARM/Thumb modes, call graph, call stack, card reads, overlay loads", WBNeed::Recording},
    {"Write history", "Recording (started for you)", "Every write from the moment it is turned on, up to its capacity",
     "Write history panel, writes.list, Inspector", WBNeed::WriteHistory},
    {"Data origin", "Recording (started for you)",
     "Data loaded from the moment it is turned on; earlier data has no origin, except files recognised by their content",
     "Data origin panel, Inspector, origin.get", WBNeed::DataOrigin},
    {"Breakpoints and stepping", "The interpreter (JIT off)", "Any time", "Breakpoints panel, Debug menu", WBNeed::Interpreter},
    {"Game archive", "A detected archive (TrackMania's vfs.zip)", "Always, by content; card reads only while recording",
     "Archive panel, archived-file labels", WBNeed::Archive},
    {"Containers", "NARC or SDAT files in the ROM (packed whole or not, nested or not)", "Always, by content; card reads only while recording",
     "Containers panel, labels of the files inside containers", WBNeed::Containers},
};
}

QString WorkbenchWindow::FeatureTableHtml() const
{
    QString h = "<table cellspacing=0 cellpadding=4 border=1 style='border-collapse: collapse'>"
                "<tr><th align=left>Feature</th><th align=left>Needs</th><th align=left>Covers</th><th align=left>Used by</th><th align=left>Now</th></tr>";
    for (const FeatureRow& f : FeatureTable)
    {
        QString missing = H->Missing(f.Need);
        QString now = missing.isEmpty() ? "<b>on</b>" : missing.toHtmlEscaped();
        h += QString("<tr><td><b>%1</b></td><td>%2</td><td>%3</td><td>%4</td><td>%5</td></tr>")
                 .arg(f.Feature, f.Needs, f.Covers, f.UsedBy, now);
    }
    h += "</table><p>Recording, write history and data origin are already on when a game boots, unless the JIT "
         "recompiler is on (recording needs the interpreter: Config &gt; Emu settings, then reset) or you started "
         "melonDS with <code>--no-record</code>. Turn one off from the Recording menu when you want the extra speed "
         "or a smaller write-history file. <b>Recording &gt; Reset game with tracking</b> restarts with the switches you pick.</p>";
    return h;
}

void WorkbenchWindow::ShowFeatureRequirements()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Feature requirements");
    dlg.resize(900, 420);
    auto* v = new QVBoxLayout(&dlg);
    auto* view = new QTextBrowser(&dlg);
    view->setHtml(FeatureTableHtml());
    v->addWidget(view);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    dlg.exec();
}

GettingStartedPanel::GettingStartedPanel(WorkbenchWindow* w) : WBPanel(w)
{
    View = new QTextBrowser(this);
    View->setOpenLinks(false);
    QPushButton* rec = RecBtn = new QPushButton("Start recording", this);
    QPushButton* hist = HistBtn = new QPushButton("Turn on write history", this);
    QPushButton* origin = OriginBtn = new QPushButton("Turn on data origin", this);
    QPushButton* reset = new QPushButton("Reset game with tracking...", this);
    QCheckBox* show = new QCheckBox("Show at startup", this);
    show->setChecked(!Config::GetGlobalTable().GetBool("Workbench.GettingStartedDismissed"));
    connect(rec, &QPushButton::clicked, this, [this]() { H->SetRecording(true); });
    connect(hist, &QPushButton::clicked, this, [this]() { H->SetWriteHistory(true); });
    connect(origin, &QPushButton::clicked, this, [this]() { H->SetDataOrigin(true); });
    connect(reset, &QPushButton::clicked, this, [this]() { W->ResetWithTracking(); });
    connect(show, &QCheckBox::toggled, this, [](bool on)
    {
        Config::Table cfg = Config::GetGlobalTable();
        cfg.SetBool("Workbench.GettingStartedDismissed", !on);
        Config::Save();
    });
    auto* b = new QHBoxLayout();
    b->addWidget(rec);
    b->addWidget(hist);
    b->addWidget(origin);
    b->addWidget(reset);
    b->addStretch();
    b->addWidget(show);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->addWidget(View, 1);
    v->addLayout(b);
}

void GettingStartedPanel::Refresh()
{
    // a switch that is already on says so instead of offering to turn it on
    WBStatus s = H->Status();
    bool can = s.Available && s.EmuState != WBStatus::NoGame;
    RecBtn->setText(s.Recording ? "Recording is on" : "Start recording");
    RecBtn->setEnabled(can && !s.Recording);
    HistBtn->setText(s.History ? "Write history is on" : "Turn on write history");
    HistBtn->setEnabled(can && !s.History);
    OriginBtn->setText(s.Origin ? "Data origin is on" : "Turn on data origin");
    OriginBtn->setEnabled(can && !s.Origin);
    QString h =
        "<h3>Getting started</h3>"
        "<p>Recording, write history and data origin are already on. The badges in the status bar (REC, HIST, ORIGIN) "
        "show them; click a badge or use the Recording menu to turn one off.</p>"
        "<ul>"
        "<li><b>Recording</b> is what every byte of memory did: executed (and as ARM or Thumb), read, written, copied by DMA, "
        "loaded from the card, and the calls between functions. The Disassembly, Coverage map, Call graph, "
        "Call stack and Functions panels read it.</li>"
        "<li><b>Write history</b> is every write: when, by which code, the old and new value. The Write history panel groups "
        "addresses by how often they were written.</li>"
        "<li><b>Data origin</b> follows data from the ROM file it came from, through copies, unpacking and arithmetic, "
        "to where it ends up (Data origin panel, Inspector).</li>"
        "</ul>"
        "<p>If the JIT recompiler is on, recording stays off: switch to the interpreter in Config &gt; Emu settings (the game resets). "
        "Turn these off when you want the extra speed, or when you do not want the write-history file to grow.</p>"
        "<p>Functions are created automatically when their entry runs, named <code>sub_</code> plus the address. "
        "Rename them as you learn what they are. Define function (P) is for code that never ran, an unusual prologue, an overlay, or a branch that is not a call.</p>"
        "<p>The Decompiler panel is a draft of the machine code as C. Source and diff is the function's real source file and the compare against the original build.</p>"
        "<h4>What each needs, and whether it is on</h4>" + W->FeatureTableHtml();
    if (h != LastHtml)
    {
        LastHtml = h;
        int pos = View->verticalScrollBar() ? View->verticalScrollBar()->value() : 0;
        View->setHtml(h);
        if (View->verticalScrollBar()) View->verticalScrollBar()->setValue(pos);
    }
}
