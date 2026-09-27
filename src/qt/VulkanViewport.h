#pragma once
//
// VulkanViewport.h - QWidget embedding wiv's Vulkan/VSG rendering (via
// vsgQt::Window + GivViewer) as the central widget of MainWindow. Owns the
// full parse -> SceneBuilder -> camera-fit -> LabelPicker/BalloonController
// wiring that used to live inline in the old (pre-Qt) src/main.cpp, now
// packaged as loadFiles() so it can be re-run from File > Open as well as
// at startup.
//
#include "BalloonController.h"
#include "CaliperTool.h"
#include "GivScene.h"
#include "GivViewer.h"
#include "ImagePluginHost.h"
#include "LabelPicker.h"
#include "PanZoomHandler.h"
#include "SceneBuilder.h"

#include <vsgQt/Window.h>

#include <QColor>
#include <QResizeEvent>
#include <QSize>
#include <QWidget>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QScrollBar;
class QVBoxLayout;

namespace givqt
{

class VulkanViewport : public QWidget
{
    Q_OBJECT

public:
    // How many decoded images (each a full-resolution RGBA buffer, plus its
    // resident GPU textures) are kept around at once while paging through
    // $image references with next/previousImage() - see ImageCache. Small on
    // purpose: wiv only ever displays one at a time, this just avoids
    // re-decoding on every step back and forth.
    static constexpr size_t kImageCacheCapacity = 4;

    explicit VulkanViewport(QWidget* parent = nullptr);

    // Parses and displays `paths` (replacing whatever is currently shown).
    // Returns false (and sets *error, if non-null) on the first parse/build
    // failure. Safe to call again after the viewer is already running.
    bool loadFiles(const std::vector<std::string>& paths, QString* error = nullptr);

    void zoomIn();
    void zoomOut();
    void fitToWindow();

    // Spins the Qt event loop (bounded - never blocks indefinitely) until
    // window_->windowAdapter->extent2D() reflects this widget's actual
    // current size, or gives up. See its definition for why this is needed
    // before the very first fitToWindow() call after the window is shown.
    void ensureExtentSettled();
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

    // giv's do_auto_fit_marks: on (default) re-fits the view (fill, see
    // fitContentToWindow()) to each new image on next/previousImage(); off
    // preserves whatever zoom/pan the view was already at instead.
    void setAutoFit(bool enable);
    void toggleAutoFit() { setAutoFit(!autoFit_); }
    bool autoFit() const { return autoFit_; }

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

    // giv's 'a' key (do_no_transparency): forces every mark/line/fill
    // color's alpha to 1.0, ignoring its $color/alpha value. Takes effect
    // immediately, no scene rebuild needed.
    void toggleForceOpaque();
    bool forceOpaque() const { return forceOpaque_; }

    // Tools > Measure Distance Diagonal (giv's caliper tool) - see
    // CaliperTool.h.
    void toggleMeasureDistance();
    bool measureDistanceEnabled() const { return measureEnabled_; }

signals:
    void sceneLoaded();
    void cursorWorldPosition(double x, double y);
    void imageChanged(int index, int count, QString filename);
    void measurementChanged(QString text);

private:
    vsg::ref_ptr<vsg::WindowTraits> traits_;
    vsg::ref_ptr<GivViewer> viewer_;
    vsgQt::Window* window_ = nullptr; // owned by Qt (child of the container widget)
    QWidget* container_ = nullptr;
    vsg::ref_ptr<vsg::Options> options_;
    std::string shaderDir_;
    std::unique_ptr<giv::SceneBuilder> sceneBuilder_; // persistent, not recreated per rebuild - see rebuildSceneGraph()

    giv::SceneData scene_;
    bool hasScene_ = false;
    std::vector<giv::LoadedImage> loadedImages_; // 0 or 1 entries: only the currently-displayed image is ever decoded/resident (see decodeCurrentImage())
    bool globalShowMarks_ = true;
    bool forceOpaque_ = false;

    vsg::ref_ptr<vsg::Camera> camera_;
    vsg::ref_ptr<vsg::Orthographic> projection_;
    vsg::ref_ptr<vsg::View> mainView_; // persists across rebuilds - see rebuildSceneGraph()

    // vsg::RenderGraph::accept() maintains its *own* renderArea/viewportState
    // (separate from camera_->viewportState!), seeded once at construction
    // from camera_->getRenderArea() and the window's extent2D() *at that
    // moment*; on every later frame where the window's current extent
    // differs from that frozen baseline, it proportionally rescales the old
    // renderArea by the (possibly non-uniform, per-axis) ratio - entirely
    // bypassing whatever camera_->viewportState is set to afterwards. Kept
    // as a member (rather than rebuildSceneGraph()'s previous local
    // variable) so fitToBounds()/resizeEvent() can reset renderArea and its
    // resize-tracking baseline directly, instead of letting that proportional
    // rescale run (which is what was producing a non-isotropic stretch no
    // amount of camera-side fixing could touch).
    vsg::ref_ptr<vsg::RenderGraph> renderGraph_;
    vsg::ref_ptr<giv::PanZoomHandler> panZoom_;
    vsg::ref_ptr<giv::LabelPicker> labelPicker_;
    QLabel* balloonLabel_ = nullptr; // top-level Qt::ToolTip popup, see BalloonController.h
    vsg::ref_ptr<giv::BalloonController> balloonController_;

    vsg::ref_ptr<giv::CaliperTool> caliperTool_;
    vsg::ref_ptr<vsg::Font> caliperFont_;
    bool measureEnabled_ = false;

    // giv-style scrollbars around the render surface: horizontal/vertical
    // pan controls, ranged over the current content bounds and disabled
    // whenever the whole content already fits in the viewport.
    QScrollBar* hScrollBar_ = nullptr;
    QScrollBar* vScrollBar_ = nullptr;

    vsg::ref_ptr<vsg::Switch> imageSwitch_;
    std::vector<std::string> loadedImageNames_; // resolved paths of $image refs a plugin claims to support (not yet decoded - see ImagePluginHost::isSupported)
    std::optional<std::pair<double, double>> currentImageSize_; // (width, height) px of loadedImageNames_[currentImageIndex_], once decoded
    int currentImageIndex_ = 0;
    giv::ImageCache imageCache_{kImageCacheCapacity}; // bounds how many decoded images are resident at once while paging with next/previousImage()

    bool balloonEnabled_ = false;
    int lastWidth_ = 0;
    int lastHeight_ = 0;
    bool autoFit_ = true; // see setAutoFit()

    // See checkResizeSettling()/GivViewer::isResizeSettling's doc comment.
    bool resizeSettling_ = false;
    QSize pendingResizeExtent_;

    // "Contain": scales the content down to the smaller of scaleX/scaleY so
    // the whole thing is visible, possibly with letterbox margins on one axis.
    void fitToBounds(double minX, double minY, double maxX, double maxY);

    // Keeps camera_->viewportState AND renderGraph_'s own renderArea/
    // viewportState/previous_extent all matching `extent` - see
    // renderGraph_'s doc comment for why the latter needs it explicitly too
    // (vsg::RenderGraph::accept() otherwise proportionally rescales its own
    // renderArea from a stale baseline instead of snapping to the real
    // extent, which is what was causing the non-isotropic stretch).
    void syncRenderExtent(VkExtent2D extent);

    // Wired to viewer_->isResizeSettling: true while resizeEvent() has asked
    // window_ to resize but window_->windowAdapter->extent2D() hasn't caught
    // up to it yet (the resize is still pending, asynchronously - see
    // ensureExtentSettled()'s doc comment). Clears resizeSettling_ (and
    // returns false) the first time extent2D() matches pendingResizeExtent_.
    bool checkResizeSettling();

    // Shared tail of fitToWindow()/the initial-load and image-switch auto-fit:
    // reads currentFitBoundsYDown() and calls fitToBounds() with it.
    void fitContentToWindow();

    // Decodes loadedImageNames_[index] (via imageCache_) and, on success,
    // updates currentImageIndex_/loadedImages_/currentImageSize_ to match.
    // Does not rebuild the scene graph or touch the view - see
    // switchToImage() for the full next/previousImage() path.
    bool decodeImageAt(int index);

    // Initial-load counterpart of decodeImageAt(): decodes
    // loadedImageNames_[currentImageIndex_] (currently always 0), scanning
    // forward through the rest of the list on failure so one unreadable
    // file doesn't blank the whole load. Leaves loadedImages_/
    // currentImageSize_ empty if every candidate fails.
    void decodeCurrentImage();

    // nextImage()/previousImage()'s shared tail: decodes `index`, rebuilds
    // the scene graph so its texture actually replaces whatever was
    // previously displayed, and re-fits the view.
    void switchToImage(int index);

    // Fit bounds (in giv/image y-down space, i.e. before SceneBuilder's
    // Y-negation) for whatever should currently be visible: the parsed
    // dataset/mark bounds (scene_.min/maxX/Y), unioned with the pixel rect
    // of the currently-selected image only (not every loaded image - giv
    // only ever has one image resident at a time, so its own auto-fit never
    // sees the others). Falls back to a fixed -100..100 box if there's
    // nothing to fit at all.
    void currentFitBoundsYDown(double& minX, double& minY, double& maxX, double& maxY) const;

    // Resyncs hScrollBar_/vScrollBar_'s range/page-step/thumb-position from
    // the current content bounds and camera/projection state (giv's
    // update_adjustments()). Disables (and zeroes the range of) a scrollbar
    // whenever its axis already fits entirely in the viewport. Called after
    // every operation that changes the content bounds or the visible extent:
    // load, fit, zoom in/out, resize, and drag-pan/wheel-zoom (the latter via
    // PanZoomHandler::onViewChanged).
    void updateScrollBars();

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
