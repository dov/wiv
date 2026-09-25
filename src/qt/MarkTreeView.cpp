#include "MarkTreeView.h"

#include "VulkanViewport.h"

#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <unordered_map>

namespace givqt
{

MarkTreeView::MarkTreeView(VulkanViewport* viewport, QWidget* parent) : QWidget(parent), viewport_(viewport)
{
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({"Path", "Visible", "#nodes"});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree_->header()->resizeSection(0, 220);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    tree_->header()->resizeSection(2, 60);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);

    connect(tree_, &QTreeWidget::itemChanged, this, &MarkTreeView::onItemChanged);
}

void MarkTreeView::rebuildFromScene()
{
    updatingCheckState_ = true;
    tree_->clear();
    nodes_.clear();

    std::unordered_map<std::string, Node*> pathToNode;

    auto getOrCreateNode = [&](const std::string& fullPath, QTreeWidgetItem* parentItem, const std::string& label) -> Node* {
        auto it = pathToNode.find(fullPath);
        if (it != pathToNode.end()) return it->second;

        auto node = std::make_unique<Node>();
        QTreeWidgetItem* item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(tree_);
        item->setText(0, QString::fromStdString(label));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(1, Qt::Checked); // giv's default for a freshly-created (group) node
        item->setTextAlignment(2, Qt::AlignRight);

        node->item = item;
        Node* raw = node.get();
        item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<void*>(raw)));
        nodes_.push_back(std::move(node));
        pathToNode.emplace(fullPath, raw);
        return raw;
    };

    const auto& scene = viewport_->sceneData();
    for (size_t i = 0; i < scene.datasets.size(); ++i)
    {
        const auto& ds = scene.datasets[i];

        std::vector<std::string> segments;
        if (ds.pathName.empty())
        {
            segments.push_back(("dataset " + std::to_string(i)));
        }
        else
        {
            size_t start = 0;
            while (true)
            {
                size_t slash = ds.pathName.find('/', start);
                segments.push_back(ds.pathName.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
        }

        QTreeWidgetItem* parentItem = nullptr;
        std::string cumulative;
        Node* leafNode = nullptr;
        for (const auto& segment : segments)
        {
            cumulative = cumulative.empty() ? segment : cumulative + "/" + segment;
            Node* node = getOrCreateNode(cumulative, parentItem, segment);
            node->datasetIndices.push_back(i);
            node->pointCount += static_cast<long long>(ds.pointCount());
            parentItem = node->item;
            leafNode = node;
        }
        if (leafNode) leafNode->item->setCheckState(1, ds.isVisible ? Qt::Checked : Qt::Unchecked);
    }

    for (auto& node : nodes_) node->item->setText(2, QString::number(node->pointCount));

    tree_->expandAll();
    updatingCheckState_ = false;
}

void MarkTreeView::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (updatingCheckState_ || column != 1) return;

    bool checked = item->checkState(1) == Qt::Checked;

    updatingCheckState_ = true;
    setSubtreeChecked(item, checked);
    updatingCheckState_ = false;

    auto* node = static_cast<Node*>(item->data(0, Qt::UserRole).value<void*>());
    if (node) viewport_->setDatasetsVisible(node->datasetIndices, checked);
}

void MarkTreeView::setSubtreeChecked(QTreeWidgetItem* item, bool checked)
{
    item->setCheckState(1, checked ? Qt::Checked : Qt::Unchecked);
    for (int i = 0; i < item->childCount(); ++i) setSubtreeChecked(item->child(i), checked);
}

} // namespace givqt
