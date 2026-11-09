// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ImageDownsample.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace
{
// Straightforward double-precision area average with premultiplied alpha.
QImage referenceDownscale(const QImage& src, int dw, int dh)
{
    QImage out(dw, dh, QImage::Format_RGBA8888);
    const double sx = double(src.width()) / dw, sy = double(src.height()) / dh;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x)
        {
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int j = int(std::floor(y * sy)); j < std::min(src.height(), int(std::ceil((y + 1) * sy))); ++j)
                for (int i = int(std::floor(x * sx)); i < std::min(src.width(), int(std::ceil((x + 1) * sx))); ++i)
                {
                    const double w = (std::min((x + 1) * sx, i + 1.0) - std::max(x * sx, double(i))) *
                                     (std::min((y + 1) * sy, j + 1.0) - std::max(y * sy, double(j)));
                    const uchar* p = src.constScanLine(j) + i * 4;
                    for (int c = 0; c < 3; ++c) acc[c] += w * p[c] * p[3] / 255.0;
                    acc[3] += w * p[3];
                    wsum += w;
                }
            uchar* o = out.scanLine(y) + x * 4;
            const double a = acc[3] / wsum;
            for (int c = 0; c < 3; ++c) o[c] = a > 0 ? uchar(std::clamp(acc[c] / wsum * 255.0 / a + 0.5, 0.0, 255.0)) : 0;
            o[3] = uchar(std::clamp(a + 0.5, 0.0, 255.0));
        }
    return out;
}

QImage randomImage(int w, int h, bool withAlpha, unsigned seed = 1)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; ++y)
    {
        uchar* p = img.scanLine(y);
        for (int x = 0; x < w * 4; ++x) p[x] = uchar(rng());
        if (!withAlpha)
            for (int x = 0; x < w; ++x) p[x * 4 + 3] = 255;
        else
            for (int x = 0; x < w; x += 3) p[x * 4 + 3] = 0; // fully transparent pixels: no division by zero
    }
    return img;
}

// Largest channel difference; the colour of a nearly transparent pixel is ill-conditioned, so it is skipped.
int maxDiff(const QImage& a, const QImage& b)
{
    EXPECT_EQ(a.size(), b.size());
    int worst = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
        {
            const uchar* p = a.constScanLine(y) + x * 4;
            const uchar* q = b.constScanLine(y) + x * 4;
            worst = std::max(worst, std::abs(p[3] - q[3]));
            if (q[3] >= 32)
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(p[c] - q[c]));
        }
    return worst;
}
} // namespace

TEST(Downsample, MatchesReferenceOnEveryTarget)
{
    struct Case { int w, h, box; bool alpha; };
    const Case cases[] = {{1000, 700, 256, false}, {1001, 333, 100, true}, {513, 517, 64, false}, {37, 91, 16, true}, {4000, 30, 256, false}};
    for (int64_t target : giv::downsampleAvailableTargets())
    {
        giv::downsampleForceTarget(target);
        SCOPED_TRACE(giv::downsampleTargetName(target).toStdString());
        for (const Case& c : cases)
        {
            const QImage src = randomImage(c.w, c.h, c.alpha);
            const QImage got = giv::downscaleToFit(src, c.box, c.box);
            const QSize want = src.size().scaled(c.box, c.box, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
            ASSERT_EQ(got.size(), want);
            EXPECT_LE(maxDiff(got, referenceDownscale(src, want.width(), want.height())), 1) << c.w << "x" << c.h;
        }
    }
    giv::downsampleForceTarget(0);
}

TEST(Downsample, OpaqueFlatImageStaysFlat)
{
    QImage src(777, 555, QImage::Format_RGBA8888);
    src.fill(QColor(200, 100, 50, 255));
    const QImage out = giv::downscaleToFit(src, 256, 256);
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) ASSERT_EQ(out.pixelColor(x, y), QColor(200, 100, 50, 255));
}

TEST(Downsample, ExtremePanoramaKeepsOneRow)
{
    const QImage out = giv::downscaleToFit(randomImage(6000, 3, false), 256, 256);
    EXPECT_EQ(out.width(), 256);
    EXPECT_GE(out.height(), 1);
}

TEST(Downsample, SmallImageIsEnlargedLikeQt)
{
    const QImage src = randomImage(40, 20, false);
    EXPECT_EQ(giv::downscaleToFit(src, 256, 256).size(), src.scaled(256, 256, Qt::KeepAspectRatio).size());
}

TEST(Downsample, ReportsATarget) { EXPECT_FALSE(giv::downsampleTargetName().isEmpty()); }
