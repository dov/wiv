// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ExportWriter.h"
#include "Qoi.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

namespace
{
std::vector<uint8_t> testPixels(int w, int h, bool alpha)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    unsigned seed = 12345;
    for (int i = 0; i < w * h; ++i)
    {
        // Mix of smooth gradients (diff/luma ops), flat runs, and noise (rgb/rgba ops).
        const int kind = (i / 37) % 4;
        uint8_t* p = &px[size_t(i) * 4];
        if (kind == 0) p[0] = p[1] = p[2] = 7;
        else if (kind == 1) p[0] = uint8_t(i), p[1] = uint8_t(i / 3), p[2] = uint8_t(i / 7);
        else { seed = seed * 1664525u + 1013904223u; p[0] = seed >> 24; p[1] = seed >> 16; p[2] = seed >> 8; }
        p[3] = alpha && kind == 3 ? uint8_t(seed >> 4) : 255;
    }
    return px;
}
} // namespace

TEST(Qoi, RoundTripOpaqueAndAlpha)
{
    for (bool alpha : {false, true})
    {
        const int w = 97, h = 53;
        const auto px = testPixels(w, h, alpha);
        const auto enc = giv::qoi::encode(px.data(), w, h, alpha ? 4 : 3);
        ASSERT_FALSE(enc.empty());
        giv::qoi::Header hdr;
        std::vector<uint8_t> out;
        ASSERT_TRUE(giv::qoi::decode(enc.data(), enc.size(), hdr, out));
        EXPECT_EQ(hdr.width, uint32_t(w));
        EXPECT_EQ(hdr.height, uint32_t(h));
        EXPECT_EQ(out, px);
    }
}

TEST(Qoi, RejectsTruncatedAndBadData)
{
    const auto px = testPixels(32, 32, true);
    auto enc = giv::qoi::encode(px.data(), 32, 32, 4);
    giv::qoi::Header hdr;
    std::vector<uint8_t> out;
    EXPECT_FALSE(giv::qoi::decode(enc.data(), enc.size() / 2, hdr, out));
    enc[0] = 'x';
    EXPECT_FALSE(giv::qoi::decode(enc.data(), enc.size(), hdr, out));
}

TEST(ExportWriter, WritesEveryFormatAndQoiReadsBack)
{
    int argc = 1;
    char arg0[] = "t";
    char* argv[] = {arg0, nullptr};
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage img(64, 48, QImage::Format_RGBA8888);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x) img.setPixelColor(x, y, QColor(x * 4, y * 5, (x + y) * 2, 255));

    givqt::ExportOptions o;
    o.quality = 80;
    for (const char* name : {"a.png", "a.jpg", "a.webp", "a.tif", "a.bmp", "a.qoi"})
    {
        QString err;
        const QString path = dir.filePath(name);
        EXPECT_TRUE(givqt::writeExportImage(img, path, o, &err)) << name << ": " << err.toStdString();
        EXPECT_GT(QFile(path).size(), 0) << name;
    }
    // Compression choices change the PNG size.
    givqt::ExportOptions none, best;
    none.pngCompression = 0;
    best.pngCompression = 9;
    ASSERT_TRUE(givqt::writeExportImage(img, dir.filePath("n.png"), none));
    ASSERT_TRUE(givqt::writeExportImage(img, dir.filePath("b.png"), best));
    EXPECT_GT(QFile(dir.filePath("n.png")).size(), QFile(dir.filePath("b.png")).size());

    QFile f(dir.filePath("a.qoi"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray data = f.readAll();
    giv::qoi::Header hdr;
    std::vector<uint8_t> out;
    ASSERT_TRUE(giv::qoi::decode(reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()), hdr, out));
    EXPECT_EQ(int(hdr.width), 64);
    EXPECT_EQ(out[0], img.pixelColor(0, 0).red());
}

// ---- ICC: embedded profiles are read from PNG and converted to sRGB on load ----
#include "IccProfile.h"
#include "ImagePluginHost.h"

#include <QColorSpace>

namespace
{
QString writeProfiledPng(const QTemporaryDir& dir, const char* name, const QColorSpace& cs)
{
    QImage img(8, 8, QImage::Format_RGBA8888);
    img.fill(QColor(200, 100, 50));
    img.setColorSpace(cs);
    const QString path = dir.filePath(name);
    EXPECT_TRUE(img.save(path));
    return path;
}
} // namespace

TEST(Icc, ReadsProfileAndConvertsOnLoad)
{
    if (!giv::iccConversionAvailable()) GTEST_SKIP() << "built without Little CMS";
    int argc = 1;
    char arg0[] = "t";
    char* argv[] = {arg0, nullptr};
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    setenv("WIV_PLUGIN_DIR", WIV_TEST_PLUGIN_DIR, 1);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString p3 = writeProfiledPng(dir, "p3.png", QColorSpace(QColorSpace::DisplayP3));
    const QString srgb = writeProfiledPng(dir, "srgb.png", QColorSpace(QColorSpace::SRgb));

    const auto profile = giv::readIccProfile(p3.toStdString());
    ASSERT_FALSE(profile.empty());
    EXPECT_TRUE(giv::iccConversionNeeded(profile));
    EXPECT_FALSE(giv::iccConversionNeeded(giv::readIccProfile(srgb.toStdString())));

    giv::ImagePluginHost::setApplyIccProfile(true);
    auto converted = giv::ImagePluginHost::load(p3.toStdString());
    ASSERT_TRUE(converted.has_value());
    // Display P3 (200,100,50) is more saturated than the same numbers in sRGB: red goes up, green and blue down.
    EXPECT_GT(converted->rgba[0], 200);
    EXPECT_LT(converted->rgba[1], 100);
    EXPECT_EQ(converted->rgba[3], 255);

    giv::ImagePluginHost::setApplyIccProfile(false);
    auto untouched = giv::ImagePluginHost::load(p3.toStdString());
    ASSERT_TRUE(untouched.has_value());
    EXPECT_EQ(untouched->rgba[0], 200);
    EXPECT_EQ(untouched->rgba[1], 100);

    giv::ImagePluginHost::setApplyIccProfile(true);
    auto plain = giv::ImagePluginHost::load(srgb.toStdString());
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->rgba[0], 200);
    EXPECT_EQ(plain->rgba[2], 50);
}
