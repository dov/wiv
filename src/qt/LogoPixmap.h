// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// LogoPixmap.h - rasterizes the vector mascot (resources/giv-logo.svg) at any
// size, shared by the About dialog and the splash page.
//
#include "nanosvg.h"
#include "nanosvgrast.h"

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QPixmap>

#include <algorithm>
#include <vector>

namespace givqt
{

// Rasterizes the vector mascot (cropped to its drawn extent) at the screen's
// physical resolution so it stays sharp on high-DPI displays.
inline QPixmap renderLogoPixmap(int logicalSize, qreal dpr)
{
    QFile f(":/giv-logo.svg");
    if (!f.open(QIODevice::ReadOnly))
        return QPixmap();
    QByteArray svg = f.readAll();
    svg.append('\0');

    NSVGimage* image = nsvgParse(svg.data(), "px", 96.0f);
    if (!image)
        return QPixmap();

    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (NSVGshape* s = image->shapes; s; s = s->next)
    {
        minX = std::min(minX, s->bounds[0]);
        minY = std::min(minY, s->bounds[1]);
        maxX = std::max(maxX, s->bounds[2]);
        maxY = std::max(maxY, s->bounds[3]);
    }
    QPixmap result;
    if (maxX > minX && maxY > minY)
    {
        const int px = std::max(1, static_cast<int>(logicalSize * dpr));
        const float scale = px / std::max(maxX - minX, maxY - minY);
        const int w = std::max(1, static_cast<int>((maxX - minX) * scale));
        const int h = std::max(1, static_cast<int>((maxY - minY) * scale));
        if (NSVGrasterizer* rast = nsvgCreateRasterizer())
        {
            std::vector<unsigned char> buf(static_cast<size_t>(w) * h * 4, 0);
            nsvgRasterize(rast, image, -minX * scale, -minY * scale, scale, buf.data(), w, h, w * 4);
            result = QPixmap::fromImage(
                QImage(buf.data(), w, h, w * 4, QImage::Format_RGBA8888).copy());
            result.setDevicePixelRatio(dpr);
            nsvgDeleteRasterizer(rast);
        }
    }
    nsvgDelete(image);
    return result;
}

} // namespace givqt
