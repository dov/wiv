#pragma once
// SMIL animation for SVG: <animate>, <set>, <animateTransform> and
// <animateMotion> elements
// are collected from the DOM once, and evaluate() then returns a copy of the
// DOM with every animated attribute replaced by its value at a given time.
// Not supported: CSS @keyframes, event-based begin times, `end`,
// `accumulate`; arcs in motion paths are approximated by their chord.

#include "SvgDom.h"

#include <memory>
#include <vector>

namespace giv
{

class SvgAnimation
{
public:
    // Collects the animations under `root` (which must outlive this object).
    explicit SvgAnimation(const SvgNode& root);
    ~SvgAnimation();

    bool empty() const { return anims_.empty(); }
    // Length in seconds of one full cycle (or of the whole timeline when
    // !loops()).
    double duration() const { return duration_; }
    // True if some animation repeats indefinitely.
    bool loops() const { return loops_; }

    // The DOM as it looks at time `t` seconds.
    std::unique_ptr<SvgNode> evaluate(double t) const;

private:
    struct Anim;
    const SvgNode& root_;
    std::vector<Anim> anims_;
    double vw_ = 800.0, vh_ = 600.0; // viewport, for resolving percentages
    double duration_ = 0.0;
    bool loops_ = false;
};

} // namespace giv
