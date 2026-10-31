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
#include "Colormaps.h"
#include "GivScene.h"
#include "GivViewer.h"
#include "ImagePluginHost.h"
#include "LabelPicker.h"
#include "PanZoomHandler.h"
#include "ViewFlip.h"
#include "SceneBuilder.h"

#include <vsgQt/Window.h>

#include <QColor>
#include <QResizeEvent>
#include <QSize>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
    ~VulkanViewport() override;

    // Parses and displays `paths` (replacing whatever is currently shown).
    // Returns false (and sets *error, if non-null) on the first parse/build
    // failure. Safe to call again after the viewer is already running.
    bool loadFiles(const std::vector<std::string>& paths, QString* error = nullptr);

    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void zoomActualPixels();

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

    // Slice navigation (giv's plain Up/Down key, giv-win.cc's current_slice)
    // for multi-slice images (npy 3D arrays, FITS NAXIS=3, multi-frame
    // DICOM - see WivPluginImage::depth). No-ops if the current image has
    // fewer than 2 slices. Wraps around at either end.
    void nextSlice();
    void previousSlice();
    int sliceCount() const { return loadedImages_.empty() ? 1 : std::max(1, loadedImages_.front().depth); }
    int currentSlice() const { return currentSlice_; }

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
    // Multiplies every dataset's quiverScale by `factor` and rebuilds the
    // scene (giv's Alt+wheel quiver-arrow zoom).
    void scaleQuivers(double factor);

    bool eventFilter(QObject* watched, QEvent* event) override;

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

    // giv's 'g' key (do_show_grid): outlines every image pixel once the view
    // is zoomed in far enough. Takes effect immediately, no scene rebuild.
    void toggleShowPixelGrid();
    bool showPixelGrid() const { return showPixelGrid_; }

    // View > Horizontal/Vertical Flip, Vertical Zoom Lock, Image, Auto
    // Contrast (giv's gtk_image_viewer_set_flip/_set_vertical_lock/
    // _set_show_image and do_auto_contrast).
    void setFlipHorizontal(bool flip);
    void setFlipVertical(bool flip);
    void setVerticalZoomLock(bool lock);
    void setShowImage(bool show);
    void setAutoContrast(bool enable);
    bool autoContrast() const { return autoContrast_; }

    // Tools > Measure Distance Diagonal (giv's caliper tool) - see
    // CaliperTool.h.
    void toggleMeasureDistance();
    bool measureDistanceEnabled() const { return measureEnabled_; }

    // Tools > Calibrate Pixel Size - see CalibrateDialog.h. Reapplied to
    // caliperTool_ on every scene rebuild (it's recreated each time - see
    // rebuildSceneGraph()), so calibration survives across file loads/next-
    // previousImage() within one wiv session.
    void setPixelCalibration(double pixelSize, const std::string& unit);
    double pixelSize() const { return pixelSize_; }
    std::string pixelSizeUnit() const { return pixelSizeUnit_; }
    double lastMeasureDistancePixels() const { return caliperTool_ ? caliperTool_->lastDistancePixels() : 0.0; }
    std::optional<std::pair<double, double>> currentImageSize() const { return currentImageSize_; }

    // giv's status-bar pixel readout (giv-win.cc's on_motion_notify): given
    // the cursor's current world position (as reported via
    // cursorWorldPosition()), returns " [gray] = #XX" or " [ r g b] = #RRGGBB"
    // for the image pixel underneath it, or an empty string if no image is
    // loaded or the position falls outside its bounds.
    QString pixelValueText(double worldX, double worldY) const;

    // Tools > Adjust Contrast / Color Table - see ContrastDialog.h/
    // ColorTableDialog.h. Per-image state (giv's do_auto_contrast default:
    // stretched to the image's own sampleMin/sampleMax, colormap carried
    // forward from the last one the user picked - see lastColormapId_) the
    // first time each image path is seen, keyed by resolved path so it
    // survives next/previousImage() cycling and directory navigation.
    struct ContrastState
    {
        float min = 0.0f;
        float max = 0.0f;
        giv::colormaps::Id colormapId = giv::colormaps::Id::None;
        bool colormapEnabled = false;
    };

    // The raw (undisplayed) currently-decoded image - nullptr if none, or if
    // it has no raw sample buffer (sampleType == WIV_SAMPLE_NONE). Used by
    // ContrastDialog to compute the histogram and by MainWindow to decide
    // whether to enable the Contrast/Color Table tool actions.
    const giv::LoadedImage* currentImage() const { return loadedImages_.empty() ? nullptr : &loadedImages_.front(); }

    // Current contrast/colormap settings for the displayed image, defaulted
    // (and recorded) to sampleMin/sampleMax and the last-chosen colormap the
    // first time this image's path is seen.
    ContrastState currentContrastState();

    // Recomputes the displayed image (see DisplayImage.h) with the new
    // contrast window / colormap selection and rebuilds the scene graph so
    // the change is actually visible. No-ops if the current image has no
    // raw sample buffer.
    void setContrast(float min, float max);
    void setColormap(giv::colormaps::Id id, bool enabled);

    // 256-bucket histogram of the current image's raw samples, binned
    // against its own sampleMin/sampleMax (a fixed axis, independent of the
    // live contrast window - mirrors giv's giv_histo). Empty if the current
    // image has no raw sample buffer.
    std::array<uint32_t, 256> computeHistogram() const;

    // json-rpc giv_string command: parses `text` as a .giv scene-description
    // buffer (see GivParser::parseString()) and merges it into the current
    // scene. If `append` is false, existing datasets (marks) are cleared
    // first - the loaded image itself is untouched either way, matching
    // giv's own giv_widget_clear_giv(), which only ever clears marks. Returns
    // false (and sets *error, if non-null) on rebuild failure.
    bool applyGivString(const std::string& text, bool append, QString* error = nullptr);

    // json-rpc get_transformation/set_transformation: the current view's
    // scale (world-units-per-pixel^-1, i.e. pixels per world unit, matching
    // giv's convention) and world-space pan center, per axis.
    void getTransformation(double& scaleX, double& scaleY, double& shiftX, double& shiftY) const;
    void setTransformation(double scaleX, double scaleY, double shiftX, double shiftY);

signals:
    void sceneLoaded();
    // The contrast window or colormap of the displayed image changed.
    void contrastChanged();
    // Local files dropped onto the view (see DropOverlay in the .cpp).
    void filesDropped(QStringList files);
    void cursorWorldPosition(double x, double y);
    void imageChanged(int index, int count, QString filename);
    void measurementChanged(QString text);

    // Emitted on every left/middle/right button press, with the click's
    // world-space (x,y), the vsg button number (1/2/3), and Qt keyboard
    // modifiers - feeds the json-rpc pick_coordinate command (RpcServer).
    void clicked(double x, double y, int button, int modifiers);

    // Emitted whenever the current slice changes: on nextSlice()/
    // previousSlice(), and whenever decodeImageAt() (re)selects an image
    // (so a status bar can clear/refresh its "Slice N/D" text on next/
    // previousImage() and initial load too). `count` is 1 for an ordinary
    // 2D image.
    void sliceChanged(int slice, int count);

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

    // Set once rebuildSceneGraph() has completed a build for the first time.
    // Distinct from that call's own isInitialLoad parameter: isInitialLoad
    // means "(re)create the camera/projection and auto-fit to this load's
    // bounds", which is true for every full loadFiles() call (command-line,
    // File>Open, ...) - but the GPU can only be presenting a frame that needs
    // waitForGpuIdle()'s protection once a build has actually happened
    // before, which is only false for the constructor's own bootstrap call.
    bool everBuiltSceneGraph_ = false;
    std::vector<giv::LoadedImage> loadedImages_; // 0 or 1 entries: only the currently-displayed image is ever decoded/resident (see decodeCurrentImage())
    std::vector<giv::LoadedImage> displayImages_; // parallel to loadedImages_, but .rgba is the contrast/colormap-stretched buffer actually uploaded (see recomputeDisplayImage())
    std::unordered_map<std::string, ContrastState> contrastState_; // keyed by resolved image path, see currentContrastState()
    // The colormap selection last chosen via setColormap(), carried forward
    // as the default for any image path not yet in contrastState_ - so
    // navigating to a file never seen before (e.g. directory Next/Previous)
    // keeps the user's color table instead of resetting to None. min/max
    // deliberately aren't sticky the same way: each new image is still
    // auto-stretched to its own sampleMin/sampleMax, since that's data-range
    // dependent per file.
    giv::colormaps::Id lastColormapId_ = giv::colormaps::Id::None;
    bool lastColormapEnabled_ = false;
    bool globalShowMarks_ = true;
    bool forceOpaque_ = false;
    bool showPixelGrid_ = false;
    bool flipHorizontal_ = false;
    bool flipVertical_ = false;
    bool verticalZoomLock_ = false;
    bool showImage_ = true;
    bool autoContrast_ = true;
    float lastContrastMin_ = 0.0f;
    float lastContrastMax_ = 0.0f;
    bool hasLastContrast_ = false;
    vsg::ref_ptr<giv::FlipOrthographic> flipProjection_;

    // Re-applies showImage_ to imageSwitch_ (which is rebuilt with the scene).
    void applyShowImage();

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
    QTimer* zoomTimer_ = nullptr; // ticks panZoom_->advance() during a wheel-zoom motion
    vsg::ref_ptr<giv::LabelPicker> labelPicker_;
    QLabel* balloonLabel_ = nullptr; // top-level Qt::ToolTip popup, see BalloonController.h
    vsg::ref_ptr<giv::BalloonController> balloonController_;

    vsg::ref_ptr<giv::CaliperTool> caliperTool_;
    vsg::ref_ptr<vsg::Font> caliperFont_;
    bool measureEnabled_ = false;
    double pixelSize_ = 1.0; // see setPixelCalibration()
    std::string pixelSizeUnit_;

    // giv-style scrollbars around the render surface: horizontal/vertical
    // pan controls, ranged over the current content bounds and disabled
    // whenever the whole content already fits in the viewport.
    QScrollBar* hScrollBar_ = nullptr;
    QScrollBar* vScrollBar_ = nullptr;

    vsg::ref_ptr<vsg::Switch> imageSwitch_;
    std::vector<std::string> loadedImageNames_; // resolved paths of $image refs a plugin claims to support (not yet decoded - see ImagePluginHost::isSupported)
    std::optional<std::pair<double, double>> currentImageSize_; // (width, height) px of loadedImageNames_[currentImageIndex_], once decoded
    int currentImageIndex_ = 0;
    int currentSlice_ = 0; // see nextSlice()/previousSlice(); reset to 0 by decodeImageAt()
    giv::ImageCache imageCache_{kImageCacheCapacity}; // bounds how many decoded images are resident at once while paging with next/previousImage()

    bool balloonEnabled_ = false;
    int lastWidth_ = 0;
    int lastHeight_ = 0;
    bool autoFit_ = true; // see setAutoFit()
    // True while switchToImage() rebuilds the scene before re-fitting: the
    // interim scrollbar state (old zoom, new bounds) is never shown, avoiding a
    // show-then-hide flicker.
    bool deferScrollBarUpdate_ = false;

    // See checkResizeSettling()/GivViewer::isResizeSettling's doc comment.
    bool resizeSettling_ = false;
    QSize pendingResizeExtent_;
    int settleTicks_ = 0;

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

    // syncRenderExtent() plus rescaling the projection's visible extents to
    // `extent` at the current zoom - see its definition for why it is also
    // re-run once a pending resize settles.
    void applyExtent(VkExtent2D extent);

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

    // Recomputes displayImages_ from loadedImages_.front() and the current
    // image's ContrastState (see setContrast()/setColormap()). Called
    // whenever loadedImages_ changes (decodeImageAt()) or the contrast/
    // colormap settings change. Does not itself rebuild the scene graph.
    void recomputeDisplayImage();

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
    // Resyncs the camera/render graph to the native window's real size after
    // this widget or its container_ was resized.
    void handleViewportResize();

    // Shared tail of loadFiles()/setDatasetsVisible()/setShowMarks(): builds
    // the scene graph from the current scene_/loadedImages_ and (re)installs
    // it into the viewer. `isInitialLoad` controls whether the camera is
    // (re)created and auto-fit to the new bounds (a fresh file load) or left
    // untouched (a visibility-only rebuild), and whether the balloon overlay
    // is force-reset to hidden (new file) or kept as it was.
    bool rebuildSceneGraph(QString* error, bool isInitialLoad);

    // Waits (bounded, see rebuildSceneGraph()'s doc comment on why not
    // viewer_->deviceWaitIdle()) for the GPU to retire every frame that may
    // still be in flight, so it's safe to destroy/replace pipelines,
    // descriptor sets, etc. right after this returns true. Shared by
    // rebuildSceneGraph() and caliperTool_->onNeedsCompile - both call
    // viewer_->compile(), which does exactly that destroy/replace.
    bool waitForGpuIdle();
};

} // namespace givqt
