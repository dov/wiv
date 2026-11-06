// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// UITestDriver - in-process driver for GUI tests. Owns a real givqt::MainWindow
// (Qt + Vulkan viewport) and wraps the public remote-driving API the RPC
// server uses (loadFiles / triggerAction / grabScreenshot) plus synthesized
// mouse/keyboard input through QTest.
//
// One QApplication per process: see ui_test_main.cpp, which also points HOME /
// XDG_* at a scratch directory so tests never touch the user's real settings.
#pragma once

#include <QImage>
#include <QPoint>
#include <optional>
#include <QString>
#include <QStringList>
#include <memory>
#include <string>

class QWidget;
class QWindow;

namespace givqt
{
class MainWindow;
class VulkanViewport;
} // namespace givqt

class UITestDriver
{
public:
    // Creates and shows a main window of the given size.
    explicit UITestDriver(int width = 900, int height = 650);
    ~UITestDriver();
    UITestDriver(const UITestDriver&) = delete;
    UITestDriver& operator=(const UITestDriver&) = delete;

    givqt::MainWindow* window() const { return window_.get(); }
    givqt::VulkanViewport* viewport() const { return viewport_; }

    // Loads a file into the viewport; returns false (with error()) on failure.
    bool load(const std::string& path);
    // Writes `text` to a temp .giv file in the scratch dir and loads it.
    bool loadGivText(const std::string& text, const std::string& name = "scene.giv");

    // Menu actions, by id (see MainWindow::actionIds()).
    bool trigger(const QString& id, std::optional<bool> checked = std::nullopt);
    QStringList actionIds() const;
    const QString& error() const { return error_; }

    // Runs the event loop for `ms` milliseconds.
    static void pump(int ms);
    // Pumps until `pred()` is true or `timeoutMs` elapses; returns the final value.
    template <class Pred> static bool waitFor(Pred pred, int timeoutMs = 5000)
    {
        for (int t = 0; t < timeoutMs && !pred(); t += 20) pump(20);
        return pred();
    }

    QImage screenshot(bool viewportOnly = true) const;
    // Polls screenshots until at least `minCount` pixels are near `color`
    // (rendering is asynchronous); returns whether that happened in time.
    bool waitForColor(QRgb color, int minCount, int tol = 40, int timeoutMs = 5000) const;

    // Synthesized input, in viewport-local logical pixels.
    void click(QPoint p, Qt::MouseButton b = Qt::LeftButton, Qt::KeyboardModifiers m = {});
    void doubleClick(QPoint p);
    void moveMouse(QPoint p);
    void drag(QPoint from, QPoint to, Qt::MouseButton b = Qt::LeftButton);
    void wheel(QPoint p, int angleDeltaY, Qt::KeyboardModifiers m = {});
    void key(Qt::Key k, Qt::KeyboardModifiers m = {});

    QPoint viewportCenter() const;
    // The embedded Vulkan render window (where the viewport receives input).
    QWindow* renderWindow() const;
    QPoint toWindow(QPoint viewportPos) const;
    // The scratch dir (the process's fake $HOME).
    static QString scratchDir();

    // Pixel helpers.
    static bool isColorNear(QRgb actual, QRgb expected, int tol = 40);
    // Count of pixels in `img` near `color`.
    static int countPixels(const QImage& img, QRgb color, int tol = 40);

private:
    std::unique_ptr<givqt::MainWindow> window_;
    givqt::VulkanViewport* viewport_ = nullptr;
    QString error_;
};
