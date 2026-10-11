#pragma once
//
// ViewFlip.h - Orthographic projection with optional horizontal/vertical
// mirroring (giv's View > Horizontal Flip / Vertical Flip). The mirror is
// applied only in transform(), so left/right/bottom/top keep their usual
// symmetric (-half..+half) meaning everywhere else; code that maps screen
// pixels to world units multiplies its offset from the view center by
// flipSignX()/flipSignY() to stay consistent with what is drawn.
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

    vsg::dmat4 transform() const override
    {
        return vsg::scale(flipX ? -1.0 : 1.0, flipY ? -1.0 : 1.0, 1.0) * vsg::Orthographic::transform();
    }
};

// +1 normally, -1 when the camera's projection is mirrored on that axis.
inline double flipSignX(const vsg::ref_ptr<vsg::Camera>& camera)
{
    auto p = camera ? camera->projectionMatrix.cast<FlipOrthographic>() : vsg::ref_ptr<FlipOrthographic>();
    return (p && p->flipX) ? -1.0 : 1.0;
}

inline double flipSignY(const vsg::ref_ptr<vsg::Camera>& camera)
{
    auto p = camera ? camera->projectionMatrix.cast<FlipOrthographic>() : vsg::ref_ptr<FlipOrthographic>();
    return (p && p->flipY) ? -1.0 : 1.0;
}

} // namespace giv
