#pragma once
//
// MarkTreeView.h - the content widget of wiv's Mark Browser: a checkbox
// tree of every loaded dataset's $path hierarchy, letting the user show/hide
// individual marks or whole groups. Mirrors giv's GivMarkTreeDialog
// (giv-mark-tree-dialog.gob) - same three columns (Path / Visible / #nodes),
// same "$path segments split on '/' build nested groups" tree shape, same
// "toggling a node recursively sets every descendant to the same state"
// behavior - but as a plain QWidget rather than a dialog, so MainWindow can
// host it in either a standalone window or a docked side panel (see
// MainWindow::showMarkBrowser/setMarkBrowserPlacement).
//
#include <QWidget>

#include <memory>
#include <vector>

class QTreeWidget;
class QTreeWidgetItem;

namespace givqt
{

class VulkanViewport;

class MarkTreeView : public QWidget
{
    Q_OBJECT

public:
    explicit MarkTreeView(VulkanViewport* viewport, QWidget* parent = nullptr);

    // Rebuilds the tree from the viewport's current scene. Call after
    // construction and again whenever new files are loaded while the
    // browser may be open (matches giv's giv_mark_tree_dialog_update_tree_view).
    void rebuildFromScene();

private:
    struct Node
    {
        QTreeWidgetItem* item = nullptr;
        std::vector<size_t> datasetIndices; // every dataset in this node's subtree (aggregate)
        long long pointCount = 0;
    };

    VulkanViewport* viewport_;
    QTreeWidget* tree_ = nullptr;
    std::vector<std::unique_ptr<Node>> nodes_;
    bool updatingCheckState_ = false; // guards against reacting to our own setCheckState() calls

    void onItemChanged(QTreeWidgetItem* item, int column);
    void setSubtreeChecked(QTreeWidgetItem* item, bool checked);
};

} // namespace givqt
