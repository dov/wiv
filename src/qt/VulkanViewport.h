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
};

} // namespace givqt
