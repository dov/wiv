#pragma once
//
// InfoPanel.h - the content of View > Image Info ("i" key), giv's GivInfo
// dialog (src/giv-info.gob there): a read-only rich-text (HTML table) view
// whose contents MainWindow rebuilds whenever the displayed image changes.
// Hosted in a QDockWidget so the user can dock it or float it as its own
// window. Like giv's, it preserves the scroll position across content
// updates so a live refresh doesn't yank the user back to the top.
//
#include <QWidget>

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
    QTextBrowser* textEdit_ = nullptr;
    QString html_;
};

} // namespace givqt
