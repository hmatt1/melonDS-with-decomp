/*
    The settings windows of the screen filters (View > Screen filter > LCD / CRT filter settings).
    One builder makes both: a "Use this filter" box, a slider per parameter, any filter-specific
    controls, and a preset row whose "Default" entry always restores the built-in values.
*/

#ifndef FILTERSETTINGSWINDOW_H
#define FILTERSETTINGSWINDOW_H

#include <functional>

#include <QCloseEvent>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include "Screen.h"

class QCheckBox;
class QGridLayout;

namespace Config { class Table; }

// a settings window that tells its owner when the user closes it
class FilterWindow : public QWidget
{
public:
    explicit FilterWindow(QWidget* parent) : QWidget(parent, Qt::Window) {}
    std::function<void()> onClosed;

    // ticks or unticks "Use this filter" without telling the owner (the owner changed the mode)
    void setActive(bool on);

    QCheckBox* useBox = nullptr;

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (onClosed) onClosed();
        QWidget::closeEvent(event);
    }
};

// controls a filter has besides its sliders, and how they go to and from a preset
struct FilterExtras
{
    std::function<QJsonObject()> toJson;
    std::function<void(const QJsonObject&)> fromJson;   // a missing key means its default
};

struct FilterWindowSpec
{
    QString title;
    QString filterName;                       // "LCD filter", for "Use the LCD filter"
    const GridParamDef* params = nullptr;
    int count = 0;
    std::function<void(int, float)> setParam; // parameter i changed: give it to the screen
    std::function<void(bool)> setActive;      // "Use this filter" ticked / unticked
    QString presetFile;                       // next to melonDS.toml
    QString legacyPresetFile;                 // copied to presetFile if that doesn't exist yet
    QString note;                             // a hint at the bottom, may be empty
    // adds the filter's own controls: next to slider i at (paramRow + i, 3), or in whole rows
    // from `row` on (which it moves past). May be empty.
    std::function<FilterExtras(QWidget* win, QGridLayout* grid, int paramRow, int& row)> addExtras;
};

FilterWindow* buildFilterWindow(QWidget* parent, Config::Table& windowCfg, const FilterWindowSpec& spec);

#endif
