// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ImageDownsample.h"

#include <QSize>

#include <algorithm>
#include <cmath>

// Highway's dynamic dispatch: this file is re-included once per target (foreach_target.h),
// so everything between HWY_BEFORE_NAMESPACE and HWY_AFTER_NAMESPACE is compiled for each.
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "ImageDownsample.cpp"
#include "hwy/foreach_target.h" // must come before highway.h
#include "hwy/highway.h"
#include "hwy/targets.h"

HWY_BEFORE_NAMESPACE();
namespace giv
{
namespace HWY_NAMESPACE
{
namespace hn = hwy::HWY_NAMESPACE;

// acc[0..n) += w * premultiply(src[0..n)); both are interleaved RGBA, n a multiple of 4.
// Source bytes are straight alpha 0..255; the accumulator holds colour*alpha/255 and alpha, both
// on a 0..255 scale. One pass over the source row is the whole per-pixel cost of the reduction.
void AccumulateRowImpl(const uint8_t* HWY_RESTRICT src, float* HWY_RESTRICT acc, size_t n, float w)
{
    const hn::ScalableTag<float> df;
    const hn::RebindToSigned<decltype(df)> di;
    const hn::RebindToUnsigned<decltype(df)> du;
    const hn::Rebind<uint8_t, decltype(df)> d8;
    const size_t N = hn::Lanes(df); // always a multiple of 4: a pixel never straddles two vectors
    const auto vw = hn::Set(df, w);
    const auto vwa = hn::Set(df, w * (1.0f / 255.0f));
    // Lane 3 of every pixel is alpha: it is weighted but not multiplied by itself.
    const auto isAlpha = hn::RebindMask(df, hn::Eq(hn::And(hn::Iota(du, 0), hn::Set(du, 3)), hn::Set(du, 3)));
    size_t i = 0;
    for (; i + N <= n; i += N)
    {
        const auto v = hn::ConvertTo(df, hn::PromoteTo(di, hn::LoadU(d8, src + i)));
        const auto a = hn::Broadcast<3>(v); // alpha of each pixel, in all four of its lanes
        const auto colour = hn::Mul(v, hn::Mul(a, vwa));
        const auto t = hn::IfThenElse(isAlpha, hn::Mul(v, vw), colour);
        hn::StoreU(hn::Add(hn::LoadU(df, acc + i), t), df, acc + i);
    }
    for (; i < n; i += 4) // tail: whole pixels
    {
        const float a = src[i + 3];
        acc[i + 0] += w * src[i + 0] * a * (1.0f / 255.0f);
        acc[i + 1] += w * src[i + 1] * a * (1.0f / 255.0f);
        acc[i + 2] += w * src[i + 2] * a * (1.0f / 255.0f);
        acc[i + 3] += w * a;
    }
}

// Horizontal step for one output row. Output pixel x = sum over source pixels
// [first[x], first[x] + count[x]) of weight * acc pixel, weights taken consecutively from `weights`.
void HorizontalImpl(const float* HWY_RESTRICT acc, const int* HWY_RESTRICT first, const int* HWY_RESTRICT count,
                    const float* HWY_RESTRICT weights, int dstW, float* HWY_RESTRICT out)
{
    const hn::FixedTag<float, 4> d4; // one pixel per vector, on every target
    const float* wp = weights;
    for (int x = 0; x < dstW; ++x)
    {
        auto sum = hn::Zero(d4);
        const float* p = acc + size_t(first[x]) * 4;
        for (int j = 0; j < count[x]; ++j)
            sum = hn::MulAdd(hn::Set(d4, wp[j]), hn::LoadU(d4, p + size_t(j) * 4), sum);
        wp += count[x];
        hn::StoreU(sum, d4, out + size_t(x) * 4);
    }
}

const char* TargetNameImpl() { return hwy::TargetName(HWY_TARGET); }

} // namespace HWY_NAMESPACE
} // namespace giv
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace giv
{
HWY_EXPORT(AccumulateRowImpl);
HWY_EXPORT(HorizontalImpl);
HWY_EXPORT(TargetNameImpl);

namespace
{
// Contribution of each source cell to each of `dst` cells covering `src` cells (area weights, summing to 1).
struct Spans
{
    std::vector<int> first, count;
    std::vector<float> weights;
};

Spans buildSpans(int src, int dst)
{
    Spans s;
    s.first.resize(dst);
    s.count.resize(dst);
    const double scale = double(src) / dst;
    for (int i = 0; i < dst; ++i)
    {
        const double a = i * scale, b = (i + 1) * scale;
        const int lo = std::min(src - 1, int(std::floor(a)));
        const int hi = std::min(src - 1, std::max(lo, int(std::ceil(b)) - 1));
        s.first[i] = lo;
        s.count[i] = hi - lo + 1;
        for (int j = lo; j <= hi; ++j)
            s.weights.push_back(float(std::max(0.0, std::min(b, j + 1.0) - std::max(a, double(j))) / scale));
    }
    return s;
}

QImage areaDownscale(const QImage& srcIn, int dw, int dh)
{
    const QImage src = srcIn.format() == QImage::Format_RGBA8888 ? srcIn : srcIn.convertToFormat(QImage::Format_RGBA8888);
    const int sw = src.width(), sh = src.height();
    const Spans cols = buildSpans(sw, dw);
    const double vscale = double(sh) / dh;

    QImage out(dw, dh, QImage::Format_RGBA8888);
    std::vector<float> acc(size_t(sw) * 4), row(size_t(dw) * 4);
    const size_t accFloats = size_t(sw) * 4;
    for (int y = 0; y < dh; ++y)
    {
        const double a = y * vscale, b = (y + 1) * vscale;
        const int lo = std::min(sh - 1, int(std::floor(a)));
        const int hi = std::min(sh - 1, std::max(lo, int(std::ceil(b)) - 1));
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int r = lo; r <= hi; ++r)
        {
            const float w = float(std::max(0.0, std::min(b, r + 1.0) - std::max(a, double(r))) / vscale);
            if (w > 0.0f) HWY_DYNAMIC_DISPATCH(AccumulateRowImpl)(src.constScanLine(r), acc.data(), accFloats, w);
        }
        HWY_DYNAMIC_DISPATCH(HorizontalImpl)(acc.data(), cols.first.data(), cols.count.data(), cols.weights.data(), dw, row.data());

        uchar* o = out.scanLine(y);
        for (int x = 0; x < dw; ++x)
        {
            const float* p = row.data() + size_t(x) * 4;
            const float al = p[3];
            if (al <= 0.0f)
            {
                o[x * 4 + 0] = o[x * 4 + 1] = o[x * 4 + 2] = o[x * 4 + 3] = 0;
                continue;
            }
            const float k = 255.0f / al; // un-premultiply
            for (int c = 0; c < 3; ++c) o[x * 4 + c] = uchar(std::clamp(p[c] * k + 0.5f, 0.0f, 255.0f));
            o[x * 4 + 3] = uchar(std::clamp(al + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}
} // namespace

QImage downscaleToFit(const QImage& src, int maxW, int maxH)
{
    if (src.isNull() || maxW <= 0 || maxH <= 0) return src;
    QSize target = src.size().scaled(maxW, maxH, Qt::KeepAspectRatio);
    target = target.expandedTo(QSize(1, 1)); // a panorama must not round to zero rows
    if (target.width() >= src.width() || target.height() >= src.height())
        return src.scaled(maxW, maxH, Qt::KeepAspectRatio, Qt::SmoothTransformation); // enlarging: Qt's filter
    return areaDownscale(src, target.width(), target.height());
}

QString downsampleTargetName() { return QString::fromLatin1(HWY_DYNAMIC_DISPATCH(TargetNameImpl)()); }
QString downsampleTargetName(int64_t target) { return QString::fromLatin1(hwy::TargetName(target)); }
std::vector<int64_t> downsampleAvailableTargets() { return hwy::SupportedAndGeneratedTargets(); }
void downsampleForceTarget(int64_t target)
{
    hwy::SetSupportedTargetsForTest(target); // 0 restores detection of the CPU's real capabilities
}
} // namespace giv
#endif // HWY_ONCE
