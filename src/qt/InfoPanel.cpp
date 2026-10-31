#include "InfoPanel.h"

#include <QScrollBar>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace givqt
{

InfoPanel::InfoPanel(QWidget* parent) : QWidget(parent)
{
    textEdit_ = new QTextBrowser(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(textEdit_);
}

void InfoPanel::setInfo(const QString& text)
{
    if (html_ == text) return;
    html_ = text;

    const int vValue = textEdit_->verticalScrollBar()->value();
    const int hValue = textEdit_->horizontalScrollBar()->value();
    textEdit_->setHtml(text);
    textEdit_->verticalScrollBar()->setValue(vValue);
    textEdit_->horizontalScrollBar()->setValue(hValue);
}

} // namespace givqt
