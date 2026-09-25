#include "DatasetPanel.h"

#include <QHeaderView>
#include <QPixmap>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace givqt
{

DatasetPanel::DatasetPanel(QWidget* parent) : QWidget(parent)
{
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({"Dataset", "Color", "Points"});
    tree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tree_->setRootIsDecorated(false);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);
    setLayout(layout);

    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        int index = item->data(0, Qt::UserRole).toInt();
        emit datasetFocused(index);
    });
}

void DatasetPanel::setScene(const giv::SceneData& scene)
{
    scene_ = &scene;
    tree_->clear();

    for (size_t i = 0; i < scene.datasets.size(); ++i)
    {
        const auto& ds = scene.datasets[i];
        auto item = new QTreeWidgetItem(tree_);
        item->setText(0, ds.pathName.empty() ? QString("dataset %1").arg(i) : QString::fromStdString(ds.pathName));
        item->setData(0, Qt::UserRole, static_cast<int>(i));

        QPixmap swatch(16, 16);
        swatch.fill(QColor::fromRgbF(ds.color.r, ds.color.g, ds.color.b, ds.color.a));
        item->setIcon(1, QIcon(swatch));

        item->setText(2, QString::number(ds.pointCount()));

        tree_->addTopLevelItem(item);
    }
}

} // namespace givqt
