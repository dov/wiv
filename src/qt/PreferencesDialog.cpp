#include "PreferencesDialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace givqt
{

namespace
{
QSettings settings() { return QSettings(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv"); }
} // namespace

QColor PreferencesDialog::loadBackgroundColor()
{
    auto s = settings();
    return s.value("backgroundColor", QColor(Qt::white)).value<QColor>();
}

int PreferencesDialog::loadMsaaSamples()
{
    auto s = settings();
    return s.value("msaaSamples", 4).toInt();
}

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Preferences");

    backgroundColor_ = loadBackgroundColor();

    colorButton_ = new QPushButton(this);
    colorButton_->setAutoFillBackground(true);
    auto updateSwatch = [this]() {
        colorButton_->setStyleSheet(QString("background-color: %1").arg(backgroundColor_.name()));
    };
    updateSwatch();
    connect(colorButton_, &QPushButton::clicked, this, [this, updateSwatch]() {
        QColor c = QColorDialog::getColor(backgroundColor_, this, "Background Color");
        if (c.isValid())
        {
            backgroundColor_ = c;
            updateSwatch();
            emit backgroundColorChanged(backgroundColor_);
        }
    });

    msaaCombo_ = new QComboBox(this);
    msaaCombo_->addItem("Off (1x)", 1);
    msaaCombo_->addItem("2x", 2);
    msaaCombo_->addItem("4x", 4);
    msaaCombo_->addItem("8x", 8);
    int current = loadMsaaSamples();
    int idx = msaaCombo_->findData(current);
    msaaCombo_->setCurrentIndex(idx >= 0 ? idx : msaaCombo_->findData(4));

    auto form = new QFormLayout();
    form->addRow("Background color:", colorButton_);
    form->addRow("Antialiasing (restart required):", msaaCombo_);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(new QLabel("Antialiasing changes take effect after restarting wiv."));
    layout->addWidget(buttons);
    setLayout(layout);
}

int PreferencesDialog::msaaSamples() const
{
    return msaaCombo_->currentData().toInt();
}

void PreferencesDialog::save()
{
    auto s = settings();
    s.setValue("backgroundColor", backgroundColor_);
    s.setValue("msaaSamples", msaaSamples());
}

} // namespace givqt
