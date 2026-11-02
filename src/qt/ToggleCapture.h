#pragma once

// Diagnostic only (enabled by the environment variable WIV_CAPTURE=1): grabs
// frames of the application's own window from the real, composed desktop around
// a layout change, to see exactly what the user sees - including what only the
// window compositor shows, which no in-process render can reveal. Frames and a
// manifest are written to ~/.wiv/captures/<label>/ for offline analysis.

#include <QString>

namespace givqt
{

// True if WIV_CAPTURE is set.
bool toggleCaptureEnabled();

// Starts capturing the bottom part of the window `hwnd` (a native window
// handle) for `durationMs` on a worker thread. Returns immediately.
void startToggleCapture(void* hwnd, const QString& label, int durationMs = 700);

} // namespace givqt
