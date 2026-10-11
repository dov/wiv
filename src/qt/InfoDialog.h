#pragma once
//
// InfoDialog.h - View > Image Info ("i" key), giv's GivInfo dialog
// (src/giv-info.gob there): a non-modal, read-only rich-text (HTML table) view whose
// contents MainWindow rebuilds whenever the displayed image changes. Like
// giv's, it preserves the scroll position across content updates so a live
// refresh doesn't yank the user back to the top.
//
#include <QDialog>

class QTextBrowser;

namespace givqt
{

class InfoDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit InfoDialog(QWidget* parent = nullptr);

    // Replaces the window title and HTML. A no-op for the text (so the user's
    // selection and scroll position are left alone) if it is unchanged.
    void setInfo(const QString& title, const QString& html);

private:
    QTextBrowser* textEdit_ = nullptr;
    QString html_;
};

} // namespace givqt
