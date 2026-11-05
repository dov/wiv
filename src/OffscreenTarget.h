#pragma once
//
// OffscreenTarget.h - the off-screen color render target and its CPU-readback
// plumbing, shared by LabelPicker (balloon label picking) and the image
// export (VulkanViewport::exportImage): an optimal-tiled color attachment
// the scene is rendered into, plus commands that copy it into a linear,
// host-visible image whose memory can be mapped and read after the frame's
// fence has signalled.
//
#include <vsg/all.h>

#include <cstdint>
#include <vector>

namespace giv
{

struct OffscreenTarget
{
    vsg::ref_ptr<vsg::ImageView> renderImageView; // color attachment, written by the render pass
    vsg::ref_ptr<vsg::Image> captureImage;        // linear-tiled, host-visible copy destination
    vsg::ref_ptr<vsg::Commands> captureCommands;  // copies renderImageView -> captureImage; record after the render graph
    vsg::ref_ptr<vsg::Framebuffer> framebuffer;   // for the RenderGraph that draws into renderImageView
    vsg::ref_ptr<vsg::ImageView> depthImageView;  // only with withDepth

    // Single-sample target. Color-only by default (the label pass's
    // pipelines disable depth test/write); withDepth adds a depth buffer,
    // which the lines pipeline uses to draw translucent lines once per pixel
    // (see lines.frag) - needed for image export to match the window.
    static OffscreenTarget create(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent,
                                  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, bool withDepth = false);

    // Maps captureImage and returns its pixels tightly packed (4 bytes per
    // pixel, row-major). Only valid once the frame that rendered into the
    // target has finished on the GPU (wait on its fence first).
    std::vector<uint8_t> readPixels(vsg::ref_ptr<vsg::Device> device) const;
};

} // namespace giv
