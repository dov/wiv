// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// BalloonController.h - wires the 'b' key, mouse position and LabelPicker
// together and drives a native QLabel tooltip, matching giv's own
// balloon/tooltip behavior (giv-widget.gob: 'b' toggles do_show_balloon;
// motion-notify calls giv_widget_show_balloon(), which looks up the label
// under the cursor from the label image and either shows or hides the
// popup).
//
// The popup itself is a plain top-level QLabel (Qt::ToolTip), not a vsg
// scene node: wiv's Vulkan surface is embedded via
// QWidget::createWindowContainer(), and a native child window like that
// always composites on top of any sibling QWidget in the same parent
// regardless of z-order - a scene-graph-drawn overlay would need its own
// depth/pipeline handling to appear over the 3D content and is still at the
// mercy of that native-window layering for anything Qt-side (menus,
// dialogs); a genuine top-level OS window has none of those problems. This
// mirrors how the older qviv widget (QvivWidget.cpp) shows its balloon.
//
#include "GivScene.h"
#include "LabelPicker.h"

#include <vsg/all.h>

class QLabel;
class QWindow;

namespace giv
{

class BalloonController : public vsg::Inherit<vsg::Visitor, BalloonController>
{
public:
    // `label` is the tooltip widget to drive (owned by the caller, e.g.
    // VulkanViewport); `originWindow` is the native window mouseX_/mouseY_
    // are local to, used to map them to the global screen position `label`
    // needs (it's a top-level window, positioned independently of wiv's
    // widget hierarchy).
    BalloonController(const SceneData* scene, vsg::ref_ptr<LabelPicker> picker, QLabel* label, QWindow* originWindow);

    void apply(vsg::KeyPressEvent& event) override;
    void apply(vsg::MoveEvent& event) override;

    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);

    // Call once per frame, after present(): reads back this frame's label
    // render at the last known mouse position (see LabelPicker::pick's
    // one-frame-latency note) and shows/hides/repositions the tooltip
    // accordingly; also syncs LabelPicker's enabled/extent state ready for
    // the next frame's render.
    void update(vsg::Viewer* viewer);

private:
    const SceneData* scene_;
    vsg::ref_ptr<LabelPicker> picker_;
    QLabel* label_;
    QWindow* originWindow_;
    bool enabled_ = false;
    int32_t mouseX_ = 0;
    int32_t mouseY_ = 0;
    bool haveMouse_ = false;
};

} // namespace giv
