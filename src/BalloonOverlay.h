#pragma once
//
// BalloonOverlay.h - draws giv's balloon/tooltip popup as an on-screen HUD
// layer: a flat yellow background box plus a text label, positioned in
// window-pixel space near the mouse cursor (giv-widget.gob's
// giv_widget_show_balloon: balloon_x = px+20, balloon_y = py-20).
//
// Rendered as a second vsg::View (its own screen-space orthographic camera)
// added to the same window RenderGraph as the main scene, so it draws after
// (on top of) it every frame with no depth test.
//
#include <vsg/all.h>

#include <string>

namespace giv
{

class BalloonOverlay : public vsg::Inherit<vsg::Object, BalloonOverlay>
{
public:
    BalloonOverlay(vsg::ref_ptr<vsg::Options> options, const std::string& shaderDir);

    // Add this to the same RenderGraph as the main scene's View, e.g.
    // renderGraph->addChild(overlay->view()).
    vsg::ref_ptr<vsg::View> view() const { return view_; }

    // Recomputes the HUD camera's projection/viewport to match the current
    // window pixel extent. Cheap no-op if unchanged; call once per frame.
    void updateExtent(const VkExtent2D& extent);

    // Shows `text` anchored near window pixel position (x,y); rebuilds the
    // displayed content only if `text` differs from what's already shown.
    // No-op (beyond becoming visible) if already showing this exact text -
    // the caller is expected to re-call every frame the balloon should stay
    // up (e.g. every frame the same dataset is under the cursor). When text
    // changes, a new vsg::Text node is built and spliced into the live
    // scene graph, so `viewer` is needed to compile it (upload its font-atlas
    // descriptor set / vertex buffers) before the next frame records it -
    // an uncompiled Text node has null Vulkan handles and segfaults
    // RecordTraversal.
    void show(vsg::Viewer* viewer, const std::string& text, int32_t x, int32_t y);
    void hide();

private:
    vsg::ref_ptr<vsg::Options> options_;
    std::string shaderDir_;
    vsg::ref_ptr<vsg::Font> font_;
    double fontSize_ = 14.0;

    vsg::ref_ptr<vsg::Camera> camera_;
    vsg::ref_ptr<vsg::View> view_;
    vsg::ref_ptr<vsg::Switch> switch_;
    vsg::ref_ptr<vsg::Group> content_;   // children: [0]=background box StateGroup, [1]=textTransform_
    vsg::ref_ptr<vsg::vec2Array> boxPos_; // dynamic - rewritten by show()

    // vsg::Text bakes its layout->position into vertex data at setup() time
    // (CpuLayoutTechnique), so it can't be cheaply repositioned frame-to-
    // frame by mutating layout->position alone - wrap it in a MatrixTransform
    // instead and update *that* every frame the balloon follows the cursor,
    // keeping the Text node itself (and its font-atlas descriptor set)
    // untouched except when the text content actually changes.
    vsg::ref_ptr<vsg::MatrixTransform> textTransform_;

    VkExtent2D extent_{0, 0};
    std::string currentText_;
    bool visible_ = false;

    void rebuildContent(vsg::Viewer* viewer, const std::string& text, int32_t x, int32_t y);
};

} // namespace giv
