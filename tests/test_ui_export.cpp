// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
// Off-screen export (File > Export / the json-rpc export_image call).
#include "UITestDriver.h"

#include "qt/VulkanViewport.h"

#include <QDir>
#include <gtest/gtest.h>

using givqt::VulkanViewport;

class UIExport : public ::testing::Test
{
protected:
    UITestDriver ui;
    QString out(const char* name) { return QDir(UITestDriver::scratchDir()).filePath(name); }
};

TEST_F(UIExport, FitExportHasRequestedSizeAndContent)
{
    ASSERT_TRUE(ui.loadGivText("$background white\n$color red\n$polygon\n-5 -5\n5 -5\n5 5\n-5 5\n-5 -5\n"));
    VulkanViewport::ExportView view; // Fit
    QString err;
    ASSERT_TRUE(ui.viewport()->exportImage(out("fit.png"), 320, 200, view, &err)) << qPrintable(err);
    QImage img(out("fit.png"));
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 320);
    EXPECT_EQ(img.height(), 200);
    // The content is fit into the export: centre is red, a corner is background.
    EXPECT_TRUE(UITestDriver::isColorNear(img.pixel(160, 100), qRgb(255, 0, 0)));
    EXPECT_GT(UITestDriver::countPixels(img, qRgb(255, 255, 255)), 100);
}

TEST_F(UIExport, ExplicitViewZoomsIn)
{
    ASSERT_TRUE(ui.loadGivText("$background white\n$color red\n$polygon\n-5 -5\n5 -5\n5 5\n-5 5\n-5 -5\n"));
    VulkanViewport::ExportView view;
    view.kind = VulkanViewport::ExportView::Kind::Explicit;
    view.scale = 100.0; // the 10-unit square is far bigger than the 100x100 image
    view.centerX = 0;
    view.centerY = 0;
    ASSERT_TRUE(ui.viewport()->exportImage(out("zoom.png"), 100, 100, view));
    QImage img(out("zoom.png"));
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(UITestDriver::countPixels(img, qRgb(255, 0, 0)), 100 * 100);
}

TEST_F(UIExport, ExportToBadPathFails)
{
    ASSERT_TRUE(ui.loadGivText("0 0\n1 1\n"));
    QString err;
    EXPECT_FALSE(ui.viewport()->exportImage("/nonexistent-dir/x.png", 50, 50, {}, &err));
    EXPECT_FALSE(err.isEmpty());
}
