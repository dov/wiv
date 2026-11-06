// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "BalloonController.h"

#include <QLabel>
#include <QPoint>
#include <QString>
#include <QWindow>

namespace giv
{

namespace
{

// Matches giv_widget_show_balloon's search_and_replace(balloon_string,
// "\\n"/"\\r", "\n") - lets a balloon string embed literal "\n"/"\r"
// two-character escapes (as text authored in a .giv file necessarily must,
// since the file format itself is line-oriented) to force a line break in
// the popup.
std::string unescapeNewlines(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == 'r'))
        {
            out += '\n';
            ++i;
        }
        else
            out += s[i];
    }
    return out;
}

} // namespace

BalloonController::BalloonController(const SceneData* scene, vsg::ref_ptr<LabelPicker> picker, QLabel* label, QWindow* originWindow) :
    scene_(scene), picker_(picker), label_(label), originWindow_(originWindow)
{
}

void BalloonController::setEnabled(bool enabled)
{
    enabled_ = enabled;
    if (!enabled_) label_->hide();
}

void BalloonController::apply(vsg::KeyPressEvent& event)
{
    if (event.keyBase == 'b')
    {
        setEnabled(!enabled_);
        event.handled = true;
    }
}

void BalloonController::apply(vsg::MoveEvent& event)
{
    mouseX_ = event.x;
    mouseY_ = event.y;
    haveMouse_ = true;
}

void BalloonController::update(vsg::Viewer* viewer)
{
    picker_->syncExtent(viewer);
    picker_->setEnabled(enabled_);

    if (!enabled_ || !haveMouse_)
    {
        label_->hide();
        return;
    }

    int label = picker_->pick(viewer, mouseX_, mouseY_);
    if (label < 0 || static_cast<size_t>(label) >= scene_->datasets.size())
    {
        label_->hide();
        return;
    }

    // giv falls back to the dataset's $path name when no $balloon text was
    // given (giv-parser.cc: push_back(marks->path_name) when
    // !marks->balloon_string). path_name itself defaults to "Dataset %d"
    // (giv-data.cc: new_giv_dataset), so a dataset with neither $path nor
    // $balloon still shows a "Dataset N" tooltip; "label = N" only remains
    // as a last-resort fallback in case pathName is ever left empty.
    const Dataset& ds = scene_->datasets[static_cast<size_t>(label)];
    std::string text = !ds.balloon.empty() ? ds.balloon : ds.pathName;
    if (text.empty()) text = "label = " + std::to_string(label);

    label_->setText(QString::fromStdString(unescapeNewlines(text)));
    label_->adjustSize();

    // mouseX_/mouseY_ (from vsg::MoveEvent) are *device* pixels - wiv's
    // pixel-space math throughout matches extent2D(), the swapchain's
    // device-pixel size, not the QWidget/QWindow logical size (see
    // VulkanViewport::resizeEvent's doc comment on that same distinction).
    // QWindow::mapToGlobal()/QLabel::move() work in logical (device-
    // independent) pixels, so on a scaled (HiDPI) screen using the device
    // pixel position directly would place the popup a multiple of the
    // cursor's distance away instead of right next to it.
    qreal dpr = originWindow_->devicePixelRatio();
    QPoint localLogical(static_cast<int>(mouseX_ / dpr) + 20, static_cast<int>(mouseY_ / dpr) - 20);

    // Matches giv_widget_show_balloon's balloon_x = px+20, balloon_y = py-20
    // (the box's top-left corner, in window-pixel space); label_ is a
    // top-level window, so that local point needs mapping to the screen.
    label_->move(originWindow_->mapToGlobal(localLogical));
    label_->show();
}

} // namespace giv
