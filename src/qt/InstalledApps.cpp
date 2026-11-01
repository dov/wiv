#include "InstalledApps.h"

#include <QFileInfo>
#include <QProcess>

#include <algorithm>

#ifdef _WIN32
#include <QFileIconProvider>

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <QDir>
#include <functional>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTextStream>
#endif

namespace givqt
{

#ifdef _WIN32

namespace
{
QList<InstalledApp> appsForSuffix(const QString& suffix)
{
    QList<InstalledApp> out;
    if (suffix.isEmpty()) return out; // the shell associates by extension only

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IEnumAssocHandlers* handlers = nullptr;
    const QString ext = '.' + suffix;
    if (SUCCEEDED(SHAssocEnumHandlers(reinterpret_cast<LPCWSTR>(ext.utf16()), ASSOC_FILTER_RECOMMENDED, &handlers)))
    {
        IAssocHandler* h = nullptr;
        ULONG fetched = 0;
        while (handlers->Next(1, &h, &fetched) == S_OK && fetched == 1)
        {
            PWSTR exe = nullptr;
            PWSTR ui = nullptr;
            if (SUCCEEDED(h->GetName(&exe)) && SUCCEEDED(h->GetUIName(&ui)))
            {
                const QString exePath = QString::fromWCharArray(exe);
                InstalledApp app;
                app.name = QString::fromWCharArray(ui);
                app.command = '"' + exePath + '"';
                app.icon = QFileIconProvider().icon(QFileInfo(exePath));
                out << app;
            }
            if (exe) CoTaskMemFree(exe);
            if (ui) CoTaskMemFree(ui);
            h->Release();
        }
        handlers->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return out;
}
} // namespace

QList<InstalledApp> installedAppsFor(const QString& path) { return appsForSuffix(QFileInfo(path).suffix()); }

QList<InstalledApp> installedImageApps()
{
    QList<InstalledApp> out;
    QStringList seen;
    for (const char* ext : {"jpg", "png", "gif", "bmp", "tif", "webp", "ico", "svg", "heic", "avif"})
        for (const InstalledApp& app : appsForSuffix(ext))
            if (!seen.contains(app.command))
            {
                seen << app.command;
                out << app;
            }
    std::sort(out.begin(), out.end(), [](const InstalledApp& a, const InstalledApp& b) { return a.name.toLower() < b.name.toLower(); });
    return out;
}

#else

namespace
{
// "key=a;b;c" lists of a freedesktop file: group -> key -> desktop ids.
using IdLists = QHash<QString, QHash<QString, QStringList>>;

IdLists readIdLists(const QString& file)
{
    IdLists lists;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return lists;
    QTextStream in(&f);
    QString group;
    while (!in.atEnd())
    {
        const QString line = in.readLine().trimmed();
        if (line.startsWith('[') && line.endsWith(']')) group = line.mid(1, line.size() - 2);
        else if (!line.startsWith('#') && line.contains('='))
        {
            const int eq = line.indexOf('=');
            lists[group][line.left(eq)] = line.mid(eq + 1).split(';', Qt::SkipEmptyParts);
        }
    }
    return lists;
}

QStringList applicationDirs()
{
    return QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
}

// The key/value pairs of a .desktop file's [Desktop Entry] group.
QHash<QString, QString> readDesktopEntry(const QString& file)
{
    QHash<QString, QString> entry;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return entry;
    QTextStream in(&f);
    bool inEntry = false;
    while (!in.atEnd())
    {
        const QString line = in.readLine().trimmed();
        if (line.startsWith('['))
        {
            inEntry = line == "[Desktop Entry]";
            continue;
        }
        const int eq = line.indexOf('=');
        if (inEntry && eq > 0 && !line.startsWith('#')) entry[line.left(eq).trimmed()] = line.mid(eq + 1).trimmed();
    }
    return entry;
}

// Exec= with the file-argument codes turned into our "%f"; other codes dropped.
QString commandFromExec(const QString& exec)
{
    QString out;
    for (int i = 0; i < exec.size(); ++i)
    {
        if (exec[i] != '%' || i + 1 == exec.size())
        {
            out += exec[i];
            continue;
        }
        const QChar code = exec[++i];
        if (code == 'f' || code == 'F' || code == 'u' || code == 'U') out += "%f";
        else if (code == '%') out += '%';
    }
    return out.trimmed();
}
} // namespace

namespace
{
// Applications for the .desktop `ids`, in order, one per id that can be run.
QList<InstalledApp> appsFromIds(const QStringList& ids, const QStringList& removed)
{
    QList<InstalledApp> out;
    QStringList seen;
    const QString locale = QLocale().name(); // "en_US"
    for (const QString& id : ids)
    {
        if (seen.contains(id) || removed.contains(id)) continue;
        seen << id;
        QHash<QString, QString> entry;
        for (const QString& dir : applicationDirs())
        {
            entry = readDesktopEntry(dir + '/' + id);
            if (!entry.isEmpty()) break;
        }
        if (entry.value("Exec").isEmpty() || entry.value("Hidden") == "true" || entry.value("Terminal") == "true")
            continue;
        InstalledApp app;
        app.name = entry.value("Name[" + locale + "]", entry.value("Name[" + locale.section('_', 0, 0) + "]", entry.value("Name")));
        if (app.name.isEmpty()) app.name = id;
        app.command = commandFromExec(entry.value("Exec"));
        const QString icon = entry.value("Icon");
        app.icon = QFile::exists(icon) ? QIcon(icon) : QIcon::fromTheme(icon);
        out << app;
    }
    return out;
}

// The .desktop ids associated with the MIME types accepted by `wanted`: the
// user's chosen defaults, added associations, then everything that declares
// the type. `removed` gets the ids the user took away.
QStringList idsForMimes(const std::function<bool(const QString&)>& wanted, QStringList* removed)
{
    QStringList configFiles;
    for (const QString& dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
        configFiles << dir + "/mimeapps.list";
    for (const QString& dir : applicationDirs()) configFiles << dir + "/mimeapps.list";
    QList<IdLists> mimeapps;
    for (const QString& f : configFiles) mimeapps << readIdLists(f);
    QList<IdLists> caches;
    for (const QString& dir : applicationDirs()) caches << readIdLists(dir + "/mimeinfo.cache");

    QStringList ids;
    const auto collect = [&](const IdLists& l, const char* group, QStringList& into) {
        const auto& g = l[group];
        for (auto it = g.cbegin(); it != g.cend(); ++it)
            if (wanted(it.key())) into << it.value();
    };
    for (const IdLists& l : mimeapps) collect(l, "Removed Associations", *removed);
    for (const IdLists& l : mimeapps) collect(l, "Default Applications", ids);
    for (const IdLists& l : mimeapps) collect(l, "Added Associations", ids);
    for (const IdLists& l : caches) collect(l, "MIME Cache", ids);
    return ids;
}
} // namespace

QList<InstalledApp> installedAppsFor(const QString& path)
{
    QMimeDatabase db;
    const QMimeType type = db.mimeTypeForFile(path);
    QStringList mimes{type.name()};
    mimes << type.allAncestors();
    mimes.removeAll("application/octet-stream");
    QStringList removed;
    // One MIME type at a time, so a type's own handlers come before its ancestors'.
    QStringList ids;
    for (const QString& mime : mimes) ids << idsForMimes([&mime](const QString& m) { return m == mime; }, &removed);
    return appsFromIds(ids, removed);
}

QList<InstalledApp> installedImageApps()
{
    QStringList removed;
    const QStringList ids = idsForMimes([](const QString& m) { return m.startsWith("image/"); }, &removed);
    QList<InstalledApp> apps = appsFromIds(ids, removed);
    // The same program is often registered twice (plain and with a plugin flag).
    QStringList seen;
    QList<InstalledApp> out;
    for (const InstalledApp& app : apps)
        if (!seen.contains(app.name))
        {
            seen << app.name;
            out << app;
        }
    std::sort(out.begin(), out.end(), [](const InstalledApp& a, const InstalledApp& b) { return a.name.toLower() < b.name.toLower(); });
    return out;
}

#endif

QIcon iconForCommand(const QString& command, const QList<InstalledApp>& known)
{
    for (const InstalledApp& app : known)
        if (app.command == command) return app.icon;
    const QString program = QProcess::splitCommand(command).value(0);
    const QString base = QFileInfo(program).completeBaseName();
    if (base.isEmpty()) return {};
    for (const InstalledApp& app : known)
        if (QFileInfo(QProcess::splitCommand(app.command).value(0)).completeBaseName() == base) return app.icon;
#ifdef _WIN32
    if (QFileInfo::exists(program)) return QFileIconProvider().icon(QFileInfo(program));
    return {};
#else
    return QIcon::fromTheme(base);
#endif
}

} // namespace givqt
