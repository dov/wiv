// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// InfoPanel.h - the content of View > Image Info ("i" key), giv's GivInfo
// dialog (src/giv-info.gob there): a read-only rich-text (HTML table) view
// whose contents MainWindow rebuilds whenever the displayed image changes.
// Hosted in a QDockWidget so the user can dock it or float it as its own
// window. Like giv's, it preserves the scroll position across content
// updates so a live refresh doesn't yank the user back to the top. A filter
// box above the text narrows long property lists (EXIF, DICOM, netCDF...), like
// a properties search.
//
#include <QWidget>

class QLineEdit;
class QTextBrowser;

namespace givqt
{

class InfoPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit InfoPanel(QWidget* parent = nullptr);

    // Replaces the HTML. A no-op (so the user's selection and scroll position
    // are left alone) if it is unchanged.
    void setInfo(const QString& html);

private:
    // The HTML to show for html_ under the current filter text: rows whose key or value
    // contains the text (case-insensitive) stay; a section whose title matches stays whole;
    // sections left empty are dropped. Empty filter: html_ unchanged.
    QString filteredHtml() const;
    void render();

    QLineEdit* filterEdit_ = nullptr;
    QTextBrowser* textEdit_ = nullptr;
    QString html_;     // as given to setInfo()
    QString rendered_; // what textEdit_ currently shows
};

} // namespace givqt
