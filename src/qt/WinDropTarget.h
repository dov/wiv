#pragma once

// WinDropTarget.h - Windows-only: accept dropped files on a top-level window
// with a custom OLE IDropTarget instead of Qt's, so we can tell the shell to
// show no "Copy" drop-effect label next to the cursor (Qt's own target gives
// no access to the incoming IDataObject, which is where that is set).

#include <QStringList>
#include <QWidget>

#include <functional>

namespace givqt
{

// Registers a drop target on `window`'s native handle (creating it if
// needed). `onDrop` runs later from the event loop, not inside OLE's modal
// drag loop, so it's safe to do a slow load from it. Returns false on failure.
bool installFileDropTarget(QWidget* window, std::function<void(QStringList)> onDrop);

} // namespace givqt
