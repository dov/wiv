#include "ToggleCapture.h"

#include <QDir>
#include <QImage>
#include <QStandardPaths>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace givqt
{

bool toggleCaptureEnabled()
{
    static const bool enabled = std::getenv("WIV_CAPTURE") != nullptr;
    return enabled;
}

#ifdef _WIN32
namespace
{
long long wallMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
} // namespace

void startToggleCapture(void* hwndPtr, const QString& label, int durationMs)
{
    HWND hwnd = static_cast<HWND>(hwndPtr);
    RECT r{};
    if (!hwnd || !GetWindowRect(hwnd, &r)) return;
    const std::string dir = (QDir::homePath() + "/.wiv/captures/" + label).toStdString();
    spdlog::info("[trace] capture '{}' start: window {}x{} at {},{}", label.toStdString(), r.right - r.left, r.bottom - r.top,
                 r.left, r.top);

    std::thread([r, dir, durationMs, label]() {
        // Bottom 55% of the window: where the strip / status bar are.
        const int fullW = r.right - r.left;
        const int fullH = r.bottom - r.top;
        const int y0 = r.top + fullH * 45 / 100;
        const int w = fullW;
        const int h = r.bottom - y0;

        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);

        struct Frame
        {
            long long ms;
            QImage img;
        };
        std::vector<Frame> frames;
        const long long t0 = wallMs();
        while (wallMs() - t0 < durationMs)
        {
            const long long t = wallMs();
            BitBlt(mem, 0, 0, w, h, screen, r.left, y0, SRCCOPY | CAPTUREBLT);
            QImage img(w, h, QImage::Format_ARGB32);
            std::memcpy(img.bits(), bits, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
            frames.push_back({t, std::move(img)});
        }
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);

        QDir().mkpath(QString::fromStdString(dir));
        for (size_t i = 0; i < frames.size(); ++i)
        {
            frames[i].img.save(QString::fromStdString(dir) + QString("/f%1_%2.png").arg(i, 3, 10, QChar('0')).arg(frames[i].ms));
        }
        spdlog::info("[trace] capture '{}' done: {} frames in {} ms", label.toStdString(), frames.size(), durationMs);
    }).detach();
}
#else
void startToggleCapture(void*, const QString&, int) {}
#endif

} // namespace givqt
