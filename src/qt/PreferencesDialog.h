#pragma once
//
// PreferencesDialog.h - Edit > Preferences. Background color applies
// immediately (persisted via QSettings and re-applied to the live
// VulkanViewport); MSAA sample count is QSettings-persisted only and takes
// effect on next launch, since changing it means recreating the Vulkan
// swapchain/window, not attempted live.
//
#include <QColor>
#include <QDialog>
#include <QKeySequenceEdit>
#include <QList>

class QAction;
class QComboBox;
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

    // Overrides `action`'s shortcuts with the user's saved ones, if any
    // (QSettings "shortcuts/<objectName>", portable text joined by newlines;
    // an empty value means deliberately unbound).
    static void applySavedShortcuts(QAction* action);

signals:
    void backgroundColorChanged(const QColor& color);

private:
    QColor backgroundColor_;
    QPushButton* colorButton_ = nullptr;
    QComboBox* msaaCombo_ = nullptr;
    QList<QAction*> bindable_;
    QTableWidget* keyTable_ = nullptr;
    // keyEdits_[row][col]: the editors of table row `row`.
    QList<QList<QKeySequenceEdit*>> keyEdits_;

    QWidget* buildKeyboardTab();
    void fillKeyEditors(int row, const QStringList& keys);
    bool saveShortcuts();

    void pickColor();
    void save();
};

} // namespace givqt
