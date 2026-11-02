#pragma once
//
// PreferencesDialog.h - Edit > Preferences. Background color applies
// immediately (persisted via QSettings and re-applied to the live
// VulkanViewport); MSAA sample count is QSettings-persisted only and takes
// effect on next launch, since changing it means recreating the Vulkan
// swapchain/window, not attempted live.
//
#include "MouseSettings.h"

#include <QColor>
#include <QDialog>
#include <QIcon>
#include <QKeySequenceEdit>
#include <QList>

class QAction;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QTableWidget;
class QPushButton;

namespace givqt
{

class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    // `bindable` are the actions listed in the Keyboard tab; each carries a
    // "defaultShortcuts" (QStringList, portable text) and "menuTitle" property.
    explicit PreferencesDialog(const QList<QAction*>& bindable, QWidget* parent = nullptr);

    QColor backgroundColor() const { return backgroundColor_; }
    int msaaSamples() const;

    // Reads current values from QSettings ("wiv", "wiv").
    static QColor loadBackgroundColor();
    static int loadMsaaSamples();
    static MouseSettings loadMouseSettings();
    static NavigationView loadNavigationView();

    // Colour theme: 0 = follow the system, 1 = light, 2 = dark. Applies the
    // saved choice to the application (Qt 6.8+; a no-op before that).
    static int loadTheme();
    static void applyTheme();
    static void saveNavigationView(NavigationView v);

    // Overrides `action`'s shortcuts with the user's saved ones, if any
    // (QSettings "shortcuts/<objectName>", portable text joined by newlines;
    // an empty value means deliberately unbound).
    static void applySavedShortcuts(QAction* action);

signals:
    void backgroundColorChanged(const QColor& color);
    void mouseSettingsChanged(const MouseSettings& m);
    void navigationViewChanged(NavigationView v);

private:
    QColor backgroundColor_;
    QPushButton* colorButton_ = nullptr;
    QComboBox* msaaCombo_ = nullptr;
    QComboBox* navViewCombo_ = nullptr;
    QComboBox* wheelCombo_ = nullptr;
    QComboBox* middleClickCombo_ = nullptr;
    QCheckBox* leftDragCheck_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QComboBox* sideButtonsCombo_ = nullptr;
    QCheckBox* swapSideCheck_ = nullptr;
    QCheckBox* tiltCheck_ = nullptr;
    QCheckBox* dblClickCheck_ = nullptr;
    QList<QAction*> bindable_;
    QTableWidget* keyTable_ = nullptr;
    QLineEdit* extDefaultEdit_ = nullptr;
    QTableWidget* extRuleTable_ = nullptr;
    QTableWidget* extAppTable_ = nullptr;
    // keyEdits_[row][col]: the editors of table row `row`.
    QList<QList<QKeySequenceEdit*>> keyEdits_;

    QWidget* buildKeyboardTab();
    QWidget* buildMouseTab();
    QWidget* buildExternalAppsTab();
    void addTableRow(QTableWidget* table, const QString& a, const QString& b, const QIcon& icon = {});
    void addExternalRuleRow(const QString& extensions, const QString& command);
    void fillKeyEditors(int row, const QStringList& keys);
    bool saveShortcuts();

    void pickColor();
    void save();
};

} // namespace givqt
