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

#include <algorithm>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTextBlock>

#include "WBCodeEdit.h"
#include "WBColors.h"

namespace
{
class GutterWidget : public QWidget
{
public:
    explicit GutterWidget(CodeEditor* e) : QWidget(e), Ed(e) {}
    QSize sizeHint() const override { return QSize(Ed->GutterWidth(), 0); }
protected:
    void paintEvent(QPaintEvent* e) override { Ed->PaintGutter(e); }
private:
    CodeEditor* Ed;
};
}

CodeEditor::CodeEditor(QWidget* parent, Language lang) : QPlainTextEdit(parent)
{
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    setFont(f);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    Gutter = new GutterWidget(this);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this]() { setViewportMargins(GutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& r, int dy)
    {
        if (dy) Gutter->scroll(0, dy);
        else Gutter->update(0, r.y(), Gutter->width(), r.height());
    });
    setViewportMargins(GutterWidth(), 0, 0, 0);
    if (lang != Plain) new CodeHighlighter(document(), lang);
}

int CodeEditor::GutterWidth() const
{
    int digits = QString::number(std::max(1, blockCount())).size();
    return 10 + fontMetrics().horizontalAdvance('9') * std::max(3, digits);
}

void CodeEditor::resizeEvent(QResizeEvent* e)
{
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    Gutter->setGeometry(QRect(cr.left(), cr.top(), GutterWidth(), cr.height()));
}

void CodeEditor::keyPressEvent(QKeyEvent* e)
{
    // Tab inserts spaces (the script is Python)
    if (e->key() == Qt::Key_Tab && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && !isReadOnly())
    {
        insertPlainText("    ");
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

void CodeEditor::PaintGutter(QPaintEvent* e)
{
    QPainter p(Gutter);
    p.fillRect(e->rect(), WBColor::Off());
    QTextBlock block = firstVisibleBlock();
    int n = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    int cur = textCursor().blockNumber();
    while (block.isValid() && top <= e->rect().bottom())
    {
        if (block.isVisible() && bottom >= e->rect().top())
        {
            p.setPen(n == cur ? WBColor::Text() : WBColor::Muted());
            p.drawText(0, top, Gutter->width() - 5, fontMetrics().height(), Qt::AlignRight, QString::number(n + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        n++;
    }
}

CodeHighlighter::CodeHighlighter(QTextDocument* doc, CodeEditor::Language lang) : QSyntaxHighlighter(doc), Lang(lang)
{
    QTextCharFormat kw, num, builtin, key;
    kw.setForeground(WBColor::Blue());
    kw.setFontWeight(QFont::Bold);
    num.setForeground(WBColor::Amber());
    builtin.setForeground(WBColor::Teal());
    key.setForeground(WBColor::Blue());
    StringFmt.setForeground(WBColor::Green());
    CommentFmt.setForeground(WBColor::Muted());
    CommentFmt.setFontItalic(true);
    if (lang == CodeEditor::Python)
    {
        static const char* words[] = {"and", "as", "assert", "break", "class", "continue", "def", "del", "elif", "else",
            "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "None", "nonlocal",
            "not", "or", "pass", "raise", "return", "True", "False", "try", "while", "with", "yield"};
        for (const char* w : words) Rules.push_back({QRegularExpression(QString("\\b%1\\b").arg(w)), kw});
        Rules.push_back({QRegularExpression("\\b(print|len|open|range|str|int|float|dict|list|set|tuple|isinstance|getattr|setattr|sorted|max|min|super|Exception)\\b"), builtin});
        Rules.push_back({QRegularExpression("\\b(0x[0-9A-Fa-f]+|\\d+(\\.\\d+)?)\\b"), num});
        Rules.push_back({QRegularExpression("^\\s*@\\w+"), builtin});
    }
    else if (lang == CodeEditor::Json)
    {
        Rules.push_back({QRegularExpression("\\b(true|false|null)\\b"), kw});
        Rules.push_back({QRegularExpression("-?\\b\\d+(\\.\\d+)?\\b"), num});
    }
    if (lang == CodeEditor::Json)
    {
        QTextCharFormat k = key;
        k.setFontWeight(QFont::Bold);
        Rules.push_back({QRegularExpression("(\"(?:[^\"\\\\]|\\\\.)*\")\\s*:"), k, 1});
    }
}

void CodeHighlighter::highlightBlock(const QString& text)
{
    for (const Rule& r : Rules)
    {
        auto it = r.Re.globalMatch(text);
        while (it.hasNext())
        {
            QRegularExpressionMatch m = it.next();
            setFormat(m.capturedStart(r.Group), m.capturedLength(r.Group), r.Fmt);
        }
    }
    // strings and comments last, scanning so a '#' inside a string isn't a comment. Python's
    // triple-quoted strings carry across lines (block state 1 = inside """, 2 = inside ''')
    int i = 0;
    int state = previousBlockState() > 0 ? previousBlockState() : 0;
    if (state)
    {
        QString close = state == 1 ? "\"\"\"" : "'''";
        int e = text.indexOf(close);
        int end = e < 0 ? text.size() : e + 3;
        setFormat(0, end, StringFmt);
        if (e < 0) { setCurrentBlockState(state); return; }
        i = end;
    }
    setCurrentBlockState(0);
    while (i < text.size())
    {
        QChar c = text[i];
        if (Lang == CodeEditor::Python && c == '#') { setFormat(i, text.size() - i, CommentFmt); return; }
        if (c == '"' || (c == '\'' && Lang == CodeEditor::Python))
        {
            if (Lang == CodeEditor::Python && text.mid(i, 3) == QString(3, c))
            {
                int e = text.indexOf(QString(3, c), i + 3);
                if (e < 0)
                {
                    setFormat(i, text.size() - i, StringFmt);
                    setCurrentBlockState(c == '"' ? 1 : 2);
                    return;
                }
                setFormat(i, e + 3 - i, StringFmt);
                i = e + 3;
                continue;
            }
            int j = i + 1;
            while (j < text.size() && text[j] != c) j += text[j] == '\\' ? 2 : 1;
            int len = std::min<int>(j + 1, text.size()) - i;
            // keep a JSON key's own colour
            if (!(Lang == CodeEditor::Json && format(i).fontWeight() == QFont::Bold)) setFormat(i, len, StringFmt);
            i += len;
            continue;
        }
        i++;
    }
}
