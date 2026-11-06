// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "OffscreenTarget.h"

#include <cstring>

namespace giv
{

namespace
{

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

bool supportsBlit(vsg::ref_ptr<vsg::Device> device, VkFormat format, VkFormat dstFormat)
{
    auto physicalDevice = device->getPhysicalDevice();
    VkFormatProperties srcProps, dstProps;
    vkGetPhysicalDeviceFormatProperties(*physicalDevice, format, &srcProps);
    vkGetPhysicalDeviceFormatProperties(*physicalDevice, dstFormat, &dstProps);
    return ((srcProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0) &&
           ((dstProps.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0);
}

vsg::ref_ptr<vsg::ImageView> createRenderImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent, VkFormat format)
{
    auto image = vsg::Image::create();
    image->format = format;
    image->extent = VkExtent3D{extent.width, extent.height, 1};
    image->mipLevels = 1;
    image->arrayLayers = 1;
    image->samples = VK_SAMPLE_COUNT_1_BIT; // no MSAA - label picking needs exact, unblended pixels; export doesn't need it either
    image->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return vsg::createImageView(device, image, VK_IMAGE_ASPECT_COLOR_BIT);
}

// Host-visible, linearly-tiled destination for the post-render copy - see
// my-vsg-qt-viewer.cpp's createCaptureImage (this is the same screenshot-
// style two-image pattern: optimal-tiled render target -> copy -> linear
// host-visible image -> map).
vsg::ref_ptr<vsg::Image> createCaptureImage(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent, VkFormat format)
{
    auto image = vsg::Image::create();
    image->format = format;
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
    else if (supportsBlit(device, srcImage->format, dstImage->format))
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
        throw std::runtime_error("wiv: GPU does not support the image copy/blit needed for off-screen capture");
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
vsg::ref_ptr<vsg::RenderPass> createRenderPass(vsg::ref_ptr<vsg::Device> device, VkFormat format, bool withDepth)
{
    auto colorAttachment = vsg::defaultColorAttachment(format);
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    vsg::RenderPass::Attachments attachments{colorAttachment};
    if (withDepth) attachments.push_back(vsg::defaultDepthAttachment(kDepthFormat));

    vsg::AttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    vsg::SubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachments.emplace_back(colorAttachmentRef);
    if (withDepth)
    {
        vsg::AttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.depthStencilAttachments.emplace_back(depthRef);
    }

    vsg::RenderPass::Subpasses subpasses{subpass};

    vsg::SubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (withDepth)
    {
        const VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcStageMask |= depthStages;
        dependency.dstStageMask |= depthStages;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    dependency.dependencyFlags = 0;

    vsg::RenderPass::Dependencies dependencies{dependency};

    return vsg::RenderPass::create(device, attachments, subpasses, dependencies);
}

vsg::ref_ptr<vsg::ImageView> createDepthImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent)
{
    auto image = vsg::Image::create();
    image->format = kDepthFormat;
    image->extent = VkExtent3D{extent.width, extent.height, 1};
    image->mipLevels = 1;
    image->arrayLayers = 1;
    image->samples = VK_SAMPLE_COUNT_1_BIT;
    image->usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return vsg::createImageView(device, image, VK_IMAGE_ASPECT_DEPTH_BIT);
}

vsg::ref_ptr<vsg::Framebuffer> createFramebuffer(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::ImageView> imageView,
                                                  vsg::ref_ptr<vsg::ImageView> depthImageView)
{
    auto renderPass = createRenderPass(device, imageView->image->format, depthImageView != nullptr);
    vsg::ImageViews imageViews{imageView};
    if (depthImageView) imageViews.push_back(depthImageView);
    return vsg::Framebuffer::create(renderPass, imageViews, imageView->image->extent.width, imageView->image->extent.height, 1);
}

} // namespace

OffscreenTarget OffscreenTarget::create(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent, VkFormat format, bool withDepth)
{
    OffscreenTarget t;
    t.renderImageView = createRenderImageView(device, extent, format);
    t.captureImage = createCaptureImage(device, extent, format);
    t.captureCommands = createCaptureCommands(device, t.renderImageView->image, t.captureImage);
    if (withDepth) t.depthImageView = createDepthImageView(device, extent);
    t.framebuffer = createFramebuffer(device, t.renderImageView, t.depthImageView);
    return t;
}

std::vector<uint8_t> OffscreenTarget::readPixels(vsg::ref_ptr<vsg::Device> device) const
{
    const uint32_t w = captureImage->extent.width;
    const uint32_t h = captureImage->extent.height;

    VkImageSubresource subResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(*device, captureImage->vk(device->deviceID), &subResource, &layout);

    auto deviceMemory = captureImage->getDeviceMemory(device->deviceID);
    void* mapped = nullptr;
    const VkDeviceSize size = layout.offset + static_cast<VkDeviceSize>(h) * layout.rowPitch;
    deviceMemory->map(0, size, 0, &mapped);

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    const auto* base = static_cast<const uint8_t*>(mapped) + layout.offset;
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(y) * w * 4, base + static_cast<size_t>(y) * layout.rowPitch,
                    static_cast<size_t>(w) * 4);
    deviceMemory->unmap();
    return pixels;
}

} // namespace giv
