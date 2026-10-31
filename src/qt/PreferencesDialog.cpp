#include "PreferencesDialog.h"

#include <QAction>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
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

namespace
{
constexpr int kKeyColumns = 3; // most built-in commands have up to three alternative keys

QString shortcutKey(const QAction* a) { return "shortcuts/" + a->objectName(); }

QStringList toTexts(const QList<QKeySequence>& keys)
{
    QStringList out;
    for (const QKeySequence& k : keys)
        if (!k.isEmpty()) out << k.toString(QKeySequence::PortableText);
    return out;
}

QList<QKeySequence> fromTexts(const QStringList& texts)
{
    QList<QKeySequence> out;
    for (const QString& t : texts)
        if (!t.isEmpty()) out << QKeySequence(t, QKeySequence::PortableText);
    return out;
}
} // namespace

void PreferencesDialog::applySavedShortcuts(QAction* action)
{
    auto s = settings();
    const QString key = shortcutKey(action);
    if (!s.contains(key)) return;
    action->setShortcuts(fromTexts(s.value(key).toString().split('\n', Qt::SkipEmptyParts)));
}

PreferencesDialog::PreferencesDialog(const QList<QAction*>& bindable, QWidget* parent)
    : QDialog(parent), bindable_(bindable)
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
        if (!saveShortcuts()) return; // conflicting keys: stay open
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto general = new QWidget(this);
    auto generalLayout = new QVBoxLayout(general);
    generalLayout->addLayout(form);
    generalLayout->addWidget(new QLabel("Antialiasing changes take effect after restarting wiv."));
    generalLayout->addStretch();

    auto tabs = new QTabWidget(this);
    tabs->addTab(general, "General");
    tabs->addTab(buildKeyboardTab(), "Keyboard");

    auto layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
    resize(560, 520);
    setLayout(layout);
}

QWidget* PreferencesDialog::buildKeyboardTab()
{
    auto page = new QWidget(this);
    auto layout = new QVBoxLayout(page);

    auto filter = new QLineEdit(page);
    filter->setPlaceholderText("Filter commands...");
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);

    keyTable_ = new QTableWidget(bindable_.size(), 2 + kKeyColumns, page);
    QStringList headers{"Menu", "Command"};
    for (int c = 0; c < kKeyColumns; ++c) headers << QString("Key %1").arg(c + 1);
    keyTable_->setHorizontalHeaderLabels(headers);
    keyTable_->verticalHeader()->setVisible(false);
    keyTable_->setSelectionMode(QAbstractItemView::NoSelection);
    keyTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    keyTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    for (int row = 0; row < bindable_.size(); ++row)
    {
        QAction* a = bindable_[row];
        keyTable_->setItem(row, 0, new QTableWidgetItem(a->property("menuTitle").toString()));
        keyTable_->setItem(row, 1, new QTableWidgetItem(QString(a->text()).remove('&').remove("...")));
        QList<QKeySequenceEdit*> edits;
        for (int c = 0; c < kKeyColumns; ++c)
        {
            auto* e = new QKeySequenceEdit(keyTable_);
            e->setMaximumSequenceLength(1);
            e->setClearButtonEnabled(true);
            keyTable_->setCellWidget(row, 2 + c, e);
            edits << e;
        }
        keyEdits_ << edits;
        fillKeyEditors(row, toTexts(a->shortcuts()));
    }
    keyTable_->resizeColumnsToContents();
    keyTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(keyTable_);

    connect(filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int row = 0; row < keyTable_->rowCount(); ++row)
        {
            const bool match = text.isEmpty() || keyTable_->item(row, 1)->text().contains(text, Qt::CaseInsensitive) ||
                               keyTable_->item(row, 0)->text().contains(text, Qt::CaseInsensitive);
            keyTable_->setRowHidden(row, !match);
        }
    });

    auto reset = new QPushButton("Restore Defaults", page);
    connect(reset, &QPushButton::clicked, this, [this]() {
        for (int row = 0; row < bindable_.size(); ++row)
            fillKeyEditors(row, bindable_[row]->property("defaultShortcuts").toStringList());
    });
    auto hint = new QLabel("Click a key box and press the new key; use the clear button to unbind.", page);
    auto bottom = new QHBoxLayout();
    bottom->addWidget(hint, 1);
    bottom->addWidget(reset);
    layout->addLayout(bottom);
    return page;
}

void PreferencesDialog::fillKeyEditors(int row, const QStringList& keys)
{
    for (int c = 0; c < kKeyColumns; ++c)
        keyEdits_[row][c]->setKeySequence(c < keys.size() ? QKeySequence(keys[c], QKeySequence::PortableText)
                                                          : QKeySequence());
}

// Validates (no key bound to two commands) and then applies + persists every
// action's shortcuts. Returns false, leaving everything untouched, on conflict.
bool PreferencesDialog::saveShortcuts()
{
    QList<QStringList> wanted;
    QMap<QString, int> owner; // key text -> first row using it
    for (int row = 0; row < bindable_.size(); ++row)
    {
        QStringList keys;
        for (QKeySequenceEdit* e : keyEdits_[row])
        {
            const QString t = e->keySequence().toString(QKeySequence::PortableText);
            if (t.isEmpty() || keys.contains(t)) continue;
            auto it = owner.constFind(t);
            if (it != owner.constEnd())
            {
                QMessageBox::warning(this, "Conflicting shortcut",
                                     QString("\"%1\" is assigned to both \"%2\" and \"%3\".")
                                         .arg(t, keyTable_->item(*it, 1)->text(), keyTable_->item(row, 1)->text()));
                return false;
            }
            owner.insert(t, row);
            keys << t;
        }
        wanted << keys;
    }

    auto s = settings();
    for (int row = 0; row < bindable_.size(); ++row)
    {
        QAction* a = bindable_[row];
        a->setShortcuts(fromTexts(wanted[row]));
        if (wanted[row] == a->property("defaultShortcuts").toStringList())
            s.remove(shortcutKey(a));
        else
            s.setValue(shortcutKey(a), wanted[row].join('\n'));
    }
    return true;
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
