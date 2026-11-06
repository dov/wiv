// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
// GUI tests: drive a real MainWindow + Vulkan viewport and inspect the result
// through screenshots and the viewport's public state.
#include "UITestDriver.h"

#include "qt/MainWindow.h"
#include "qt/VulkanViewport.h"

#include <QDockWidget>
#include <gtest/gtest.h>

namespace
{
// A big solid red square on a white background, centred on the origin.
const char* kRedSquare =
    "$background white\n"
    "$color red\n$polygon\n"
    "-50 -50\n50 -50\n50 50\n-50 50\n-50 -50\n";

struct Xform
{
    double sx, sy, shx, shy;
};
Xform xform(UITestDriver& ui)
{
    Xform t{};
    ui.viewport()->getTransformation(t.sx, t.sy, t.shx, t.shy);
    return t;
}
} // namespace

class UIViewer : public ::testing::Test
{
protected:
    UITestDriver ui;
};

TEST_F(UIViewer, StartsEmpty)
{
    EXPECT_TRUE(ui.viewport()->sceneData().datasets.empty());
    EXPECT_NE(ui.renderWindow(), nullptr);
}

TEST_F(UIViewer, LoadingSceneRendersItsGeometry)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare)) << qPrintable(ui.error());
    EXPECT_TRUE(ui.viewport()->hasScene());
    const QImage img = ui.screenshot();
    ASSERT_FALSE(img.isNull());
    // Fit-to-window: the red square fills a big part of the viewport, the
    // surrounding is the scene's white background.
    const int red = UITestDriver::countPixels(img, qRgb(255, 0, 0));
    const int white = UITestDriver::countPixels(img, qRgb(255, 255, 255));
    EXPECT_GT(red, img.width() * img.height() / 10);
    EXPECT_GT(white, 100);
    EXPECT_TRUE(UITestDriver::isColorNear(img.pixel(img.width() / 2, img.height() / 2), qRgb(255, 0, 0)));
}

TEST_F(UIViewer, BackgroundDirectiveChangesBackground)
{
    ASSERT_TRUE(ui.loadGivText("$background blue\n$color red\n0 0\n1 1\n"));
    const QImage img = ui.screenshot();
    EXPECT_GT(UITestDriver::countPixels(img, qRgb(0, 0, 255)), img.width() * img.height() / 4);
}

TEST_F(UIViewer, MissingFileReportsError)
{
    EXPECT_FALSE(ui.load("/nonexistent/nothing.giv"));
    EXPECT_FALSE(ui.error().isEmpty());
}

TEST_F(UIViewer, ZoomActionsChangeScale)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare));
    ASSERT_TRUE(ui.trigger("FittoWindow"));
    const double fit = xform(ui).sx;
    ASSERT_GT(fit, 0.0);
    ASSERT_TRUE(ui.trigger("ZoomIn"));
    EXPECT_GT(xform(ui).sx, fit);
    ASSERT_TRUE(ui.trigger("FittoWindow"));
    ui.pump(600); // the fit glides
    ASSERT_TRUE(ui.trigger("ZoomOut"));
    EXPECT_LT(xform(ui).sx, fit);
}

TEST_F(UIViewer, ActualPixelsIsOneToOne)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare));
    ASSERT_TRUE(ui.trigger("ActualPixels11"));
    ui.pump(600);
    // 1:1 means one device pixel per data unit.
    EXPECT_NEAR(xform(ui).sx, ui.window()->devicePixelRatioF(), 0.01);
    EXPECT_NEAR(std::abs(xform(ui).sx), std::abs(xform(ui).sy), 1e-6);
}

TEST_F(UIViewer, ZoomInThenFitRestoresTheFitScale)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare));
    ui.viewport()->fitToWindow();
    ui.pump(300);
    const double fit = xform(ui).sx;
    ui.viewport()->zoomIn();
    ui.viewport()->zoomIn();
    ui.pump(300);
    EXPECT_GT(xform(ui).sx, fit * 1.5);
    ui.viewport()->fitToWindow();
    ui.pump(300);
    EXPECT_NEAR(xform(ui).sx, fit, fit * 0.01);
}

TEST_F(UIViewer, CtrlWheelZoomsAndPlainWheelDoesNot)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare));
    const double fit = xform(ui).sx;
    // Default mouse settings: the plain wheel navigates between images ...
    ui.wheel(ui.viewportCenter(), 120);
    EXPECT_DOUBLE_EQ(xform(ui).sx, fit);
    // ... and Ctrl+wheel zooms (animated, so give it time).
    ui.moveMouse(ui.viewportCenter());
    ui.wheel(ui.viewportCenter(), 120, Qt::ControlModifier);
    ui.pump(800);
    const double in = xform(ui).sx;
    EXPECT_GT(in, fit);
    ui.wheel(ui.viewportCenter(), -120, Qt::ControlModifier);
    ui.pump(800);
    EXPECT_LT(xform(ui).sx, in);
}

TEST_F(UIViewer, MiddleDragPans)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare));
    ui.viewport()->zoomIn();
    ui.viewport()->zoomIn();
    ui.pump(600);
    const Xform before = xform(ui);
    ui.moveMouse(ui.viewportCenter());
    ui.drag(ui.viewportCenter(), ui.viewportCenter() + QPoint(60, 40), Qt::MiddleButton);
    const Xform after = xform(ui);
    EXPECT_TRUE(after.shx != before.shx || after.shy != before.shy);
    EXPECT_DOUBLE_EQ(after.sx, before.sx); // panning never changes the scale
}

TEST_F(UIViewer, SetTransformationRoundTrips)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare));
    ui.viewport()->setTransformation(2.0, 2.0, 10.0, -5.0);
    ui.pump(100);
    const Xform t = xform(ui);
    EXPECT_NEAR(t.sx, 2.0, 1e-6);
    EXPECT_NEAR(t.shx, 10.0, 1e-6);
    EXPECT_NEAR(t.shy, -5.0, 1e-6);
}

TEST_F(UIViewer, FlipActionsMirrorTheImage)
{
    // Red in the top-left, blue in the bottom-right of the data (y is down).
    ASSERT_TRUE(ui.loadGivText("$background white\n$marks fsquare\n$mark_size 40\n$noline\n"
                               "$color red\n0 0\n\n$color blue\n100 100\n"));
    auto find = [&](const QImage& img, QRgb c) {
        int sx = 0, sy = 0, n = 0;
        for (int y = 0; y < img.height(); y += 2)
            for (int x = 0; x < img.width(); x += 2)
                if (UITestDriver::isColorNear(img.pixel(x, y), c)) { sx += x; sy += y; ++n; }
        return n ? QPoint(sx / n, sy / n) : QPoint(-1, -1);
    };
    QImage a = ui.screenshot();
    const QPoint redBefore = find(a, qRgb(255, 0, 0));
    ASSERT_NE(redBefore.x(), -1);
    ASSERT_TRUE(ui.trigger("HorizontalFlip", true));
    QImage b = ui.screenshot();
    const QPoint redAfter = find(b, qRgb(255, 0, 0));
    ASSERT_NE(redAfter.x(), -1);
    EXPECT_GT(redAfter.x(), redBefore.x() + 50) << "red mark should have moved to the right";
    EXPECT_NEAR(redAfter.y(), redBefore.y(), 5);
    ASSERT_TRUE(ui.trigger("HorizontalFlip", false));
    ASSERT_TRUE(ui.trigger("VerticalFlip", true));
    const QPoint redV = find(ui.screenshot(), qRgb(255, 0, 0));
    EXPECT_GT(redV.y(), redBefore.y() + 50) << "red mark should have moved to the bottom";
}

TEST_F(UIViewer, ShowMarksToggleHidesMarks)
{
    ASSERT_TRUE(ui.loadGivText("$background white\n$marks fsquare\n$mark_size 60\n$noline\n$color red\n0 0\n"));
    ASSERT_TRUE(ui.trigger("ShowMarks", true));
    EXPECT_GT(UITestDriver::countPixels(ui.screenshot(), qRgb(255, 0, 0)), 200);
    ASSERT_TRUE(ui.trigger("ShowMarks", false));
    EXPECT_EQ(UITestDriver::countPixels(ui.screenshot(), qRgb(255, 0, 0)), 0);
    EXPECT_FALSE(ui.viewport()->showMarks());
}

TEST_F(UIViewer, HiddenDatasetIsNotDrawn)
{
    // The blue square is the control: once it shows, the hidden red one would have too.
    ASSERT_TRUE(ui.loadGivText("$background white\n$hide\n$color red\n$polygon\n-5 -5\n5 -5\n5 5\n-5 5\n-5 -5\n\n"
                               "$color blue\n$polygon\n10 -5\n20 -5\n20 5\n10 5\n10 -5\n"));
    ASSERT_TRUE(ui.waitForColor(qRgb(0, 0, 255), 1000));
    EXPECT_EQ(UITestDriver::countPixels(ui.screenshot(), qRgb(255, 0, 0)), 0);
}

TEST_F(UIViewer, ReplacingTheSceneDropsTheOldOne)
{
    ASSERT_TRUE(ui.loadGivText("$background white\n$color red\n$polygon\n-5 -5\n5 -5\n5 5\n-5 5\n-5 -5\n", "a.giv"));
    ASSERT_TRUE(ui.loadGivText("$background white\n$color green\n$polygon\n-5 -5\n5 -5\n5 5\n-5 5\n-5 -5\n", "b.giv"));
    EXPECT_TRUE(ui.waitForColor(qRgb(0, 128, 0), 1000, 70));
    EXPECT_EQ(UITestDriver::countPixels(ui.screenshot(), qRgb(255, 0, 0)), 0);
}

TEST_F(UIViewer, ExampleFilesLoadAndRender)
{
    for (const char* name : {"example.giv", "marks.giv", "text-markup.giv", "bezier.giv", "drop.svg"})
    {
        SCOPED_TRACE(name);
        ASSERT_TRUE(ui.load(std::string(WIV_EXAMPLES_DIR) + "/" + name)) << qPrintable(ui.error());
        EXPECT_TRUE(ui.viewport()->hasScene());
        const QImage img = ui.screenshot();
        EXPECT_FALSE(img.isNull());
    }
}

TEST_F(UIViewer, DockTogglesFollowActions)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare));
    auto* dock = ui.window()->findChild<QDockWidget*>("MarkBrowserDock");
    ASSERT_NE(dock, nullptr);
    ASSERT_TRUE(ui.trigger("MarkBrowser", true));
    EXPECT_TRUE(dock->isVisible());
    ASSERT_TRUE(ui.trigger("MarkBrowser", false));
    EXPECT_FALSE(dock->isVisible());
}

TEST_F(UIViewer, UnknownActionIsRejected)
{
    EXPECT_FALSE(ui.trigger("NoSuchAction"));
    EXPECT_FALSE(ui.error().isEmpty());
}

// Menu actions that only change view state must be safe to fire at any time,
// with or without a scene loaded (no crash, no hang).
TEST_F(UIViewer, ViewActionsDoNotCrashWithoutAScene)
{
    for (const char* id : {"FittoWindow", "ZoomIn", "ZoomOut", "StepZoomIn", "StepZoomOut", "ActualPixels11", "PanLeft",
                           "PanRight", "PanUp", "PanDown", "RotateLeft", "RotateRight", "NextImage", "PreviousImage",
                           "FirstImage", "LastImage", "NextFrame", "PreviousFrame", "PlayPauseAnimation",
                           "StopAnimation"})
    {
        SCOPED_TRACE(id);
        ui.trigger(id); // may be disabled; only the absence of a crash matters
    }
    SUCCEED();
}

TEST_F(UIViewer, ViewActionsDoNotCrashWithAScene)
{
    ASSERT_TRUE(ui.loadGivText(kRedSquare));
    for (const char* id : {"FittoWindow", "ZoomIn", "ZoomOut", "StepZoomIn", "StepZoomOut", "ActualPixels11", "PanLeft",
                           "PanRight", "PanUp", "PanDown", "RotateLeft", "RotateRight", "ForceOpaque", "PixelGrid",
                           "VerticalZoomLock", "AutoFit", "BalloonTooltips", "MarkBrowser", "ImageInfo",
                           "AdjustContrast", "ColorTable", "CalibratePixelSize", "ThumbnailStrip"})
    {
        SCOPED_TRACE(id);
        ui.trigger(id);
    }
    EXPECT_FALSE(ui.screenshot().isNull());
}
