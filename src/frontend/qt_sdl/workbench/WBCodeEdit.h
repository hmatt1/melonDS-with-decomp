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

#ifndef WB_CODEEDIT_H
#define WB_CODEEDIT_H

#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

// A plain-text editor with a line-number gutter, for the compare script and compare.json
// (Build > Compare setup).
class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT
public:
    enum Language { Plain, Python, Json };
    explicit CodeEditor(QWidget* parent = nullptr, Language lang = Plain);
    int GutterWidth() const;
    void PaintGutter(QPaintEvent* e);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    QWidget* Gutter;
};

class CodeHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    CodeHighlighter(QTextDocument* doc, CodeEditor::Language lang);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule { QRegularExpression Re; QTextCharFormat Fmt; int Group = 0; };
    std::vector<Rule> Rules;
    CodeEditor::Language Lang;
    QTextCharFormat StringFmt, CommentFmt;
};

#endif
