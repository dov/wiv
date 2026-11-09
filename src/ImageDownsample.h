// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// ImageDownsample.h - high-quality, SIMD-accelerated image reduction for thumbnails.
//
// Reduction is an exact area average (every source pixel contributes in proportion to
// how much of it falls inside the destination pixel), done in premultiplied-alpha float so
// transparent pixels don't bleed their colour. The inner loops are written with Google
// Highway and dispatched at run time to the best instruction set the CPU has (SSE2/SSE4/
// AVX2/AVX-512 on x86, NEON/SVE on ARM, a portable fallback elsewhere), so one binary is
// optimal on every machine.
//
#include <QImage>
#include <QString>

#include <cstdint>
#include <vector>

namespace giv
{

// Scales `src` to fit inside maxW x maxH keeping the aspect ratio. A reduction is an area
// average (returned as straight-alpha QImage::Format_RGBA8888); an enlargement or a no-op
// falls back to Qt's smooth scaling. Thread-safe. A null image comes back null.
QImage downscaleToFit(const QImage& src, int maxW, int maxH);

// Name of the instruction-set target the dispatcher currently picks, e.g. "AVX3" or "AVX2".
QString downsampleTargetName();

// Testing: the dispatch targets (hwy bit masks) that this binary has code for and the CPU
// can run, and a way to force one of them. Pass 0 to restore automatic selection.
std::vector<int64_t> downsampleAvailableTargets();
void downsampleForceTarget(int64_t target);
QString downsampleTargetName(int64_t target);

} // namespace giv
