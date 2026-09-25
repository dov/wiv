#pragma once
//
// DatasetPanel.h - QDockWidget contents listing the datasets of the
// currently-loaded scene (name, color swatch, point count). Double-clicking
// a row asks VulkanViewport to fit the camera to that dataset's own bounds.
//
// Per-dataset show/hide is intentionally not offered here: wiv's
// SceneBuilder batches every dataset of a given primitive kind (marks,
// lines, fills) into one shared instanced draw across the whole scene (see
// SceneBuilder.h), so there is no per-dataset node to toggle without a
// larger SceneBuilder redesign.
//
#include "GivScene.h"

#include <QWidget>

class QTreeWidget;

namespace givqt
{

class DatasetPanel : public QWidget
{
    Q_OBJECT

public:
    explicit DatasetPanel(QWidget* parent = nullptr);

    void setScene(const giv::SceneData& scene);

signals:
    void datasetFocused(int index);

private:
    QTreeWidget* tree_ = nullptr;
    const giv::SceneData* scene_ = nullptr;
};

} // namespace givqt
