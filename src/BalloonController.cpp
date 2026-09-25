#include "BalloonController.h"

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

BalloonController::BalloonController(const SceneData* scene, vsg::ref_ptr<LabelPicker> picker, vsg::ref_ptr<BalloonOverlay> overlay) :
    scene_(scene), picker_(picker), overlay_(overlay)
{
}

void BalloonController::apply(vsg::KeyPressEvent& event)
{
    if (event.keyBase == 'b')
    {
        enabled_ = !enabled_;
        if (!enabled_) overlay_->hide();
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
        overlay_->hide();
        return;
    }

    int label = picker_->pick(viewer, mouseX_, mouseY_);
    if (label < 0 || static_cast<size_t>(label) >= scene_->datasets.size())
    {
        overlay_->hide();
        return;
    }

    // giv falls back to the dataset's $path name when no $balloon text was
    // given (giv-parser.cc: push_back(marks->path_name) when
    // !marks->balloon_string), and to a bare "label = N" when neither is
    // set (giv-widget.gob's giv_widget_show_balloon).
    const Dataset& ds = scene_->datasets[static_cast<size_t>(label)];
    std::string text = !ds.balloon.empty() ? ds.balloon : ds.pathName;
    if (text.empty()) text = "label = " + std::to_string(label);

    overlay_->show(viewer, unescapeNewlines(text), mouseX_, mouseY_);
}

} // namespace giv
