// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "InfoPanel.h"

#include <QLineEdit>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocumentFragment>
#include <QVBoxLayout>

namespace givqt
{

InfoPanel::InfoPanel(QWidget* parent) : QWidget(parent)
{
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText("Filter properties...");
    filterEdit_->setClearButtonEnabled(true);
    textEdit_ = new QTextBrowser(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(filterEdit_);
    layout->addWidget(textEdit_);

    connect(filterEdit_, &QLineEdit::textChanged, this, [this]() { render(); });
}

void InfoPanel::setInfo(const QString& text)
{
    if (html_ == text) return;
    html_ = text;
    render();
}

QString InfoPanel::filteredHtml() const
{
    const QString pattern = filterEdit_->text().trimmed();
    if (pattern.isEmpty()) return html_;

    auto plain = [](const QString& html) { return QTextDocumentFragment::fromHtml(html).toPlainText(); };
    static const QRegularExpression sectionRe("<h3>(.*?)</h3>(<table[^>]*>)(.*?)</table>",
                                              QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression rowRe("<tr>.*?</tr>", QRegularExpression::DotMatchesEverythingOption);

    QString out;
    bool any = false;
    for (auto it = sectionRe.globalMatch(html_); it.hasNext();)
    {
        const auto m = it.next();
        // Content outside the sections (if any) is dropped while filtering.
        const bool titleMatches = plain(m.captured(1)).contains(pattern, Qt::CaseInsensitive);
        QString rows;
        for (auto r = rowRe.globalMatch(m.captured(3)); r.hasNext();)
        {
            const QString row = r.next().captured();
            if (titleMatches || plain(row).contains(pattern, Qt::CaseInsensitive)) rows += row;
        }
        if (!rows.isEmpty())
        {
            out += "<h3>" + m.captured(1) + "</h3>" + m.captured(2) + rows + "</table>";
            any = true;
        }
    }
    if (!any) return "<p><i>No properties match the filter.</i></p>";
    return out;
}

void InfoPanel::render()
{
    const QString html = filteredHtml();
    if (html == rendered_) return;
    rendered_ = html;

    const int vValue = textEdit_->verticalScrollBar()->value();
    const int hValue = textEdit_->horizontalScrollBar()->value();
    textEdit_->setHtml(html);
    textEdit_->verticalScrollBar()->setValue(vValue);
    textEdit_->horizontalScrollBar()->setValue(hValue);
}

} // namespace givqt
