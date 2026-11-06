// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "SvgPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace giv
{

void MotionPath::add(double x, double y, bool jump)
{
    double len = 0;
    if (!pts.empty() && !jump) len = std::hypot(x - pts.back()[0], y - pts.back()[1]);
    pts.push_back({x, y});
    cum.push_back((cum.empty() ? 0.0 : cum.back()) + len);
}

void MotionPath::atDistance(double dist, double& x, double& y, double& angle) const
{
    x = y = angle = 0;
    if (pts.empty()) return;
    if (pts.size() == 1 || total() <= 0)
    {
        x = pts[0][0];
        y = pts[0][1];
        return;
    }
    const double d = std::clamp(dist, 0.0, total());
    size_t i = 1;
    while (i + 1 < pts.size() && cum[i] < d) ++i;
    double seg = cum[i] - cum[i - 1];
    double u = seg > 0 ? (d - cum[i - 1]) / seg : 0.0;
    x = pts[i - 1][0] + u * (pts[i][0] - pts[i - 1][0]);
    y = pts[i - 1][1] + u * (pts[i][1] - pts[i - 1][1]);
    // tangent of the nearest segment with length
    size_t j = i;
    while (j > 1 && cum[j] - cum[j - 1] <= 0) --j;
    angle = std::atan2(pts[j][1] - pts[j - 1][1], pts[j][0] - pts[j - 1][0]) * 180.0 / M_PI;
}

namespace
{

// Appends an SVG elliptical arc from (x0,y0) to (x1,y1) as a polyline (the
// endpoint-to-center conversion of the SVG implementation notes, F.6.5).
void addArc(MotionPath& mp, double x0, double y0, double rx, double ry, double rotDeg, bool large, bool sweep, double x1,
            double y1)
{
    rx = std::abs(rx);
    ry = std::abs(ry);
    if ((x0 == x1 && y0 == y1)) return;
    if (rx == 0 || ry == 0)
    {
        mp.add(x1, y1, false);
        return;
    }
    const double phi = rotDeg * M_PI / 180.0, cp = std::cos(phi), sp = std::sin(phi);
    const double dx = (x0 - x1) / 2, dy = (y0 - y1) / 2;
    const double xp = cp * dx + sp * dy, yp = -sp * dx + cp * dy;
    const double lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1)
    {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const double num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
    const double den = rx * rx * yp * yp + ry * ry * xp * xp;
    double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0.0;
    if (large == sweep) coef = -coef;
    const double cxp = coef * rx * yp / ry, cyp = -coef * ry * xp / rx;
    const double cx = cp * cxp - sp * cyp + (x0 + x1) / 2, cy = sp * cxp + cp * cyp + (y0 + y1) / 2;
    auto ang = [](double ux, double uy, double vx, double vy) {
        double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    const double th1 = ang(1, 0, (xp - cxp) / rx, (yp - cyp) / ry);
    double dth = ang((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry);
    if (!sweep && dth > 0) dth -= 2 * M_PI;
    else if (sweep && dth < 0) dth += 2 * M_PI;
    const int steps = std::max(4, static_cast<int>(std::ceil(std::abs(dth) / (M_PI / 32))));
    for (int i = 1; i <= steps; ++i)
    {
        const double t = th1 + dth * i / steps;
        const double ex = rx * std::cos(t), ey = ry * std::sin(t);
        if (i == steps) mp.add(x1, y1, false);
        else mp.add(cp * ex - sp * ey + cx, sp * ex + cp * ey + cy, false);
    }
}

} // namespace

MotionPath flattenPath(const std::string& d)
{
    MotionPath mp;
    const char* p = d.c_str();
    auto skip = [&] { while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p; };
    auto num = [&](double& v) {
        skip();
        char* e = nullptr;
        v = std::strtod(p, &e);
        if (e == p) return false;
        p = e;
        return true;
    };
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; // current, subpath start, last control
    char cmd = 0, prev = 0;
    constexpr int kSteps = 20;
    while (true)
    {
        skip();
        if (!*p) break;
        if (std::isalpha(static_cast<unsigned char>(*p))) cmd = *p++;
        else if (!cmd) break;
        const bool rel = std::islower(static_cast<unsigned char>(cmd));
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        double v[7];
        auto need = [&](int n) {
            for (int i = 0; i < n; ++i)
                if (!num(v[i])) return false;
            return true;
        };
        const double ox = rel ? cx : 0, oy = rel ? cy : 0;
        bool ok = true;
        switch (up)
        {
        case 'M':
            if (!(ok = need(2))) break;
            cx = sx = ox + v[0];
            cy = sy = oy + v[1];
            mp.add(cx, cy, true);
            cmd = rel ? 'l' : 'L'; // further pairs are line-tos
            break;
        case 'L':
            if (!(ok = need(2))) break;
            cx = ox + v[0];
            cy = oy + v[1];
            mp.add(cx, cy, false);
            break;
        case 'H':
            if (!(ok = need(1))) break;
            cx = ox + v[0];
            mp.add(cx, cy, false);
            break;
        case 'V':
            if (!(ok = need(1))) break;
            cy = oy + v[0];
            mp.add(cx, cy, false);
            break;
        case 'C':
        case 'S':
        {
            double x1, y1;
            if (up == 'C')
            {
                if (!(ok = need(6))) break;
                x1 = ox + v[0]; y1 = oy + v[1];
                v[0] = ox + v[2]; v[1] = oy + v[3]; v[2] = ox + v[4]; v[3] = oy + v[5];
            }
            else
            {
                if (!(ok = need(4))) break;
                bool smooth = prev == 'C' || prev == 'S';
                x1 = smooth ? 2 * cx - lcx : cx;
                y1 = smooth ? 2 * cy - lcy : cy;
                v[0] = ox + v[0]; v[1] = oy + v[1]; v[2] = ox + v[2]; v[3] = oy + v[3];
            }
            const double x2 = v[0], y2 = v[1], x3 = v[2], y3 = v[3];
            for (int i = 1; i <= kSteps; ++i)
            {
                double t = double(i) / kSteps, u = 1 - t;
                mp.add(u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                       u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3, false);
            }
            lcx = x2; lcy = y2; cx = x3; cy = y3;
            break;
        }
        case 'Q':
        case 'T':
        {
            double x1, y1;
            if (up == 'Q')
            {
                if (!(ok = need(4))) break;
                x1 = ox + v[0]; y1 = oy + v[1];
                v[0] = ox + v[2]; v[1] = oy + v[3];
            }
            else
            {
                if (!(ok = need(2))) break;
                bool smooth = prev == 'Q' || prev == 'T';
                x1 = smooth ? 2 * cx - lcx : cx;
                y1 = smooth ? 2 * cy - lcy : cy;
                v[0] = ox + v[0]; v[1] = oy + v[1];
            }
            for (int i = 1; i <= kSteps; ++i)
            {
                double t = double(i) / kSteps, u = 1 - t;
                mp.add(u * u * cx + 2 * u * t * x1 + t * t * v[0], u * u * cy + 2 * u * t * y1 + t * t * v[1], false);
            }
            lcx = x1; lcy = y1; cx = v[0]; cy = v[1];
            break;
        }
        case 'A':
        {
            // rx ry x-rotation large-arc sweep x y; the flags are single digits that
            // may be written without separators ("a1 1 0 0110 10").
            auto flag = [&](double& f) {
                skip();
                if (*p != '0' && *p != '1') return false;
                f = *p++ - '0';
                return true;
            };
            if (!(ok = num(v[0]) && num(v[1]) && num(v[2]) && flag(v[3]) && flag(v[4]) && num(v[5]) && num(v[6]))) break;
            const double ex = ox + v[5], ey = oy + v[6];
            addArc(mp, cx, cy, v[0], v[1], v[2], v[3] != 0, v[4] != 0, ex, ey);
            cx = ex;
            cy = ey;
            break;
        }
        case 'Z':
            cx = sx;
            cy = sy;
            mp.add(cx, cy, false);
            break;
        default: ok = false;
        }
        if (!ok) break;
        prev = up;
        if (up != 'C' && up != 'S' && up != 'Q' && up != 'T') { lcx = cx; lcy = cy; }
    }
    return mp;
}


} // namespace giv
