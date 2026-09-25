#pragma once
//
// BalloonController.h - wires the 'b' key, mouse position, LabelPicker and
// BalloonOverlay together, matching giv's own balloon/tooltip behavior
// (giv-widget.gob: 'b' toggles do_show_balloon; motion-notify calls
// giv_widget_show_balloon(), which looks up the label under the cursor from
// the label image and either shows or hides the popup).
//
#include "BalloonOverlay.h"
#include "GivScene.h"
#include "LabelPicker.h"

#include <vsg/all.h>

namespace giv
{

class BalloonController : public vsg::Inherit<vsg::Visitor, BalloonController>
{
public:
    BalloonController(const SceneData* scene, vsg::ref_ptr<LabelPicker> picker, vsg::ref_ptr<BalloonOverlay> overlay);

    void apply(vsg::KeyPressEvent& event) override;
    void apply(vsg::MoveEvent& event) override;

    bool enabled() const { return enabled_; }

    // Call once per frame, after present(): reads back this frame's label
    // render at the last known mouse position (see LabelPicker::pick's
    // one-frame-latency note) and shows/hides/updates the overlay
    // accordingly; also syncs LabelPicker's enabled/extent state ready for
    // the next frame's render.
    void update(vsg::Viewer* viewer);

private:
    const SceneData* scene_;
    vsg::ref_ptr<LabelPicker> picker_;
    vsg::ref_ptr<BalloonOverlay> overlay_;
    bool enabled_ = false;
    int32_t mouseX_ = 0;
    int32_t mouseY_ = 0;
    bool haveMouse_ = false;
};

} // namespace giv
