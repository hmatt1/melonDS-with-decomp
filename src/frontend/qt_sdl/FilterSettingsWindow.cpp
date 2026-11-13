/*
    Screen filter settings windows: see FilterSettingsWindow.h.
*/

#include "FilterSettingsWindow.h"

#include <cstring>
#include <memory>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>

#include "Config.h"

void FilterWindow::setActive(bool on)
{
    if (!useBox) return;
    QSignalBlocker b(useBox);
    useBox->setChecked(on);
}

namespace
{

QString formatValue(const GridParamDef& d, double v)
{
    int decimals = d.integer ? 0 : (d.max - d.min <= 1.f ? 3 : 2);
    QString s = QString::number(v, 'f', decimals);
    if (d.unit && *d.unit) s += QString(d.unit) == "x" ? QString("x") : QString(" ") + d.unit;
    return s;
}

double sliderToValue(const GridParamDef& d, int v) { return d.min + (d.max - d.min) * v / (double)gridParamSteps(d); }
int valueToSlider(const GridParamDef& d, double v) { return qRound((v - d.min) / (d.max - d.min) * gridParamSteps(d)); }

} // namespace

FilterWindow* buildFilterWindow(QWidget* parent, Config::Table& windowCfg, const FilterWindowSpec& spec)
{
    auto* win = new FilterWindow(parent);
    win->setWindowTitle(spec.title);
    auto* grid = new QGridLayout(win);
    Config::Table* cfg = &windowCfg;   // the window's own table; lives as long as its MainWindow

    // whether this filter is the one drawn: without it, tuning a filter that isn't on would seem
    // to do nothing
    win->useBox = new QCheckBox(QString("Use the %1").arg(spec.filterName), win);
    win->useBox->setToolTip(QString("Draw the screens with the %1. Unticking turns the screen filter off.\n"
                                    "Same as View > Screen filter; it applies to every window of this instance.\n\n"
                                    "The settings below can be changed either way, and are used when the filter is on.")
                                .arg(spec.filterName));
    QObject::connect(win->useBox, &QCheckBox::toggled, win, [set = spec.setActive](bool on) { if (set) set(on); });
    grid->addWidget(win->useBox, 0, 0, 1, 4);

    // one slider per parameter; a change applies on the next frame. Parameters with a section go
    // in a box of that name, which the filter can start with its own controls (sectionTop)
    const int paramRow = 1;
    auto sliders = std::make_shared<std::vector<QSlider*>>();
    QGridLayout* target = grid;
    const char* curSection = nullptr;
    int mainRow = paramRow, boxRow = 0;
    win->rows.assign(spec.count, {});
    for (int i = 0; i < spec.count; i++)
    {
        if (spec.params[i].section && (!curSection || strcmp(spec.params[i].section, curSection) != 0))
        {
            curSection = spec.params[i].section;
            auto* box = new QGroupBox(curSection, win);
            target = new QGridLayout(box);
            target->setColumnStretch(1, 1);
            grid->addWidget(box, mainRow++, 0, 1, 4);
            boxRow = 0;
            if (spec.sectionTop) spec.sectionTop(curSection, box, target, boxRow);
        }
        const GridParamDef& def = spec.params[i];
        const int steps = gridParamSteps(def);
        double val = windowCfg.GetDouble(def.key);
        QString tip = QString(def.tip) + "\n\nDefault: " + formatValue(def, def.def);

        auto* name = new QLabel(def.name, win);
        name->setToolTip(tip);
        auto* slider = new QSlider(Qt::Horizontal, win);
        slider->setToolTip(tip);
        auto* num = new QLabel(win);
        slider->setRange(0, steps);
        slider->setValue(valueToSlider(def, val));
        num->setMinimumWidth(64);
        num->setText(formatValue(def, val));
        sliders->push_back(slider);

        QObject::connect(slider, &QSlider::valueChanged, win, [cfg, def, i, num, set = spec.setParam](int v)
        {
            double f = sliderToValue(def, v);
            num->setText(formatValue(def, f));
            cfg->SetDouble(def.key, f);
            if (set) set(i, (float)f);
        });

        const int r = target == grid ? mainRow++ : boxRow++;
        target->addWidget(name, r, 0);
        target->addWidget(slider, r, 1);
        target->addWidget(num, r, 2);
        win->rows[i] = {name, slider, num};
    }

    int row = mainRow;
    FilterExtras extras;
    if (spec.addExtras) extras = spec.addExtras(win, grid, paramRow, row);

    // presets: "Default" is the built-in values and can't be deleted or overwritten; the rest
    // live in a JSON file next to melonDS.toml
    if (!spec.legacyPresetFile.isEmpty() && !QFile::exists(spec.presetFile) && QFile::exists(spec.legacyPresetFile))
        QFile::copy(spec.legacyPresetFile, spec.presetFile);

    auto* presetRow = new QHBoxLayout();
    auto* presetPick = new QComboBox(win);
    auto* presetName = new QLineEdit(win);
    auto* presetSave = new QPushButton("Save", win);
    auto* presetDelete = new QPushButton("Delete", win);
    presetPick->setToolTip("Apply a set of these settings.\n"
                           "Default always restores the built-in values, even if it's the only entry.\n"
                           "Built-in presets (if any) come next, then the ones you saved.");
    presetName->setPlaceholderText("Preset name");
    presetSave->setToolTip("Save the current settings under the name in the box");
    presetDelete->setToolTip("Delete the selected saved preset (not Default or a built-in one)");
    presetRow->addWidget(presetPick, 1);
    presetRow->addWidget(presetName, 1);
    presetRow->addWidget(presetSave);
    presetRow->addWidget(presetDelete);
    grid->addLayout(presetRow, row++, 0, 1, 4);

    if (!spec.note.isEmpty())
    {
        auto* note = new QLabel(spec.note, win);
        note->setWordWrap(true);
        note->setEnabled(false);   // greyed: a hint, not a control
        grid->addWidget(note, row++, 0, 1, 4);
    }

    const QString presetFile = spec.presetFile;
    auto loadPresets = [presetFile]()
    {
        QFile f(presetFile);
        if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    };
    auto savePresets = [presetFile](const QJsonObject& all)
    {
        QFile f(presetFile);
        if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(all).toJson());
    };
    // entries: Default (kind 0), the built-in presets (1), then the saved ones (2)
    const auto builtins = spec.builtins;
    auto refill = [presetPick, loadPresets, builtins](const QString& select)
    {
        QSignalBlocker b(presetPick);
        presetPick->clear();
        presetPick->addItem("Default", 0);
        if (!builtins.empty())
        {
            presetPick->insertSeparator(presetPick->count());
            for (const auto& bi : builtins) presetPick->addItem(bi.first, 1);
        }
        const QStringList saved = loadPresets().keys();
        if (!saved.isEmpty())
        {
            presetPick->insertSeparator(presetPick->count());
            for (const QString& n : saved) presetPick->addItem(n, 2);
        }
        // (an empty name would match a separator, whose text is empty)
        int i = select.isEmpty() ? -1 : presetPick->findText(select);
        presetPick->setCurrentIndex(i < 0 ? 0 : i);
    };
    auto isReserved = [builtins](const QString& n)
    {
        if (n.compare("Default", Qt::CaseInsensitive) == 0) return true;
        for (const auto& bi : builtins)
            if (n.compare(bi.first, Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    refill(QString());

    const GridParamDef* params = spec.params;
    const int count = spec.count;
    auto current = [sliders, params, count, extras]()
    {
        QJsonObject o = extras.toJson ? extras.toJson() : QJsonObject();
        for (int i = 0; i < count; i++)
            o[params[i].key] = sliderToValue(params[i], (*sliders)[i]->value());
        return o;
    };
    // a key the preset lacks (all of them for Default) gets its built-in value. Moving a widget
    // fires its usual handler, so the config and the screen follow whether or not the filter is on
    auto apply = [sliders, params, count, extras](const QJsonObject& o)
    {
        for (int i = 0; i < count; i++)
            (*sliders)[i]->setValue(valueToSlider(params[i], o.value(params[i].key).toDouble(params[i].def)));
        if (extras.fromJson) extras.fromJson(o);
    };

    // activated, not currentTextChanged: picking the entry already shown (say Default, when it's
    // the only one, after moving a slider) applies it again
    QObject::connect(presetPick, &QComboBox::activated, win, [=](int idx)
    {
        QString n = presetPick->itemText(idx);
        int kind = presetPick->itemData(idx).toInt();
        presetDelete->setEnabled(kind == 2);
        presetName->setText(kind == 2 ? n : QString());
        QJsonObject o;
        if (kind == 1)
        {
            for (const auto& bi : builtins)
                if (bi.first == n) o = bi.second;
        }
        else if (kind == 2)
            o = loadPresets().value(n).toObject();
        apply(o);
    });
    presetDelete->setEnabled(false);
    const QString title = spec.title;
    QObject::connect(presetSave, &QPushButton::clicked, win, [=]()
    {
        QString n = presetName->text().trimmed();
        if (n.isEmpty() || isReserved(n))
        {
            QMessageBox::information(win, title, builtins.empty()
                ? "Type a name other than Default to save a preset."
                : "Type a name other than Default or a built-in preset's to save a preset.");
            return;
        }
        QJsonObject all = loadPresets();
        all[n] = current();
        savePresets(all);
        refill(n);
        presetDelete->setEnabled(true);
    });
    QObject::connect(presetDelete, &QPushButton::clicked, win, [=]()
    {
        if (presetPick->currentData().toInt() != 2) return;
        QJsonObject all = loadPresets();
        all.remove(presetPick->currentText());
        savePresets(all);
        refill(QString());
        presetName->clear();
        presetDelete->setEnabled(false);
    });

    grid->setColumnStretch(1, 1);
    win->resize(520, 380);
    return win;
}
