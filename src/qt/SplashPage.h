// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// SplashPainter - paints the empty-window page: the logo, a faint "about"
// block, and while a file is loading a bigger "Loading..." with a small
// animation (a big circle chasing a ring of small ones). It only produces an
// image; VulkanViewport shows that as an ordinary image in the Vulkan scene
// (so nothing is stacked over the native Vulkan window, which breaks its
// presenting) and repaints it per frame for the animation.
//
#include "BuildInfo.h"
#include "LogoPixmap.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSize>
#include <QString>

#include <algorithm>
#include <cmath>

namespace givqt
{
class SplashPainter
{
public:
    static QColor backgroundColor() { return QGuiApplication::palette().color(QPalette::Base); }

    // `logical` is the page size in device-independent pixels; the image has
    // logical * dpr pixels (RGBA8888, opaque). `seconds` drives the animation.
    QImage render(QSize logical, qreal dpr, bool loading, double seconds)
    {
        QImage img(QSize(std::max(1, qRound(logical.width() * dpr)), std::max(1, qRound(logical.height() * dpr))),
                   QImage::Format_RGBA8888);
        img.setDevicePixelRatio(dpr);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        paint(p, logical, dpr, loading, seconds);
        return img;
    }

private:
    void paint(QPainter& p, QSize size, qreal dpr, bool loading, double seconds)
    {
        const int width = size.width(), height = size.height();
        const QPalette pal = QGuiApplication::palette();
        const QColor base = pal.color(QPalette::Base);
        const QColor text = pal.color(QPalette::Text);
        p.fillRect(QRect(QPoint(0, 0), size), base);

        // Quite low contrast: a third of the way from the background to the text color.
        const QColor faint = mix(base, text, 0.30);
        const QColor mid = mix(base, text, 0.55);

        const QFont f = QGuiApplication::font();
        QFont title = f;
        title.setPointSizeF(f.pointSizeF() * 2.2);
        title.setBold(true);
        const QString commit = QString::fromLatin1(giv::kCommitId).left(10);
        struct Line
        {
            QString s;
            QFont font;
        };
        const Line lines[] = {{QStringLiteral("wiv"), title},
                              {QStringLiteral("version v%1 (%2)").arg(QString::fromLatin1(giv::kVersion), commit), f},
                              {QStringLiteral("a program by Dov Grobgeld"), f},
                              {QStringLiteral("<dov.grobgeld@gmail.com>"), f}};

        const int logoSize = std::clamp(height / 4, 72, 192);
        const int logoGap = logoSize / 4;
        int blockH = logoSize + logoGap;
        for (const auto& l : lines) blockH += QFontMetrics(l.font).lineSpacing();
        QFont big = f;
        big.setPointSizeF(f.pointSizeF() * 2.0);
        const int bigH = QFontMetrics(big).lineSpacing();
        const int animSize = 72;
        const int gap = bigH / 2;
        const int animGap = bigH; // between "Loading..." and the animation
        const int loadingH = gap + bigH + animGap + animSize; // always reserved, so the text does not jump

        int y = std::max(8, (height - blockH - loadingH) / 2);
        {
            // Re-rasterized from the SVG whenever the size or the pixel ratio changes.
            if (logo_.isNull() || logoSize_ != logoSize || logoDpr_ != dpr)
            {
                logo_ = renderLogoPixmap(logoSize, dpr);
                logoSize_ = logoSize;
                logoDpr_ = dpr;
            }
            if (!logo_.isNull())
            {
                const QSizeF sz = QSizeF(logo_.size()) / dpr; // cropped to the drawing, so not always square
                p.setOpacity(0.5);
                p.drawPixmap(QPointF((width - sz.width()) / 2.0, y + (logoSize - sz.height()) / 2.0), logo_);
                p.setOpacity(1.0);
            }
            y += logoSize + logoGap;
        }
        p.setPen(faint);
        for (const auto& l : lines)
        {
            const QFontMetrics fm(l.font);
            p.setFont(l.font);
            p.drawText(QRect(0, y, width, fm.lineSpacing()), Qt::AlignHCenter | Qt::AlignVCenter, l.s);
            y += fm.lineSpacing();
        }
        if (!loading) return;

        y += gap;
        p.setFont(big);
        p.setPen(mid);
        p.drawText(QRect(0, y, width, bigH), Qt::AlignHCenter | Qt::AlignVCenter, QObject::tr("Loading..."));
        y += bigH + animGap;
        paintChase(p, mid, QPointF(width / 2.0, y + animSize / 2.0), animSize / 2.0 - 4.0, seconds);
    }

    static QColor mix(const QColor& a, const QColor& b, double t)
    {
        return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                                a.blueF() + (b.blueF() - a.blueF()) * t);
    }

    // A big circle orbits a ring of small ones, all in the text color; each
    // small circle is "nibbled" down as the big one passes and then grows back.
    static void paintChase(QPainter& p, const QColor& color, QPointF c, double radius, double t)
    {
        constexpr int kDots = 10;
        constexpr double kTwoPi = 6.283185307179586;
        const double lead = std::fmod(t * 1.6, 1.0) * kTwoPi; // one lap per 0.625 s
        const double dotR = radius * 0.16, bigR = radius * 0.34;
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (int i = 0; i < kDots; ++i)
        {
            const double a = kTwoPi * i / kDots;
            const double behind = std::fmod(lead - a + 4 * kTwoPi, kTwoPi); // how far past this dot the big one is
            const double grow = std::min(1.0, behind / (kTwoPi * 0.8));
            const double r = dotR * (0.25 + 0.75 * grow * grow);
            p.drawEllipse(c + QPointF(std::cos(a), std::sin(a)) * radius, r, r);
        }
        p.drawEllipse(c + QPointF(std::cos(lead), std::sin(lead)) * radius, bigR, bigR);
    }

    QPixmap logo_;
    int logoSize_ = 0;
    qreal logoDpr_ = 0;
};
} // namespace givqt
