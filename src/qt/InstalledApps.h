#pragma once
//
// InstalledApps.h - the applications the operating system offers for opening
// a given file, for File > Open With: on Windows the shell's recommended
// handlers for the file's extension (SHAssocEnumHandlers); elsewhere the
// freedesktop .desktop applications registered for the file's MIME type
// (mimeapps.list and mimeinfo.cache). The MIME type is found from the name
// and the file's content, so files without an extension are covered too.
//
#include <QIcon>
#include <QList>
#include <QString>

namespace givqt
{

struct InstalledApp
{
    QString name;
    QString command; // as for ExternalApps::launch(): program and arguments, "%f" is the file
    QIcon icon;
};

// Best candidates first (the system's preferred one, then the others); empty
// if the system knows no application for the file.
QList<InstalledApp> installedAppsFor(const QString& path);

// The applications registered for any image type: what the Preferences list
// offers to add to Open With. Sorted by name, one entry per application.
QList<InstalledApp> installedImageApps();

// The icon of the application a saved command runs: the entry of `known`
// with that command or program, else the program's own icon. Null if none.
QIcon iconForCommand(const QString& command, const QList<InstalledApp>& known);

} // namespace givqt
