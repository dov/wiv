#include "LabelPicker.h"

#include "OffscreenTarget.h"

#include <cstring>

namespace giv
{

LabelPicker::LabelPicker(vsg::ref_ptr<vsg::Window> window, vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::Node> labelScene) :
    window_(window), device_(window->getOrCreateDevice()), camera_(camera), labelScene_(labelScene)
{
    switch_ = vsg::Switch::create();
    commandGraph_ = vsg::CommandGraph::create(window);
    commandGraph_->submitOrder = -1; // record/submit before the main display command graph
    commandGraph_->addChild(switch_);

    rebuild(window_->extent2D());
}

void LabelPicker::rebuild(const VkExtent2D& extent)
{
    extent_ = extent;
    if (extent_.width == 0 || extent_.height == 0) return;

    auto target = OffscreenTarget::create(device_, extent_);
    renderImageView_ = target.renderImageView;
    captureImage_ = target.captureImage;
    captureCommands_ = target.captureCommands;

    renderGraph_ = vsg::RenderGraph::create();
    renderGraph_->framebuffer = target.framebuffer;
    renderGraph_->renderArea.offset = {0, 0};
    renderGraph_->renderArea.extent = extent_;
    // Cleared to (0,0,0,1) so an untouched (background) pixel decodes to
    // label id -1 (see labelColorFor()'s +1 bias) - matches giv's
    // gdk_pixbuf_fill(w_label_image, 0x000000ff).
    renderGraph_->setClearValues(VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});

    // Reused across calls (resize *and* updateScene()) rather than recreated
    // every time: vsg::View hands out viewIDs from a free-list that only
    // grows if the previous holder hasn't been destructed yet, and every
    // per-view GPU resource (vsg::GraphicsPipeline::_implementation) is
    // sized against that ID at compile time - recreating this View on every
    // resize/rebuild was observed to eventually run the counter past what an
    // already-compiled pipeline elsewhere was sized for, crashing inside
    // GraphicsPipeline::vk(). Keeping one persistent View sidesteps that.
    if (!view_)
    {
        view_ = vsg::View::create(camera_); // shares the main camera, so the label render always matches what's on screen
    }
    else
    {
        view_->children.clear();
    }
    view_->addChild(labelScene_);
    renderGraph_->addChild(view_);

    // Added enabled: viewer->compile() (called once, before any 'b' press)
    // only visits/compiles a Switch child whose mask is on at the time of
    // that single traversal - a child added disabled would never get its
    // GraphicsPipeline/etc. actually compiled, and recording it later after
    // enabling would then silently draw nothing. main.cpp disables this
    // right after its viewer->compile() call.
    switch_->children.clear();
    switch_->addChild(true, renderGraph_);
    switch_->addChild(true, captureCommands_);
}

void LabelPicker::syncExtent(vsg::Viewer* viewer)
{
    auto extent = window_->extent2D();
    if (extent.width == 0 || extent.height == 0) return;
    if (extent.width == extent_.width && extent.height == extent_.height) return;

    // rebuild() below drops renderImageView_/captureImage_/renderGraph_'s
    // framebuffer (and, via createLabelFramebuffer(), a freshly-created
    // renderPass) - all of which this same frame's commandGraph_ (submitted
    // moments ago in this call's own recordAndSubmit(), since this runs from
    // GivViewer::render() right after present()) may still be executing on
    // the GPU. Dropping those ref_ptrs before that work retires destroys
    // them out from under an in-flight VkCommandBuffer - exactly the
    // vkDestroyFramebuffer/RenderPass/Image "still in use" validation errors
    // this was observed to produce (reproducibly: zoom, enable balloons,
    // move the mouse over a few, then Fit to Window, which is what actually
    // makes window_->extent2D() step over some off-by-one-frame rounding and
    // finally disagree with extent_ here). See
    // VulkanViewport::rebuildSceneGraph()'s identical fence-wait for the
    // same reasoning/VUIDs, applied there to the main scene graph.
    constexpr uint64_t kFenceTimeoutNs = 2'000'000'000; // 2 seconds
    for (size_t relativeFrameIndex = 0; relativeFrameIndex <= 2; ++relativeFrameIndex)
    {
        if (viewer->waitForFences(relativeFrameIndex, kFenceTimeoutNs) != VK_SUCCESS) return; // GPU not caught up yet - try again next frame rather than risk destroying in-flight resources
    }

    bool wasEnabled = !switch_->children.empty() && switch_->children.front().mask != vsg::MASK_OFF;
    rebuild(extent);
    // rebuild() replaces renderGraph_/captureCommands_ with new (uncompiled)
    // objects - compile them now, while the switch is enabled (see
    // rebuild()'s doc comment on why compiling behind a disabled Switch
    // silently produces empty draws).
    viewer->compile();
    setEnabled(wasEnabled);
}

void LabelPicker::updateScene(vsg::ref_ptr<vsg::Node> labelScene)
{
    labelScene_ = labelScene;
    rebuild(extent_);
}

void LabelPicker::updateCamera(vsg::ref_ptr<vsg::Camera> camera)
{
    camera_ = camera;
    if (view_) view_->camera = camera_;
}

void LabelPicker::setEnabled(bool enabled)
{
    switch_->setAllChildren(enabled);
}

int LabelPicker::pick(vsg::Viewer* viewer, int32_t x, int32_t y)
{
    if (!captureImage_ || x < 0 || y < 0 || static_cast<uint32_t>(x) >= extent_.width || static_cast<uint32_t>(y) >= extent_.height)
        return -1;

    constexpr uint64_t waitTimeout = 1000000000; // 1 second
    viewer->waitForFences(0, waitTimeout);

    VkImageSubresource subResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(*device_, captureImage_->vk(device_->deviceID), &subResource, &layout);

    auto deviceMemory = captureImage_->getDeviceMemory(device_->deviceID);

    void* mapped = nullptr;
    VkDeviceSize offset = layout.offset + static_cast<VkDeviceSize>(y) * layout.rowPitch + static_cast<VkDeviceSize>(x) * 4;
    // Map just the one pixel we need rather than the whole image.
    deviceMemory->map(offset, 4, 0, &mapped);
    uint8_t rgba[4];
    std::memcpy(rgba, mapped, 4);
    deviceMemory->unmap();

    // Inverse of labelColorFor() / giv's GivPainterAgg::label_to_color:
    // id+1 packed big-endian into RGB.
    uint32_t v = (static_cast<uint32_t>(rgba[0]) << 16) | (static_cast<uint32_t>(rgba[1]) << 8) | static_cast<uint32_t>(rgba[2]);
    return static_cast<int>(v) - 1;
}

} // namespace giv
