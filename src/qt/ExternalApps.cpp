#include "ExternalApps.h"

#include <QDesktopServices>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>

namespace givqt::ExternalApps
{

namespace
{
QSettings settings() { return QSettings(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv"); }

QStringList splitExtensions(const QString& text)
{
    QStringList out;
    for (QString e : text.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts))
    {
        e = e.toLower();
        const bool isFormat = e.startsWith('@');
        if (isFormat) e.remove(0, 1);
        while (e.startsWith('*') || e.startsWith('.')) e.remove(0, 1);
        if (!e.isEmpty()) out << (isFormat ? '@' + e : e);
    }
    return out;
}
} // namespace

Config load()
{
    auto s = settings();
    Config c;
    c.defaultCommand = s.value("externalApps/default").toString();
    const int nApps = s.beginReadArray("externalApps/apps");
    for (int i = 0; i < nApps && i < kMaxApps; ++i)
    {
        s.setArrayIndex(i);
        c.apps.append({s.value("name").toString(), s.value("command").toString()});
    }
    s.endArray();
    const int n = s.beginReadArray("externalApps/rules");
    for (int i = 0; i < n; ++i)
    {
        s.setArrayIndex(i);
        c.rules.append({s.value("extensions").toString(), s.value("command").toString()});
    }
    s.endArray();
    return c;
}

void save(const Config& c)
{
    auto s = settings();
    s.setValue("externalApps/default", c.defaultCommand);
    s.beginWriteArray("externalApps/apps", c.apps.size());
    for (int i = 0; i < c.apps.size(); ++i)
    {
        s.setArrayIndex(i);
        s.setValue("name", c.apps[i].name);
        s.setValue("command", c.apps[i].command);
    }
    s.endArray();
    s.beginWriteArray("externalApps/rules", c.rules.size());
    for (int i = 0; i < c.rules.size(); ++i)
    {
        s.setArrayIndex(i);
        s.setValue("extensions", c.rules[i].extensions);
        s.setValue("command", c.rules[i].command);
    }
    s.endArray();
}

QString commandFor(const Config& c, const QString& path, const QString& format)
{
    const QString name = path.section('/', -1).toLower();
    qsizetype bestLen = 0;
    QString best = c.defaultCommand;
    for (const Rule& r : c.rules)
        for (const QString& ext : splitExtensions(r.extensions))
        {
            // An extension match outranks a format match; a longer extension outranks a shorter.
            qsizetype score = 0;
            if (ext.startsWith('@')) score = (!format.isEmpty() && ext.mid(1) == format.toLower()) ? 1 : 0;
            else if (name.endsWith('.' + ext)) score = ext.size() + 1;
            if (score > bestLen)
            {
                bestLen = score;
                best = r.command;
            }
        }
    return best;
}

bool launch(const QString& command, const QString& path, QString* error)
{
    auto fail = [error](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (command.trimmed().isEmpty())
    {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            return fail("The system has no default application for this file.");
        return true;
    }
    QStringList args = QProcess::splitCommand(command);
    if (args.isEmpty()) return fail("The command is empty.");
    const QString program = args.takeFirst();
    bool hasPlaceholder = false;
    for (QString& a : args)
        if (a.contains("%f"))
        {
            a.replace("%f", path);
            hasPlaceholder = true;
        }
    if (!hasPlaceholder) args << path;
    if (!QProcess::startDetached(program, args)) return fail(QString("Could not start \"%1\".").arg(program));
    return true;
}

} // namespace givqt::ExternalApps
