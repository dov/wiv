#pragma once
//
// VulkanViewport.h - QWidget embedding wiv's Vulkan/VSG rendering (via
// vsgQt::Window + GivViewer) as the central widget of MainWindow. Owns the
// full parse -> SceneBuilder -> camera-fit -> LabelPicker/BalloonController
// wiring that used to live inline in the old (pre-Qt) src/main.cpp, now
// packaged as loadFiles() so it can be re-run from File > Open as well as
// at startup.
//
#include "ImageStateStore.h"
#include <memory>
#include "BalloonController.h"
#include "CaliperTool.h"
#include "Colormaps.h"
#include "GivScene.h"
#include "GivViewer.h"
#include "ImagePluginHost.h"
#include "LabelPicker.h"
#include "MouseSettings.h"
#include "PanZoomHandler.h"
#include "ViewFlip.h"
#include "SceneBuilder.h"

#include <functional>
#include <vsgQt/Window.h>

#include <QColor>
#include <QImage>
#include <QResizeEvent>
#include <QSize>
#include <QTimer>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QPointF>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QLabel;
class QScrollBar;
class QWheelEvent;
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
    static constexpr size_t kImageCacheCapacity = 8;
    static constexpr size_t kPreviewCacheCapacity = 24;

    explicit VulkanViewport(QWidget* parent = nullptr);
    ~VulkanViewport() override;

    // Parses and displays `paths` (replacing whatever is currently shown).
    // Returns false (and sets *error, if non-null) on the first parse/build
    // failure. Safe to call again after the viewer is already running.
    bool loadFiles(const std::vector<std::string>& paths, QString* error = nullptr);

    void zoomIn();
    void zoomOut();
    void fitToWindow();
    // Like fitToWindow(), but glides there from the present view (pan and
    // zoom together) instead of jumping - for the user-triggered fits (menu,
    // key, double-click, step zoom onto the fit stop). fitToWindow() stays
    // instant for the first fit after the window is shown.
    void fitToWindowAnimated();
    void zoomActualPixels();

    // Steps to the next zoom stop beyond the current scale: the fit scale,
    // 1:1 and 4:1 (de-duplicated, sorted), zooming about the view's centre.
    // Does nothing past the outermost stop. Stepping onto the fit stop
    // re-fits (and so re-centres) the content.
    void stepZoomIn();

    // Call just before a deliberate window-size change (e.g. full screen):
    // the visible world region is then kept - the image rescales with the
    // window - instead of the default of keeping the scale and revealing
    // more or less of the scene. Lapses shortly after the resize settles.
    void preserveViewOnNextResize(bool entering);
    void stepZoomOut();

    // Double-click: 1:1 anchored at `devicePos` (device pixels in the
    // window), or - if already at 1:1 - back to fit.
    void toggleActualFit(QPointF devicePos);

    // View > Remember View Per File: when on, the pan/zoom you left a file
    // at (if you changed it from the fit) is restored when you come back to
    // it - whether by next/previous image or by loading it again. Off, the
    // views are forgotten.
    void setRememberViewPerFile(bool enable);
    bool rememberViewPerFile() const { return rememberView_; }

    void setMouseSettings(const MouseSettings& m) { mouse_ = m; }

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

    // Hands the decode cache an image decoded off-thread (see
    // MainWindow::prefetchNeighbours()) so showing it later skips the decode.
    void cachePrefetchedImage(const std::string& path, giv::LoadedImage image);
    void retainCachedImages(const std::unordered_set<std::string>& keepFull, const std::unordered_set<std::string>& keepPreview)
    {
        imageCache_.retain(keepFull);
        previewCache_.retain(keepPreview);
    }
    // Low-resolution previews decoded ahead of time by the prefetcher (see
    // ImagePluginHost::loadPreview()); cheap enough to show while keys are
    // being held down. A file known to have none is remembered too.
    void cachePrefetchedPreview(const std::string& path, giv::LoadedImage preview) { previewCache_.put(path, 0, std::move(preview)); }
    void markNoPreview(const std::string& path) { noPreview_.insert(path); }
    bool hasPreviewOrNone(const std::string& path) const { return previewCache_.contains(path) || noPreview_.count(path); }
    // After stepping stops: swaps the preview for the full image if that is
    // already cached. Returns false if it is not (the caller must decode it).
    bool upgradePreviewFromCache();
    bool isImageCached(const std::string& path) const { return imageCache_.contains(path); }
    // Jump to the first / last of the file's own images.
    void firstImage();
    void lastImage();
    // Pans the view by a fraction of its size (keyboard pan): +x moves the
    // view right, +y moves it down, as on screen.
    void panByFraction(double fx, double fy);
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

    // One navigation for everything a file can hold several of (View > Next/
    // Previous Page): the pages of a multi-page TIFF (no wrap-around), else
    // the slices of a volume (DICOM, FITS, npy; wraps), else the $image
    // references of a multi-image .giv (wraps). No-ops for a plain image.
    void nextPage() { stepPart(+1); }
    void previousPage() { stepPart(-1); }
    bool canStepPart(int dir) const;
    void stepPart(int dir);
    int pageCount() const { return loadedImages_.empty() ? 1 : std::max(1, loadedImages_.front().pageCount); }
    int currentPage() const { return currentPage_; }

    void setBackgroundColor(const QColor& color);

    // giv's do_auto_fit_marks: on (default) re-fits the view (fill, see
    // fitContentToWindow()) to each new image on next/previousImage(); off
    // preserves whatever zoom/pan the view was already at instead.
    void setAutoFit(bool enable);
    bool autoFit() const { return autoFit_; }
    void toggleAutoFit() { setAutoFit(!autoFit_); }

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
    // View > Rotate Left/Right: turns the view a quarter turn about its
    // centre. Re-fits unless the user has zoomed/panned; scrollbars are
    // hidden while rotated.
    void rotateLeft() { rotateView(1); }
    void rotateRight() { rotateView(3); }
    void setVerticalZoomLock(bool lock);
    void setShowImage(bool show);
    void setAutoContrast(bool enable);
    bool autoContrast() const { return autoContrast_; }

    // Tools > Measure Distance Diagonal (giv's caliper tool) - see
    // CaliperTool.h.
    bool measureDistanceEnabled() const { return measureEnabled_; }
    // Diagonal / Horizontal / Vertical (giv's three Measure Distance tools).
    // Selecting a mode turns the caliper on in it; selecting the active mode
    // again turns it off, as in giv's menu_measure_distance().
    void toggleMeasureMode(giv::MeasureMode mode);
    // Switches the tool on in `mode` (or just changes the mode if already on).
    void setMeasureMode(giv::MeasureMode mode);
    void setMeasureEnabled(bool enabled); // keeps the current mode
    giv::MeasureMode measureMode() const { return measureMode_; }

    // Tools > Calibrate Pixel Size - see CalibratePanel.h. Reapplied to
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
    // (nullptr while only a reduced-resolution preview is up, so nothing
    // reads its pixels or dimensions as if they were the real image's.)
    const giv::LoadedImage* currentImage() const
    {
        return loadedImages_.empty() || loadedImages_.front().isPreview() ? nullptr : &loadedImages_.front();
    }

    // The image as currently shown (slice, contrast window and colour table
    // applied), scaled to fit maxSize; null if no full image is displayed.
    QImage currentViewImage(int maxSize);

    // True while the displayed image is a quick low-resolution preview
    // standing in for a full decode that has not finished yet; the owner
    // should decode currentImageName() in the background and hand the result
    // to upgradePreview().
    bool isShowingPreview() const { return !loadedImages_.empty() && loadedImages_.front().isPreview(); }
    // Replaces the preview with the full decode, keeping the view as is.
    // No-op unless a preview of `path` is what is currently displayed.
    void upgradePreview(const std::string& path, giv::LoadedImage full);

    // --- Animation (GIF, APNG, animated WebP, AVIF sequences) ---------------
    // Once the displayed file is the full image (not a preview) and nothing
    // has changed for a moment, the owner calls startAnimationIfAny(): it
    // decodes every frame on a worker thread and, if the file turns out to be
    // an animation, starts playing it. Frames are composited full-canvas RGBA
    // that replace the image's pixels in place (no scene rebuild per frame).
    // Idempotent; a no-op for stills, multi-page files and over-budget files.
    void startAnimationIfAny();
    const giv::Animation* animation() const { return anim_.get(); }
    // True if the scene has $frame sections (mark animation). The animation
    // controls then drive those frames, and an animated $image is not played.
    bool hasMarkFrames() const { return hasScene_ && scene_.frameCount > 0; }
    bool hasAnimation() const { return anim_ != nullptr || hasMarkFrames(); }
    bool animationPlaying() const { return animPlaying_; }
    int animationFrame() const { return animFrame_; }
    int animationFrameCount() const
    {
        return anim_ ? static_cast<int>(anim_->frameCount()) : (hasMarkFrames() ? scene_.frameCount : 0);
    }
    double animationSpeed() const { return animSpeed_; }
    bool animationLooping() const { return animLoop_; }
    void toggleAnimationPlay();       // play <-> pause (at the end of a non-looping run: play again from the start)
    void stopAnimation();             // pause and go back to the first frame
    void rewindAnimation();           // go to the first frame, keeping the play/pause state
    void restartAnimation();          // go to the first frame and play
    void seekAnimationFrame(int frame); // scrubber: pauses and shows `frame`
    void stepAnimationFrame(int dir); // pauses, then shows the next/previous frame (wraps)
    void setAnimationSpeed(double speed);
    void scaleAnimationSpeed(double factor) { setAnimationSpeed(animSpeed_ * factor); }
    void setAnimationLooping(bool loop);

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
    // `wholeVolume`: of every slice of a multi-slice image instead of just the
    // current one (the same as the default for a single-slice image).
    std::array<uint32_t, 256> computeHistogram(bool wholeVolume = false) const;

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

    // View to render in exportImage(). Kinds that don't carry their own scale
    // and center derive them from the content or the on-screen view.
    struct ExportView
    {
        enum class Kind
        {
            Fit,          // whole content, fit into the export size
            Current,      // the on-screen view's scale (pixels per unit) and center
            ActualPixels, // 1 pixel per world unit, centered on the content
            Explicit,     // `scale` pixels per world unit centered on world (centerX, centerY), as set_transformation
        };
        Kind kind = Kind::Fit;
        double scale = 1.0, centerX = 0.0, centerY = 0.0;
    };

    // Renders the scene off-screen at width x height pixels (independent of
    // the window size) and saves it to `path`; the format follows the file
    // extension (whatever QImage can write: png, jpg, bmp, ...). The
    // Measure Distance overlay is not included. Must run on the GUI thread
    // with the window shown. Returns false and sets *error on failure.
    bool exportImage(const QString& path, int width, int height, const ExportView& view, QString* error = nullptr);

signals:
    void sceneLoaded();
    // The animation's state changed (frame shown, play/pause, speed, or one
    // was found/dropped: count is 0 when the current image has none).
    void animationChanged(int frame, int count, bool playing, double speed);
    // The contrast window or colormap of the displayed image changed.
    void contrastChanged();
    // Local files dropped onto the view (see DropOverlay in the .cpp).
    void filesDropped(QStringList files);
    void cursorWorldPosition(double x, double y);
    void imageChanged(int index, int count, QString filename);
    void measurementChanged(QString text);
    // The Measure Distance tool was switched on/off or changed mode (`mode` is a giv::MeasureMode).
    void measureModeChanged(bool enabled, int mode);

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

    // Emitted when the displayed page of a multi-page file changes, and
    // whenever a (new) image is selected, like sliceChanged. `count` is 1
    // for a single-page image.
    void pageChanged(int page, int count);

    // A horizontal two-finger touchpad swipe: +1 for next image (swipe left),
    // -1 for previous.
    void swipeNavigate(int direction);

    // A wheel click (Preferences > Mouse > Wheel click: toggle full screen).
    void toggleFullScreenRequested();

private:
    vsg::ref_ptr<vsg::WindowTraits> traits_;
    vsg::ref_ptr<GivViewer> viewer_;
    vsgQt::Window* window_ = nullptr; // owned by Qt (child of the container widget)
    // clip_ is the cell in the layout; container_ (the native render window) is a
    // child of it kept at a fixed, screen-sized height and masked down to clip_'s
    // size. Growing or shrinking clip_ (thumbnail strip, splitter drags) then
    // never resizes the native window or its swapchain, which is what left a
    // black, unpresented area behind. See layoutRenderWindow().
    QWidget* clip_ = nullptr;
    bool maskShrinkPending_ = false;
    bool layoutSettled_ = false; // false while the window is first being shown, see layoutRenderWindow()
    bool layoutSettleTimerStarted_ = false;
    int maskW_ = 0, maskH_ = 0; // size of the mask currently applied
    int visW_ = 0, visH_ = 0;   // visible size the camera works in (see applyMask())
    void applyMask();
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
    std::string contrastKey() const;
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
    int quarterTurns_ = 0; // counter-clockwise, 0..3
    void rotateView(int quarters);
    // The content size as seen on screen: width and height swap at an odd
    // number of quarter turns.
    void rotatedSize(double& w, double& h) const;
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
    vsg::ref_ptr<vsg::Group> sceneGraph_;               // the display scene; also drawn off-screen by exportImage()
    vsg::ref_ptr<vsg::CommandGraph> mainCommandGraph_;  // the window's command graph
    float exportWorldPerPixel_ = 0.0f;                  // >0 only while exportImage() renders its frame
    vsg::ref_ptr<giv::PanZoomHandler> panZoom_;
    QTimer* zoomTimer_ = nullptr; // ticks panZoom_->advance() during a wheel-zoom motion
    vsg::ref_ptr<giv::LabelPicker> labelPicker_;
    QLabel* balloonLabel_ = nullptr; // top-level Qt::ToolTip popup, see BalloonController.h
    vsg::ref_ptr<giv::BalloonController> balloonController_;

    vsg::ref_ptr<giv::CaliperTool> caliperTool_;
    giv::MeasureMode measureMode_ = giv::MeasureMode::Diagonal;
    void applyMeasureState(); // pushes measureEnabled_/measureMode_ to the caliper tool and signals
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
    // Animation playback (see startAnimationIfAny()). anim_ is the decoded
    // animation of the file on screen, or null.
    static constexpr size_t kAnimationBudgetBytes = size_t(768) << 20;
    std::shared_ptr<const giv::Animation> anim_;
    std::string animPath_;       // file anim_ (or an in-flight/failed decode) belongs to
    int animFrame_ = 0;
    int animPlaysDone_ = 0;
    bool animPlaying_ = false;
    bool animLoop_ = true;
    double animSpeed_ = 1.0;
    int animTextureDirty_ = 0;   // frames left to re-dirty the texture for (see onAnimationFrameHook())
    vsg::ref_ptr<vsg::ubvec4Array2D> animTexture_;
    vsg::ref_ptr<vsg::vec4Array> sampleParams_; // raw-sample image: window + colormap uniforms (see SceneBuilder::sampleParams())
    vsg::ref_ptr<vsg::Data> sampleTexture_;     // raw-sample volume: the live slice texture
    QTimer* animTimer_ = nullptr;
    QElapsedTimer animClock_;
    qint64 animDueMs_ = 0;       // animClock_ time the next frame is due
    std::shared_ptr<std::atomic<int>> animCancel_;
    QFutureWatcher<std::shared_ptr<const giv::Animation>>* animWatcher_ = nullptr;
    void startMarkAnimation();   // (re)starts playback of the scene's $frame sections
    int animationDelayMs(int frame) const;
    // $image per $frame (see SceneData::frameImage): per frame, the index into
    // loadedImageNames_ in effect (-1 = leave as is), and per loaded image the
    // frame to show of a multi-frame file (-1 = the default first frame).
    std::vector<int> markFrameImage_;
    std::vector<int> loadedImageFrameSel_;
    std::shared_ptr<const giv::Animation> selAnim_;
    std::string selAnimPath_;
    int animationLoopCount() const; // plays through the frames; 0 = forever
    void resetAnimation();       // drops everything animation-related (navigating away, reload)
    void onAnimationDecoded(const std::shared_ptr<const giv::Animation>& anim);
    void showAnimationFrame(int frame);
    void scheduleNextAnimationFrame();
    void advanceAnimation();
    bool onAnimationFrameHook();
    void emitAnimationChanged();

    int currentPage_ = 0; // see gotoPage(); reset to 0 by decodeImageAt()
    void gotoPage(int page);
    int currentSlice_ = 0; // see nextSlice()/previousSlice(); reset to 0 by decodeImageAt()
    giv::ImageCache previewCache_{kPreviewCacheCapacity}; // small decodes, see cachePrefetchedPreview()
    std::unordered_set<std::string> noPreview_;
    giv::ImageCache imageCache_{kImageCacheCapacity}; // bounds how many decoded images are resident at once while paging with next/previousImage()

    bool balloonEnabled_ = false;
    // Manual window resizing (see applyExtent()): a view that is not a "fit"
    // keeps what part of the image it shows by scaling with the widget. The
    // scale is always derived from the state when the resize began - not
    // accumulated step by step - so dragging back restores the view exactly,
    // and the size used is this widget's, so a scrollbar appearing or
    // disappearing (which only resizes the canvas inside it) does not rescale.
    WId bitGravityWindow_ = 0; // native window whose X11 bit gravity keepContentsInPlaceOnResize() has set
    void keepContentsInPlaceOnResize();
    struct ResizeReference
    {
        bool valid = false;
        QSize widgetPx;      // widget size, device pixels, when the resize began
        double scale = 0.0;  // pixels per world unit at that moment
    } resizeRef_;
    QSize lastWidgetPx_;           // widget size (device px) at the end of the last applyExtent()
    double resizeAppliedScale_ = 0.0; // scale the last applyExtent() produced; a different current scale means the view was changed since
    int lastWidth_ = 0;
    bool preserveView_ = false; // see preserveViewOnNextResize()
    double preserveHalfW_ = 0.0, preserveHalfH_ = 0.0;
    // Makes the preserve reversible: a "contain" fit of the region loses
    // scale whenever the aspect ratio differs, so repeated toggles would
    // shrink the image. The scale from before the first toggle is kept, and
    // restored by the next one if the view has not been changed meanwhile.
    double restoreScale_ = 0.0;       // scale from before entering the larger window, or 0
    double appliedScale_ = 0.0;       // scale the last preserved resize produced
    double preserveTarget_ = 0.0;     // fixed scale for this resize, or 0 to fit the saved region
    int preserveGen_ = 0;             // invalidates the lapse timer of an earlier toggle
    int lastHeight_ = 0;
    bool autoFit_ = true; // see setAutoFit()

    // ---- Zoom stops / per-file view memory ----
    // Device pixels per world unit along X (1.0 == 1:1), and the same for the
    // fit-to-window ("contain") view of the current content.
    double viewScale() const;
    double fitScale() const;
    void stepZoom(bool in);

    // The view a fit produces (see computeFitView()), and the animation that
    // glides the camera there.
    struct FitView
    {
        double centerX, centerY, halfW, halfH;
    };
    bool computeFitView(FitView& out) const;
    struct ViewAnimation
    {
        bool active = false;
        FitView from{}, to{};
        double duration = 0.0;
        bool refit = true;    // end with an exact fit (else: end exactly at `to`)
        double rotFrom = 0.0; // animated rotation: extra angle at the start, eased to 0
        QElapsedTimer clock;
    } viewAnim_;
    QTimer* viewAnimTimer_ = nullptr;
    void startViewAnimation(const FitView& to, double rotFrom = 0.0, bool refit = true);
    void tickViewAnimation();
    void cancelViewAnimation();

    // Keyboard pan glides: the pixel offset still to travel is applied in
    // eased steps from a timer, through the same pan() a mouse drag uses.
    struct PanAnimation
    {
        bool active = false;
        double totalX = 0.0, totalY = 0.0; // offset to cover, in drag pixels
        int appliedX = 0, appliedY = 0;
        double duration = 0.0;
        QElapsedTimer clock;
    } panAnim_;
    QTimer* panAnimTimer_ = nullptr;
    void tickPanAnimation();

    // True once the user (not a fit or a window resize) has changed the view
    // since the last fit; only such views are worth remembering.
    bool viewDirty_ = false;
    bool rememberView_ = false;
    struct SavedView
    {
        double centerX, centerY;
        double scaleX, scaleY; // device pixels per world unit, so a resized window keeps the zoom
    };
    std::map<std::string, SavedView> savedViews_;
    // Per-image contrast/colormap/view kept between runs (state.db next to the
    // settings file); created on first use.
    std::unique_ptr<giv::ImageStateStore> stateStore_;
    giv::ImageStateStore* stateStore();
    void rememberSlice();
    std::vector<std::string> scenePaths_; // the files the scene was loaded from
    std::string viewKeyFallback_; // first loaded path, for scenes with no image of their own
    std::string viewKey() const;
    void saveCurrentView();

public:
    // Writes the displayed file's remembered view out; for shutdown.
    void saveViewState() { saveCurrentView(); }

private:
    bool tryRestoreView();

    // Horizontal touchpad swipe -> swipeNavigate(); see handleSwipe().
    bool handleSwipe(QWheelEvent* we);
    MouseSettings mouse_;
    QPointF middlePressPos_;
    int wheelNavAccum_ = 0; // angleDelta accumulated toward one 120-unit notch
    int tiltAccum_ = 0;
    double swipeAccum_ = 0.0;
    bool swipeFired_ = false;
    QElapsedTimer swipeClock_;
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

    // The part of the (taller) native window that is actually shown: what the
    // camera, projection and mouse mapping work in. Equals extent2D() whenever
    // the window is not taller than clip_.
    VkExtent2D viewportExtent() const;
    // Sizes container_ and masks the native window to clip_'s area.
    void layoutRenderWindow();

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
    bool decodeImageAt(int index, int page = 0, bool allowPreview = true);

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
    bool applyDisplayImage(QString* error);
    bool applySlice(QString* error);

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
