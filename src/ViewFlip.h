#pragma once
//
// ViewFlip.h - Orthographic projection with optional horizontal/vertical
// mirroring (giv's View > Horizontal Flip / Vertical Flip). The mirror is
// applied only in transform(), so left/right/bottom/top keep their usual
// symmetric (-half..+half) meaning everywhere else; code that maps screen
// pixels to world units multiplies its offset from the view center by
// screenToViewOffset() to stay consistent with what is drawn. The view can
// also be turned in quarter turns about its centre (View > Rotate).
//
#include <vsg/all.h>

namespace giv
{

class FlipOrthographic : public vsg::Inherit<vsg::Orthographic, FlipOrthographic>
{
public:
    FlipOrthographic(double l, double r, double b, double t, double nd, double fd) :
        Inherit(l, r, b, t, nd, fd) {}

    bool flipX = false;
    bool flipY = false;
    // Quarter turns of the view, counter-clockwise on screen (0..3). Applied
    // about the view centre before the (screen-aligned) ortho scaling, so
    // left/right/bottom/top stay in screen axes.
    int quarterTurns = 0;

    // The rotation angle in radians that makes the view turn counter-
    // clockwise on screen even when it is also mirrored (a mirror reverses
    // the sense of a rotation applied before it).
    double angle() const
    {
        const double a = quarterTurns * (vsg::PI * 0.5);
        return flipX != flipY ? -a : a;
    }

    vsg::dmat4 transform() const override
    {
        return vsg::scale(flipX ? -1.0 : 1.0, flipY ? -1.0 : 1.0, 1.0) * vsg::Orthographic::transform() *
               vsg::rotate(angle(), 0.0, 0.0, 1.0);
    }
};

// Maps an offset on screen (sx to the right, sy upwards, already in world
// units per pixel) to the matching offset in the camera's view space (the
// space of LookAt::center), undoing the mirroring and rotation.
inline vsg::dvec2 screenToViewOffset(const vsg::ref_ptr<vsg::Camera>& camera, double sx, double sy)
{
    auto p = camera ? camera->projectionMatrix.cast<FlipOrthographic>() : vsg::ref_ptr<FlipOrthographic>();
    if (!p) return {sx, sy};
    if (p->flipX) sx = -sx;
    if (p->flipY) sy = -sy;
    const double a = -p->angle(), c = std::cos(a), s = std::sin(a);
    return {c * sx - s * sy, s * sx + c * sy};
}

} // namespace giv
