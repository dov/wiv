// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "MarkTreeView.h"

#include "VulkanViewport.h"

#include <QApplication>
#include <QFileInfo>
#include <QHeaderView>
#include <QPainter>
#include <QProxyStyle>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <unordered_map>

namespace givqt
{

namespace
{
// Qt scales the expand/collapse triangles with the indentation, so the narrow indent used here
// would shrink them. This draws them at a fixed, comfortable size inside the narrow slot instead.
class BigBranchStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override
    {
        if (element != PE_IndicatorBranch || !(option->state & State_Children))
        {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        const bool open = option->state & State_Open;
        const QPointF c = QRectF(option->rect).center();
        constexpr double kLong = 10.0, kShort = 6.0; // base and depth of the triangle
        QPolygonF tri;
        if (open) tri << QPointF(c.x() - kLong / 2, c.y() - kShort / 2) << QPointF(c.x() + kLong / 2, c.y() - kShort / 2) << QPointF(c.x(), c.y() + kShort / 2);
        else tri << QPointF(c.x() - kShort / 2, c.y() - kLong / 2) << QPointF(c.x() - kShort / 2, c.y() + kLong / 2) << QPointF(c.x() + kShort / 2, c.y());
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(option->palette.color((option->state & State_Selected) ? QPalette::HighlightedText : QPalette::Text));
        painter->drawPolygon(tri);
        painter->restore();
    }
};
} // namespace

MarkTreeView::MarkTreeView(VulkanViewport* viewport, QWidget* parent) : QWidget(parent), viewport_(viewport)
{
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({"Path", "Visible", "#nodes"});
    // Narrow nesting steps (the default is about 20 px; the branch arrows still fit); columns are fitted to their contents
    // in fitColumns() but stay user-resizable.
    tree_->setIndentation(6);
    auto* branchStyle = new BigBranchStyle(QApplication::style()->name());
    branchStyle->setParent(tree_); // owned by the tree; QWidget::setStyle does not take ownership
    tree_->setStyle(branchStyle);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);

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

        // Top-level node named after the dataset's source file, so a whole
        // file can be shown or hidden at once. giv keys its tree nodes by
        // "basename///path" too (so same-named $paths from different files
        // stay apart) but doesn't show the file as a node; we do.
        const std::string& source = ds.sourceFile;
        std::string fileLabel = source.empty() ? std::string("(unnamed)")
                                               : QFileInfo(QString::fromStdString(source)).fileName().toStdString();
        Node* fileNode = getOrCreateNode(source + "///", nullptr, fileLabel);
        fileNode->datasetIndices.push_back(i);
        fileNode->pointCount += static_cast<long long>(ds.pointCount());
        if (ds.isVisible) fileNode->anyVisible = true;

        QTreeWidgetItem* parentItem = fileNode->item;
        std::string cumulative; // path below the file node
        Node* leafNode = nullptr;
        for (const auto& segment : segments)
        {
            cumulative = cumulative.empty() ? segment : cumulative + "/" + segment;
            Node* node = getOrCreateNode(source + "///" + cumulative, parentItem, segment);
            node->datasetIndices.push_back(i);
            node->pointCount += static_cast<long long>(ds.pointCount());
            parentItem = node->item;
            leafNode = node;
        }
        if (leafNode) leafNode->item->setCheckState(1, ds.isVisible ? Qt::Checked : Qt::Unchecked);
    }

    for (auto& node : nodes_) node->item->setText(2, QString::number(node->pointCount));
    // A file node is unchecked only if every dataset in it starts hidden ($hide).
    for (auto& node : nodes_)
        if (!node->item->parent() && !node->anyVisible) node->item->setCheckState(1, Qt::Unchecked);

    tree_->expandAll();
    updatingCheckState_ = false;
    fitColumns();
}

void MarkTreeView::fitColumns()
{
    // Each column is as wide as its longest entry (header included); an over-long Path is
    // cut at kMaxPathWidth - the user can still drag it wider.
    constexpr int kMaxPathWidth = 280;
    for (int c = 0; c < tree_->columnCount(); ++c) tree_->resizeColumnToContents(c);
    if (tree_->columnWidth(0) > kMaxPathWidth) tree_->setColumnWidth(0, kMaxPathWidth);
    fittedPathWidth_ = tree_->columnWidth(0);
    fillExtraWidth();
    updateGeometry();
}

void MarkTreeView::fillExtraWidth()
{
    // Spare room (a dock wider than the contents) goes to Path rather than an empty strip on the right.
    int others = 0;
    for (int c = 1; c < tree_->columnCount(); ++c) others += tree_->columnWidth(c);
    tree_->setColumnWidth(0, std::max(fittedPathWidth_, tree_->viewport()->width() - others));
}

void MarkTreeView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    fillExtraWidth();
}

QSize MarkTreeView::sizeHint() const
{
    int w = 2 * tree_->frameWidth() + 4;
    for (int c = 0; c < tree_->columnCount(); ++c) w += tree_->columnWidth(c);
    return QSize(w + tree_->verticalScrollBar()->sizeHint().width(), QWidget::sizeHint().height());
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
