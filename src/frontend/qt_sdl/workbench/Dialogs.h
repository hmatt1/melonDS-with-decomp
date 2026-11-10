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

#ifndef WB_DIALOGS_H
#define WB_DIALOGS_H

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QStackedWidget>

#include "KnowledgeStore.h"

class WorkbenchWindow;

class AnnotationDialog : public QDialog
{
    Q_OBJECT
public:
    AnnotationDialog(WorkbenchWindow* w, const WBKB::Annotation* existing, melonDS::u32 addr, melonDS::u32 size);
private:
    void UpdateFields();
    void Save();
    WorkbenchWindow* W;
    qint64 Id = 0;
    QComboBox* Kind;
    QLineEdit* Addr;
    QLineEdit* End;
    QComboBox* Reg;
    QCheckBox* WholeFunction;
    QComboBox* TypeName;
    QComboBox* Field;
    QSpinBox* Overlay;
    QLineEdit* Savestate;
    QLabel* Resolved;
    QPlainTextEdit* Comment;
    QLineEdit* Tags;
    QWidget* RowAddr;
    QWidget* RowEnd;
    QWidget* RowReg;
    QWidget* RowType;
    QWidget* RowField;
    QWidget* RowOverlay;
    QWidget* RowSavestate;
};

class FunctionDialog : public QDialog
{
    Q_OBJECT
public:
    FunctionDialog(WorkbenchWindow* w, melonDS::u32 addr);
private:
    void Detect();
    WorkbenchWindow* W;
    QLineEdit* Addr;
    QComboBox* Mode;
    QSpinBox* Size;
    QLineEdit* Name;
    QLineEdit* Signature;
    QComboBox* Subsystem;
    QLineEdit* SourceFile;
    qint64 Existing = 0;
};

class DataDialog : public QDialog
{
    Q_OBJECT
public:
    DataDialog(WorkbenchWindow* w, melonDS::u32 addr, melonDS::u32 size);
private:
    WorkbenchWindow* W;
    QLineEdit* Addr;
    QSpinBox* Size;
    QComboBox* Type;
    QLineEdit* Name;
    qint64 Existing = 0;
};

class TypeEditorDialog : public QDialog
{
    Q_OBJECT
public:
    TypeEditorDialog(WorkbenchWindow* w);
private:
    void Reload();
    void Select(const QString& name);
    void Preview();
    WorkbenchWindow* W;
    QListWidget* List;
    QPlainTextEdit* Edit;
    QLabel* Layout;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(WorkbenchWindow* w);
private:
    void Apply();
    WorkbenchWindow* W;
    QLineEdit* KbPath;
    QLineEdit* SourceRoot;
    QLineEdit* Compare;
    QLineEdit* Editor;
    QLineEdit* Author;
    QSpinBox* Cap;
    QCheckBox* ApiOn;
    QSpinBox* ApiPort;
    QCheckBox* ApiMemWrite;
    // the API as the dialog opened on it: running (perhaps from the command line) or as saved
    bool ApiShownOn = false, ApiShownMemWrite = false;
    int ApiShownPort = 0;
    QCheckBox* AllRom;
    QCheckBox* AutoImport;
    QLabel* ApiInfo;
};

class BreakpointDialog : public QDialog
{
    Q_OBJECT
public:
    BreakpointDialog(WorkbenchWindow* w, melonDS::u32 addr);
    QComboBox* Kind;
    QLineEdit* Addr;
    QSpinBox* Size;
    QComboBox* Cpu;
    QLineEdit* Condition;
    QCheckBox* Temporary;
};

// Imports a generic static-analysis export (functions, labels, xrefs, decompiler text).
// Returns the number of imported entries, or -1 on error.
int ImportAnalysisJson(WBKB::KnowledgeStore& kb, const QString& path, QString& error);

#endif
