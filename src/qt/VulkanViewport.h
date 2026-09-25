#pragma once
//
// VulkanViewport.h - QWidget embedding wiv's Vulkan/VSG rendering (via
// vsgQt::Window + GivViewer) as the central widget of MainWindow. Owns the
// full parse -> SceneBuilder -> camera-fit -> LabelPicker/BalloonOverlay/
// BalloonController wiring that used to live inline in the old (pre-Qt)
// src/main.cpp, now packaged as loadFiles() so it can be re-run from
// File > Open as well as at startup.
//
#include "BalloonController.h"
#include "BalloonOverlay.h"
#include "GivScene.h"
#include "GivViewer.h"
#include "ImagePluginHost.h"
#include "LabelPicker.h"
#include "PanZoomHandler.h"
#include "SceneBuilder.h"

#include <vsgQt/Window.h>

#include <QColor>
#include <QResizeEvent>
#include <QWidget>

#include <string>
#include <vector>

class QVBoxLayout;

namespace givqt
{

class VulkanViewport : public QWidget
{
    Q_OBJECT

public:
    explicit VulkanViewport(QWidget* parent = nullptr);

    // Parses and displays `paths` (replacing whatever is currently shown).
    // Returns false (and sets *error, if non-null) on the first parse/build
    // failure. Safe to call again after the viewer is already running.
    bool loadFiles(const std::vector<std::string>& paths, QString* error = nullptr);

    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void toggleBalloon();
    bool balloonEnabled() const;

    // $image cycling (giv's shift-Up/shift-Down). No-ops if there are fewer
    // than 2 successfully-loaded images.
    void nextImage();
    void previousImage();
    int imageCount() const { return static_cast<int>(loadedImageNames_.size()); }
    int currentImageIndex() const { return currentImageIndex_; }
    std::string currentImageName() const;

    void setBackgroundColor(const QColor& color);
    void setAutoFitMarginPx(double px);

    const giv::SceneData& sceneData() const { return scene_; }
    bool hasScene() const { return hasScene_; }

    // Master "View Marks" switch (giv's 'm' key / do_show_marks): hides or
    // shows every mark/line/fill/text overlay regardless of each dataset's
    // own isVisible state (which the Mark Browser tree controls) - the two
    // are combined (AND) only at scene-build time, so toggling this back on
    // restores whatever the tree had set. Camera/pan state is preserved.
    void setShowMarks(bool show);
    void toggleShowMarks();
    bool showMarks() const { return globalShowMarks_; }

    // Sets the persisted isVisible flag for each of `indices` (indices into
    // sceneData().datasets) and rebuilds the scene graph in place. Used by
    // the Mark Browser tree.
    void setDatasetsVisible(const std::vector<size_t>& indices, bool visible);

signals:
    void sceneLoaded();
    void cursorWorldPosition(double x, double y);
    void frameStats(double fps);
    void imageChanged(int index, int count, QString filename);

private:
    vsg::ref_ptr<vsg::WindowTraits> traits_;
    vsg::ref_ptr<GivViewer> viewer_;
    vsgQt::Window* window_ = nullptr; // owned by Qt (child of the container widget)
    QWidget* container_ = nullptr;
    vsg::ref_ptr<vsg::Options> options_;
    std::string shaderDir_;

    giv::SceneData scene_;
    bool hasScene_ = false;
    std::vector<giv::LoadedImage> loadedImages_; // kept around so visibility-only rebuilds can redraw $image quads too
    bool globalShowMarks_ = true;

    vsg::ref_ptr<vsg::Camera> camera_;
    vsg::ref_ptr<vsg::Orthographic> projection_;
    vsg::ref_ptr<giv::PanZoomHandler> panZoom_;
    vsg::ref_ptr<giv::LabelPicker> labelPicker_;
    vsg::ref_ptr<giv::BalloonOverlay> balloonOverlay_;
    vsg::ref_ptr<giv::BalloonController> balloonController_;

    vsg::ref_ptr<vsg::Switch> imageSwitch_;
    std::vector<std::string> loadedImageNames_; // resolved paths of successfully-loaded $image refs
    int currentImageIndex_ = 0;

    double autoFitMarginPx_ = 10.0;
    bool balloonEnabled_ = false;
    int lastWidth_ = 0;
    int lastHeight_ = 0;

    void fitToBounds(double minX, double minY, double maxX, double maxY);
    void resizeEvent(QResizeEvent* event) override;

    // Shared tail of loadFiles()/setDatasetsVisible()/setShowMarks(): builds
    // the scene graph from the current scene_/loadedImages_ and (re)installs
    // it into the viewer. `isInitialLoad` controls whether the camera is
    // (re)created and auto-fit to the new bounds (a fresh file load) or left
    // untouched (a visibility-only rebuild), and whether the balloon overlay
    // is force-reset to hidden (new file) or kept as it was.
    bool rebuildSceneGraph(QString* error, bool isInitialLoad);
};

} // namespace givqt
