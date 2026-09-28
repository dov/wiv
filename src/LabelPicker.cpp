#include "LabelPicker.h"

#include <cstring>

namespace giv
{

namespace
{

constexpr VkFormat kLabelFormat = VK_FORMAT_R8G8B8A8_UNORM;

bool supportsBlit(vsg::ref_ptr<vsg::Device> device, VkFormat format)
{
    auto physicalDevice = device->getPhysicalDevice();
    VkFormatProperties srcProps, dstProps;
    vkGetPhysicalDeviceFormatProperties(*physicalDevice, format, &srcProps);
    vkGetPhysicalDeviceFormatProperties(*physicalDevice, kLabelFormat, &dstProps);
    return ((srcProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0) &&
           ((dstProps.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0);
}

vsg::ref_ptr<vsg::ImageView> createRenderImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent)
{
    auto image = vsg::Image::create();
    image->format = kLabelFormat;
    image->extent = VkExtent3D{extent.width, extent.height, 1};
    image->mipLevels = 1;
    image->arrayLayers = 1;
    image->samples = VK_SAMPLE_COUNT_1_BIT; // no MSAA - label picking needs exact, unblended pixels
    image->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return vsg::createImageView(device, image, VK_IMAGE_ASPECT_COLOR_BIT);
}

// Host-visible, linearly-tiled destination for the post-render copy - see
// my-vsg-qt-viewer.cpp's createCaptureImage (this is the same screenshot-
// style two-image pattern: optimal-tiled render target -> copy -> linear
// host-visible image -> map).
vsg::ref_ptr<vsg::Image> createCaptureImage(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent)
{
    auto image = vsg::Image::create();
    image->format = kLabelFormat;
    image->extent = {extent.width, extent.height, 1};
    image->arrayLayers = 1;
    image->mipLevels = 1;
    image->tiling = VK_IMAGE_TILING_LINEAR;
    image->usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image->compile(device);

    auto memReqs = image->getMemoryRequirements(device->deviceID);
    auto memFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto deviceMemory = vsg::DeviceMemory::create(device, memReqs, memFlags);
    image->bind(deviceMemory, 0);
    return image;
}

vsg::ref_ptr<vsg::Commands> createCaptureCommands(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Image> srcImage,
                                                   vsg::ref_ptr<vsg::Image> dstImage)
{
    auto commands = vsg::Commands::create();

    auto toDst = vsg::ImageMemoryBarrier::create(
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        dstImage, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
    commands->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, toDst));

    if (srcImage->format == dstImage->format && srcImage->extent.width == dstImage->extent.width &&
        srcImage->extent.height == dstImage->extent.height)
    {
        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = dstImage->extent;

        auto copyImage = vsg::CopyImage::create();
        copyImage->srcImage = srcImage;
        copyImage->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copyImage->dstImage = dstImage;
        copyImage->dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copyImage->regions.push_back(region);
        commands->addChild(copyImage);
    }
    else if (supportsBlit(device, dstImage->format))
    {
        VkImageBlit region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.srcOffsets[1] = VkOffset3D{static_cast<int32_t>(srcImage->extent.width), static_cast<int32_t>(srcImage->extent.height), 1};
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.dstOffsets[1] = VkOffset3D{static_cast<int32_t>(dstImage->extent.width), static_cast<int32_t>(dstImage->extent.height), 1};

        auto blit = vsg::BlitImage::create();
        blit->srcImage = srcImage;
        blit->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        blit->dstImage = dstImage;
        blit->dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        blit->regions.push_back(region);
        blit->filter = VK_FILTER_NEAREST;
        commands->addChild(blit);
    }
    else
    {
        throw std::runtime_error("wiv: GPU does not support the image copy/blit needed for balloon label picking");
    }

    auto toReadable = vsg::ImageMemoryBarrier::create(
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        dstImage, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
    commands->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, toReadable));

    return commands;
}

// Color-attachment-only render pass (no depth - the label batches disable
// depth test/write, same as the main scene) whose final layout leaves the
// image ready to be used as a copy source with no extra barrier needed.
vsg::ref_ptr<vsg::RenderPass> createLabelRenderPass(vsg::ref_ptr<vsg::Device> device)
{
    auto colorAttachment = vsg::defaultColorAttachment(kLabelFormat);
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    vsg::RenderPass::Attachments attachments{colorAttachment};

    vsg::AttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    vsg::SubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachments.emplace_back(colorAttachmentRef);

    vsg::RenderPass::Subpasses subpasses{subpass};

    vsg::SubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dependencyFlags = 0;

    vsg::RenderPass::Dependencies dependencies{dependency};

    return vsg::RenderPass::create(device, attachments, subpasses, dependencies);
}

vsg::ref_ptr<vsg::Framebuffer> createLabelFramebuffer(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::ImageView> imageView)
{
    auto renderPass = createLabelRenderPass(device);
    vsg::ImageViews imageViews{imageView};
    return vsg::Framebuffer::create(renderPass, imageViews, imageView->image->extent.width, imageView->image->extent.height, 1);
}

} // namespace

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

    renderImageView_ = createRenderImageView(device_, extent_);
    captureImage_ = createCaptureImage(device_, extent_);
    captureCommands_ = createCaptureCommands(device_, renderImageView_->image, captureImage_);

    renderGraph_ = vsg::RenderGraph::create();
    renderGraph_->framebuffer = createLabelFramebuffer(device_, renderImageView_);
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
