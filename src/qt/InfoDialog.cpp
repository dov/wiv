#include "InfoDialog.h"

#include <QDialogButtonBox>
#include <QScrollBar>
#include <QShortcut>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace givqt
{

InfoDialog::InfoDialog(QWidget* parent) : QDialog(parent)
{
    resize(520, 480);

    textEdit_ = new QTextBrowser(this);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(textEdit_);
    layout->addWidget(buttons);

    // The View > Image Info action's "I" shortcut only fires while the main
    // window is active, so give this dialog its own to toggle it back off.
    auto* closeShortcut = new QShortcut(QKeySequence("I"), this);
    connect(closeShortcut, &QShortcut::activated, this, &QDialog::close);
}

void InfoDialog::setInfo(const QString& title, const QString& text)
{
    setWindowTitle(title);
    if (html_ == text) return;
    html_ = text;

    const int vValue = textEdit_->verticalScrollBar()->value();
    const int hValue = textEdit_->horizontalScrollBar()->value();
    textEdit_->setHtml(text);
    textEdit_->verticalScrollBar()->setValue(vValue);
    textEdit_->horizontalScrollBar()->setValue(hValue);
}

} // namespace givqt
