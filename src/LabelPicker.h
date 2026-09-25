#pragma once
//
// LabelPicker.h - off-screen render + CPU readback used by wiv's 'b'
// balloon/tooltip toggle to find which dataset is under the mouse cursor.
//
// Mirrors giv's own approach (giv-widget.gob: w_label_image / GivRenderer
// with do_paint_by_index): render the whole scene a second time, once per
// frame, with every dataset painted in a unique flat, non-antialiased color
// (see SceneBuilder::labelGraph()/labelColorFor), then read back a single
// pixel at the current mouse position and decode it back into a dataset
// index. Unlike giv (single-threaded CPU/AGG rendering, so the label image
// is always exactly as fresh as the last paint), this renders on the GPU via
// a second small command graph and blocks on that frame's fence before
// reading the result back - see LabelPicker::pick()'s doc comment for the
// one-frame-latency consequence of that.
//
#include <vsg/all.h>

namespace giv
{

class LabelPicker : public vsg::Inherit<vsg::Object, LabelPicker>
{
public:
    // `window` supplies the device and (initial) extent to size the
    // off-screen target to; `camera` is shared with the main view so the
    // label render is always pixel-for-pixel aligned with what's on screen;
    // `labelScene` is SceneBuilder::labelGraph()'s output.
    LabelPicker(vsg::ref_ptr<vsg::Window> window, vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::Node> labelScene);

    // Command graph to add alongside the main display command graph in
    // viewer->assignRecordAndSubmitTaskAndPresentation({...}). Always
    // records (cheaply skips via the internal Switch) when disabled.
    vsg::ref_ptr<vsg::CommandGraph> commandGraph() const { return commandGraph_; }

    // Enables/disables the off-screen render. Cheap to call every frame.
    void setEnabled(bool enabled);

    // Call once per frame, after present(), if enabled. Blocks until the
    // GPU work submitted this frame completes (matching giv's own fully
    // synchronous paint-then-pick model), maps the just-rendered label
    // image, and returns the decoded 0-based dataset index at window pixel
    // (x,y), or -1 if none (background, or out of bounds). Because this
    // reads back *this* frame's label render only after it has already been
    // presented, the picked value lags the mouse's true position by one
    // frame - imperceptible at interactive frame rates for a hover tooltip.
    int pick(vsg::Viewer* viewer, int32_t x, int32_t y);

    // Keeps the off-screen target's size matching the window; call once per
    // frame (cheap no-op when the extent hasn't changed). Re-invokes
    // viewer->compile() on a real resize, since rebuild() replaces the
    // framebuffer/pipelines wholesale with freshly-constructed (uncompiled)
    // ones - see the comment in rebuild().
    void syncExtent(vsg::Viewer* viewer);

private:
    vsg::ref_ptr<vsg::Window> window_;
    vsg::ref_ptr<vsg::Device> device_;
    vsg::ref_ptr<vsg::Camera> camera_;
    vsg::ref_ptr<vsg::Node> labelScene_;

    VkExtent2D extent_{0, 0};
    vsg::ref_ptr<vsg::ImageView> renderImageView_; // optimal-tiled color attachment, written by the label render pass
    vsg::ref_ptr<vsg::Image> captureImage_;        // linear-tiled, host-visible copy destination
    vsg::ref_ptr<vsg::Commands> captureCommands_;  // copies renderImageView_ -> captureImage_

    vsg::ref_ptr<vsg::RenderGraph> renderGraph_;
    vsg::ref_ptr<vsg::View> view_;
    vsg::ref_ptr<vsg::Switch> switch_;
    vsg::ref_ptr<vsg::CommandGraph> commandGraph_;

    void rebuild(const VkExtent2D& extent);
};

} // namespace giv
