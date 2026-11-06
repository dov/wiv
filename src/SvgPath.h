// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
// A polyline measured along its length, built from SVG path data. Used for
// <animateMotion> paths and for <textPath>.

#include <array>
#include <string>
#include <vector>

namespace giv
{

struct MotionPath
{
    std::vector<std::array<double, 2>> pts;
    std::vector<double> cum; // cumulative length at each point
    double total() const { return cum.empty() ? 0.0 : cum.back(); }

    // Appends a point; `jump` starts a new subpath (no length is added).
    void add(double x, double y, bool jump);
    // Position and tangent angle (degrees) at fraction f (0..1) of the length.
    void at(double f, double& x, double& y, double& angle) const { atDistance(f * total(), x, y, angle); }
    // Same, at an absolute distance along the path (clamped to its ends).
    void atDistance(double d, double& x, double& y, double& angle) const;
};

// SVG path data -> polyline. Curves and arcs are subdivided.
MotionPath flattenPath(const std::string& d);

} // namespace giv
