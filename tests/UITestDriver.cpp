// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "UITestDriver.h"

#include "qt/MainWindow.h"
#include "qt/VulkanViewport.h"

#include <vsgQt/Window.h>

#include <QApplication>
#include <gtest/gtest.h>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QTest>
#include <QWindow>
#include <QTimer>

#include <cstdlib>
#include <optional>

UITestDriver::UITestDriver(int width, int height)
{
    // Every test starts from default preferences / view state: some actions
    // (e.g. Show Marks) persist in QSettings and would leak into the next test.
    {
        QSettings s(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv");
        s.clear();
        s.sync();
    }
    window_ = std::make_unique<givqt::MainWindow>();
    window_->resize(width, height);
    window_->show();
    viewport_ = window_->findChild<givqt::VulkanViewport*>();
    pump(300);
}

UITestDriver::~UITestDriver()
{
    window_.reset();
    pump(50);
}

QString UITestDriver::scratchDir() { return QString::fromLocal8Bit(qgetenv("WIV_TEST_SCRATCH")); }

bool UITestDriver::load(const std::string& path)
{
    QString err;
    const bool ok = viewport_->loadFiles({path}, &err);
    error_ = err;
    pump(200);
    return ok;
}

bool UITestDriver::loadGivText(const std::string& text, const std::string& name)
{
    // Unique per test (and per name): wiv remembers per-file view state in a DB
    // keyed by path, which must not leak from one test into the next.
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const QString unique = QString("%1_%2_%3").arg(info ? info->test_suite_name() : "x", info ? info->name() : "x",
                                                   QString::fromStdString(name));
    const QString path = QDir(scratchDir()).filePath(unique);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(text.data(), static_cast<qint64>(text.size()));
    f.close();
    return load(path.toStdString());
}

bool UITestDriver::trigger(const QString& id, std::optional<bool> checked)
{
    error_.clear();
    const bool ok = window_->triggerAction(id, checked, error_);
    pump(100);
    return ok;
}

QStringList UITestDriver::actionIds() const { return window_->actionIds(); }

void UITestDriver::pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QImage UITestDriver::screenshot(bool viewportOnly) const { return window_->grabScreenshot(viewportOnly); }

QPoint UITestDriver::viewportCenter() const { return QPoint(viewport_->width() / 2, viewport_->height() / 2); }

// The Vulkan surface is a separate QWindow (vsgQt::Window) embedded in the viewport
// widget through a window container, and the viewport's pan/zoom/pick input
// is handled on that window - so synthesized input goes to it, not to the widget.
QWindow* UITestDriver::renderWindow() const
{
    for (QWindow* w : QGuiApplication::allWindows())
        if (dynamic_cast<vsgQt::Window*>(w) && w->isVisible()) return w;
    return nullptr;
}

QPoint UITestDriver::toWindow(QPoint viewportPos) const
{
    QWindow* w = renderWindow();
    return w ? w->mapFromGlobal(viewport_->mapToGlobal(viewportPos)) : viewportPos;
}

void UITestDriver::click(QPoint p, Qt::MouseButton b, Qt::KeyboardModifiers m)
{
    QTest::mouseClick(renderWindow(), b, m, toWindow(p));
    pump(50);
}

void UITestDriver::moveMouse(QPoint p)
{
    QTest::mouseMove(renderWindow(), toWindow(p));
    pump(100);
}

bool UITestDriver::waitForColor(QRgb color, int minCount, int tol, int timeoutMs) const
{
    QElapsedTimer timer;
    timer.start();
    do
    {
        if (countPixels(screenshot(), color, tol) >= minCount) return true;
    } while (timer.elapsed() < timeoutMs);
    return false;
}

void UITestDriver::doubleClick(QPoint p)
{
    QTest::mouseDClick(renderWindow(), Qt::LeftButton, {}, toWindow(p));
    pump(50);
}

void UITestDriver::drag(QPoint from, QPoint to, Qt::MouseButton b)
{
    QWindow* w = renderWindow();
    QTest::mousePress(w, b, {}, toWindow(from));
    pump(30);
    const int steps = 8;
    for (int i = 1; i <= steps; ++i)
    {
        QTest::mouseMove(w, toWindow(from + (to - from) * i / steps));
        pump(30);
    }
    QTest::mouseRelease(w, b, {}, toWindow(to));
    pump(100);
}

void UITestDriver::wheel(QPoint p, int angleDeltaY, Qt::KeyboardModifiers m)
{
    QWindow* w = renderWindow();
    const QPoint wp = toWindow(p);
    QWheelEvent ev(QPointF(wp), QPointF(w->mapToGlobal(wp)), QPoint(), QPoint(0, angleDeltaY), Qt::NoButton, m,
                   Qt::NoScrollPhase, false);
    QGuiApplication::sendEvent(w, &ev);
    pump(200);
}

void UITestDriver::key(Qt::Key k, Qt::KeyboardModifiers m)
{
    QTest::keyClick(window_.get(), k, m);
    pump(100);
}

bool UITestDriver::isColorNear(QRgb a, QRgb e, int tol)
{
    return std::abs(qRed(a) - qRed(e)) <= tol && std::abs(qGreen(a) - qGreen(e)) <= tol &&
           std::abs(qBlue(a) - qBlue(e)) <= tol;
}

int UITestDriver::countPixels(const QImage& img, QRgb color, int tol)
{
    const QImage c = img.convertToFormat(QImage::Format_RGB32);
    int n = 0;
    for (int y = 0; y < c.height(); ++y)
    {
        const QRgb* row = reinterpret_cast<const QRgb*>(c.constScanLine(y));
        for (int x = 0; x < c.width(); ++x)
            if (isColorNear(row[x], color, tol)) ++n;
    }
    return n;
}
