#include "PreferencesDialog.h"

#include <QGuiApplication>
#include <QStyleHints>

#include "ExternalApps.h"
#include "ImagePluginHost.h"
#include "InstalledApps.h"

#include <QAction>
#include <QColorDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>

#include <algorithm>
#include <optional>
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

MouseSettings PreferencesDialog::loadMouseSettings()
{
    auto s = settings();
    MouseSettings m;
    m.wheel = s.value("mouse/wheel", 0).toInt() == 1 ? MouseSettings::Wheel::Navigate : MouseSettings::Wheel::Zoom;
    const int sb = std::clamp(s.value("mouse/sideButtons", 0).toInt(), 0, 2);
    m.sideButtons = static_cast<MouseSettings::SideButtons>(sb);
    m.middleClick = s.value("mouse/middleClick", 0).toInt() == 1 ? MouseSettings::MiddleClick::FullScreen
                                                                 : MouseSettings::MiddleClick::Nothing;
    m.leftDragPans = s.value("mouse/leftDragPans", false).toBool();
    m.swapSideButtons = s.value("mouse/swapSideButtons", false).toBool();
    m.tiltNavigates = s.value("mouse/tiltNavigates", true).toBool();
    m.doubleClickToggles = s.value("mouse/doubleClickToggles", true).toBool();
    return m;
}

int PreferencesDialog::loadTheme()
{
    return std::clamp(settings().value("view/theme", 0).toInt(), 0, 2);
}

void PreferencesDialog::applyTheme()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const int t = loadTheme();
    QGuiApplication::styleHints()->setColorScheme(t == 1 ? Qt::ColorScheme::Light
                                                  : t == 2 ? Qt::ColorScheme::Dark
                                                           : Qt::ColorScheme::Unknown);
#endif
}

NavigationView PreferencesDialog::loadNavigationView()
{
    auto s = settings();
    return static_cast<NavigationView>(std::clamp(s.value("view/navigation", 0).toInt(), 0, 2));
}

void PreferencesDialog::saveNavigationView(NavigationView v)
{
    auto s = settings();
    s.setValue("view/navigation", static_cast<int>(v));
}

int PreferencesDialog::loadMsaaSamples()
{
    auto s = settings();
    return s.value("msaaSamples", 4).toInt();
}

namespace
{
constexpr int kKeyColumns = 3; // most built-in commands have up to three alternative keys

// A dialog listing the image applications installed on the system; the chosen
// one, or nothing if cancelled.
std::optional<InstalledApp> pickInstalledApp(QWidget* parent)
{
    const QList<InstalledApp> apps = installedImageApps();
    QDialog dlg(parent);
    dlg.setWindowTitle("Choose an Application");
    auto* layout = new QVBoxLayout(&dlg);
    auto* filter = new QLineEdit(&dlg);
    filter->setPlaceholderText("Filter...");
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto* list = new QListWidget(&dlg);
    list->setIconSize(QSize(24, 24));
    for (const InstalledApp& a : apps) list->addItem(new QListWidgetItem(a.icon, a.name));
    layout->addWidget(list, 1);
    if (apps.isEmpty()) layout->addWidget(new QLabel("The system lists no image applications; use Add Custom or Browse.", &dlg));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    QObject::connect(filter, &QLineEdit::textChanged, &dlg, [list](const QString& text) {
        for (int i = 0; i < list->count(); ++i) list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    dlg.resize(380, 460);
    if (dlg.exec() != QDialog::Accepted || list->currentRow() < 0) return std::nullopt;
    return apps[list->currentRow()];
}

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

    navViewCombo_ = new QComboBox(this);
    navViewCombo_->addItem("Fit every image to the window", static_cast<int>(NavigationView::Fit));
    navViewCombo_->addItem("Remember zoom and pan per file", static_cast<int>(NavigationView::PerFile));
    navViewCombo_->addItem("Keep the current zoom and pan", static_cast<int>(NavigationView::Keep));
    navViewCombo_->setCurrentIndex(navViewCombo_->findData(static_cast<int>(loadNavigationView())));
    form->addRow("When switching images:", navViewCombo_);

    themeCombo_ = new QComboBox(this);
    themeCombo_->addItem("Follow the system", 0);
    themeCombo_->addItem("Light", 1);
    themeCombo_->addItem("Dark", 2);
    themeCombo_->setCurrentIndex(loadTheme());
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
    themeCombo_->setEnabled(false);
#endif
    form->addRow("Theme:", themeCombo_);

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
    tabs->addTab(buildMouseTab(), "Mouse");
    tabs->addTab(buildExternalAppsTab(), "External Apps");
    tabs->addTab(buildKeyboardTab(), "Keyboard");

    auto layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
    resize(560, 520);
    setLayout(layout);
}

QWidget* PreferencesDialog::buildMouseTab()
{
    const MouseSettings m = loadMouseSettings();
    auto page = new QWidget(this);
    auto form = new QFormLayout();

    wheelCombo_ = new QComboBox(page);
    wheelCombo_->addItem("Zoom (Ctrl+wheel always zooms)", static_cast<int>(MouseSettings::Wheel::Zoom));
    wheelCombo_->addItem("Next / previous image", static_cast<int>(MouseSettings::Wheel::Navigate));
    wheelCombo_->setCurrentIndex(wheelCombo_->findData(static_cast<int>(m.wheel)));
    form->addRow("Mouse wheel:", wheelCombo_);

    middleClickCombo_ = new QComboBox(page);
    middleClickCombo_->addItem("Nothing", static_cast<int>(MouseSettings::MiddleClick::Nothing));
    middleClickCombo_->addItem("Toggle full screen", static_cast<int>(MouseSettings::MiddleClick::FullScreen));
    middleClickCombo_->setCurrentIndex(middleClickCombo_->findData(static_cast<int>(m.middleClick)));
    form->addRow("Wheel click:", middleClickCombo_);

    leftDragCheck_ = new QCheckBox("Left-drag pans the image (not while measuring)", page);
    leftDragCheck_->setChecked(m.leftDragPans);
    form->addRow("", leftDragCheck_);

    sideButtonsCombo_ = new QComboBox(page);
    sideButtonsCombo_->addItem("Next / previous image", static_cast<int>(MouseSettings::SideButtons::Navigate));
    sideButtonsCombo_->addItem("Step zoom in / out", static_cast<int>(MouseSettings::SideButtons::StepZoom));
    sideButtonsCombo_->addItem("Nothing", static_cast<int>(MouseSettings::SideButtons::Nothing));
    sideButtonsCombo_->setCurrentIndex(sideButtonsCombo_->findData(static_cast<int>(m.sideButtons)));
    form->addRow("Back / forward buttons:", sideButtonsCombo_);

    swapSideCheck_ = new QCheckBox("Swap back and forward buttons", page);
    swapSideCheck_->setChecked(m.swapSideButtons);
    form->addRow("", swapSideCheck_);

    tiltCheck_ = new QCheckBox("Tilt wheel left / right navigates images", page);
    tiltCheck_->setChecked(m.tiltNavigates);
    form->addRow("", tiltCheck_);

    dblClickCheck_ = new QCheckBox("Double-click toggles actual size and fit", page);
    dblClickCheck_->setChecked(m.doubleClickToggles);
    form->addRow("", dblClickCheck_);

    auto layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(new QLabel("Middle-drag pans (left-drag too, if enabled), right-drag zooms, Shift+wheel zooms faster and Alt+wheel "
                                 "scales quivers, as in giv."));
    layout->addStretch();
    return page;
}

QWidget* PreferencesDialog::buildExternalAppsTab()
{
    const ExternalApps::Config cfg = ExternalApps::load();
    auto page = new QWidget(this);
    auto layout = new QVBoxLayout(page);

    auto browse = [this](QLineEdit* edit) {
        const QString exe = QFileDialog::getOpenFileName(this, "Choose Application");
        if (exe.isEmpty()) return;
        edit->setText(exe.contains(' ') ? '"' + exe + '"' : exe);
    };

    // Open With list: up to kMaxApps named applications, Ctrl+1... by default.
    layout->addWidget(new QLabel("Open With applications (File > Open With; Ctrl+1, Ctrl+2... by default):", page));
    extAppTable_ = new QTableWidget(0, 2, page);
    extAppTable_->setHorizontalHeaderLabels({"Menu label", "Application"});
    extAppTable_->verticalHeader()->setVisible(false);
    extAppTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    extAppTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    extAppTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    extAppTable_->setMaximumHeight(150);
    const QList<InstalledApp> known = installedImageApps();
    for (const ExternalApps::App& a : cfg.apps) addTableRow(extAppTable_, a.name, a.command, iconForCommand(a.command, known));
    layout->addWidget(extAppTable_);
    auto appFromSystem = new QPushButton("Add from System...", page);
    connect(appFromSystem, &QPushButton::clicked, this, [this]() {
        if (extAppTable_->rowCount() >= ExternalApps::kMaxApps) return;
        if (const auto app = pickInstalledApp(this)) addTableRow(extAppTable_, app->name, app->command, app->icon);
    });
    auto appAdd = new QPushButton("Add Custom", page);
    connect(appAdd, &QPushButton::clicked, this, [this]() {
        if (extAppTable_->rowCount() >= ExternalApps::kMaxApps) return;
        addTableRow(extAppTable_, "", "");
        extAppTable_->editItem(extAppTable_->item(extAppTable_->rowCount() - 1, 0));
    });
    auto appBrowse = new QPushButton("Browse...", page);
    connect(appBrowse, &QPushButton::clicked, this, [this, browse]() {
        const int row = extAppTable_->currentRow();
        if (row < 0) return;
        QLineEdit tmp;
        browse(&tmp);
        if (tmp.text().isEmpty()) return;
        extAppTable_->item(row, 1)->setText(tmp.text());
        extAppTable_->item(row, 0)->setIcon(iconForCommand(tmp.text(), installedImageApps()));
        if (extAppTable_->item(row, 0)->text().isEmpty())
            extAppTable_->item(row, 0)->setText(QFileInfo(tmp.text().remove('"')).completeBaseName());
    });
    auto appRemove = new QPushButton("Remove", page);
    connect(appRemove, &QPushButton::clicked, this, [this]() {
        if (extAppTable_->currentRow() >= 0) extAppTable_->removeRow(extAppTable_->currentRow());
    });
    auto appButtons = new QHBoxLayout();
    appButtons->addWidget(appFromSystem);
    appButtons->addWidget(appAdd);
    appButtons->addWidget(appBrowse);
    appButtons->addWidget(appRemove);
    appButtons->addStretch();
    layout->addLayout(appButtons);

    extDefaultEdit_ = new QLineEdit(cfg.defaultCommand, page);
    extDefaultEdit_->setPlaceholderText("Empty: the system's default application");
    auto defaultBrowse = new QPushButton("Browse...", page);
    connect(defaultBrowse, &QPushButton::clicked, this, [this, browse]() { browse(extDefaultEdit_); });
    auto defaultSystem = new QPushButton("From System...", page);
    connect(defaultSystem, &QPushButton::clicked, this, [this]() {
        if (const auto app = pickInstalledApp(this)) extDefaultEdit_->setText(app->command);
    });
    auto defaultRow = new QHBoxLayout();
    defaultRow->addWidget(new QLabel("Default application:", page));
    defaultRow->addWidget(extDefaultEdit_, 1);
    defaultRow->addWidget(defaultSystem);
    defaultRow->addWidget(defaultBrowse);
    layout->addLayout(defaultRow);

    layout->addWidget(new QLabel("Overrides by file type (the longest matching extension wins):", page));
    extRuleTable_ = new QTableWidget(0, 2, page);
    extRuleTable_->setHorizontalHeaderLabels({"Extensions", "Application"});
    extRuleTable_->verticalHeader()->setVisible(false);
    extRuleTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    extRuleTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    extRuleTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const ExternalApps::Rule& r : cfg.rules) addExternalRuleRow(r.extensions, r.command);
    layout->addWidget(extRuleTable_, 1);

    auto add = new QPushButton("Add", page);
    connect(add, &QPushButton::clicked, this, [this]() {
        addExternalRuleRow("", "");
        extRuleTable_->editItem(extRuleTable_->item(extRuleTable_->rowCount() - 1, 0));
    });
    auto ruleSystem = new QPushButton("From System...", page);
    connect(ruleSystem, &QPushButton::clicked, this, [this]() {
        const int row = extRuleTable_->currentRow();
        if (row < 0) return;
        if (const auto app = pickInstalledApp(this)) extRuleTable_->item(row, 1)->setText(app->command);
    });
    auto ruleBrowse = new QPushButton("Browse...", page);
    connect(ruleBrowse, &QPushButton::clicked, this, [this, browse]() {
        const int row = extRuleTable_->currentRow();
        if (row < 0) return;
        QLineEdit tmp;
        browse(&tmp);
        if (!tmp.text().isEmpty()) extRuleTable_->item(row, 1)->setText(tmp.text());
    });
    auto remove = new QPushButton("Remove", page);
    connect(remove, &QPushButton::clicked, this, [this]() {
        if (extRuleTable_->currentRow() >= 0) extRuleTable_->removeRow(extRuleTable_->currentRow());
    });
    auto buttons = new QHBoxLayout();
    buttons->addWidget(add);
    buttons->addWidget(ruleSystem);
    buttons->addWidget(ruleBrowse);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addLayout(buttons);

    QStringList formatList;
    for (const std::string& f : giv::ImagePluginHost::formatNames()) formatList << '@' + QString::fromStdString(f);
    const QString formats = formatList.join(", ");
    auto hint = new QLabel("File > Open in External App (E) starts the application chosen below by the file's type; "
                           "File > Open With starts one of the listed applications. "
                           "Write a command like  gimp \"%f\"  - %f is the file; without it the file is added "
                           "as the last argument. Types are comma separated: extensions such as  jpg, nii.gz  or, for "
                           "files without an extension, a format written with @, such as  @dicom. "
                           "An empty application opens the file with the system default.\n\nFormats: " +
                               formats,
                           page);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    return page;
}

void PreferencesDialog::addTableRow(QTableWidget* table, const QString& a, const QString& b, const QIcon& icon)
{
    const int row = table->rowCount();
    table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(icon, a));
    table->setItem(row, 1, new QTableWidgetItem(b));
}

void PreferencesDialog::addExternalRuleRow(const QString& extensions, const QString& command)
{
    addTableRow(extRuleTable_, extensions, command);
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
    s.setValue("view/theme", themeCombo_->currentData().toInt());
    s.setValue("view/navigation", navViewCombo_->currentData().toInt());
    s.setValue("mouse/wheel", wheelCombo_->currentData().toInt());
    s.setValue("mouse/sideButtons", sideButtonsCombo_->currentData().toInt());
    s.setValue("mouse/middleClick", middleClickCombo_->currentData().toInt());
    s.setValue("mouse/leftDragPans", leftDragCheck_->isChecked());
    s.setValue("mouse/swapSideButtons", swapSideCheck_->isChecked());
    s.setValue("mouse/tiltNavigates", tiltCheck_->isChecked());
    s.setValue("mouse/doubleClickToggles", dblClickCheck_->isChecked());
    ExternalApps::Config apps;
    apps.defaultCommand = extDefaultEdit_->text().trimmed();
    for (int row = 0; row < extAppTable_->rowCount(); ++row)
    {
        const QString command = extAppTable_->item(row, 1)->text().trimmed();
        QString name = extAppTable_->item(row, 0)->text().trimmed();
        if (command.isEmpty()) continue;
        if (name.isEmpty()) name = command;
        apps.apps.append({name, command});
    }
    for (int row = 0; row < extRuleTable_->rowCount(); ++row)
    {
        const QString ext = extRuleTable_->item(row, 0)->text().trimmed();
        if (!ext.isEmpty()) apps.rules.append({ext, extRuleTable_->item(row, 1)->text().trimmed()});
    }
    ExternalApps::save(apps);
    applyTheme();
    emit mouseSettingsChanged(loadMouseSettings());
    emit navigationViewChanged(loadNavigationView());
}

} // namespace givqt
