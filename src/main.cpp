// wiv - Vulkan-based, giv-format-compatible 2D vector viewer.
//
// Qt application shell (see plan): QMainWindow with menu/toolbar/dataset
// dock panel/status bar/preferences, wrapping a VulkanViewport that owns the
// actual VSG/Vulkan rendering. CLI subset (see plan/README): positional
// .giv file(s); --geometry WxH sets the initial window size.
#include "Logging.h"
#include "SceneBuilder.h"
#include "qt/MainWindow.h"
#include "qt/RemoteClient.h"

#include <QApplication>
#include "qt/PreferencesDialog.h"
#include <QCoreApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <vsg/all.h>
#include <vsgXchange/all.h>

#include <string>

#ifndef _WIN32
#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
int g_sigFd[2] = {-1, -1};

// Async-signal-safe: just poke the socketpair; the Qt side does the real work.
void onTermSignal(int)
{
    const char c = 1;
    const ssize_t n = ::write(g_sigFd[0], &c, 1);
    (void)n;
}
} // namespace
#endif

namespace
{
class KeyLogger : public QObject
{
public:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease ||
            event->type() == QEvent::ShortcutOverride)
        {
            auto* ke = static_cast<QKeyEvent*>(event);
            spdlog::info("Key {} repeat={} ts={} key=0x{:x} text='{}' mods=0x{:x} vk=0x{:x} scan=0x{:x} nativeMods=0x{:x} app mods=0x{:x}",
                         event->type() == QEvent::KeyPress ? "press" : event->type() == QEvent::KeyRelease ? "release" : "shortcut-override",
                         ke->isAutoRepeat(), ke->timestamp(), ke->key(),
                         ke->text().toStdString(), static_cast<int>(ke->modifiers()),
                         ke->nativeVirtualKey(), ke->nativeScanCode(), ke->nativeModifiers(),
                         static_cast<int>(QGuiApplication::keyboardModifiers()));
        }
        return false;
    }
};

// cmd.exe and PowerShell don't expand wildcards (unlike a POSIX shell), so
// expand '*' / '?' patterns in the file name part ourselves. Arguments that
// exist as-is or contain no wildcard pass through unchanged; a pattern that
// matches nothing is also kept so the normal "can't open" error is shown.
QStringList expandWildcards(const QStringList& args)
{
    QStringList out;
    for (const QString& arg : args)
    {
        const QFileInfo info(arg);
        const QString name = info.fileName();
        if (!name.contains('*') && !name.contains('?') && !name.contains('['))
        {
            out << arg;
            continue;
        }
        if (info.exists())
        {
            out << arg;
            continue;
        }
        QDir dir = info.dir();
        const QStringList matches =
            dir.entryList({name}, QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::LocaleAware);
        if (matches.isEmpty())
        {
            out << arg;
            continue;
        }
        for (const QString& m : matches) out << dir.filePath(m);
    }
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    giv::log::init();

    // giv's --zoom takes three separate arguments (scale shift_x shift_y),
    // which QCommandLineParser can't express: pull them out of argv first.
    std::vector<char*> filteredArgv;
    bool hasZoom = false;
    bool hasGeometryArg = false;
    QString geometryArg;
    double zoomValues[3] = {1, 0, 0};
    for (int i = 0; i < argc; ++i)
    {
        if (i > 0 && std::string(argv[i]) == "--zoom" && i + 3 < argc)
        {
            hasZoom = true;
            for (int k = 0; k < 3; ++k) zoomValues[k] = QString(argv[i + 1 + k]).toDouble();
            i += 3;
            continue;
        }
        // Qt's xcb platform plugin consumes -geometry/--geometry itself (as
        // an X11 window geometry, which the window ignores here), so take
        // it out of argv before QApplication sees it.
        if (i > 0 && std::string(argv[i]) == "--geometry" && i + 1 < argc)
        {
            geometryArg = argv[i + 1];
            hasGeometryArg = true;
            ++i;
            continue;
        }
        filteredArgv.push_back(argv[i]);
    }
    int filteredArgc = static_cast<int>(filteredArgv.size());
    filteredArgv.push_back(nullptr);

    // --remote / --json_method: act as a command-line client of a running
    // viewer and exit, without opening a window.
    {
        QCoreApplication core(filteredArgc, filteredArgv.data());
        QCommandLineParser remoteParser;
        remoteParser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        QCommandLineOption remoteOpt("remote", "", "cmd");
        QCommandLineOption jsonMethodOpt("json_method", "", "cmd");
        QCommandLineOption jsonParamsOpt("json_params", "", "prms");
        QCommandLineOption hostOpt("remote-host", "", "host", "localhost");
        QCommandLineOption portOpt("remote-port", "", "port", "8448");
        remoteParser.addOptions({remoteOpt, jsonMethodOpt, jsonParamsOpt, hostOpt, portOpt});
        // Everything else is for the GUI parser below.
        remoteParser.parse(core.arguments());
        if (remoteParser.isSet(remoteOpt) || remoteParser.isSet(jsonMethodOpt))
        {
            QString method, params;
            if (remoteParser.isSet(jsonMethodOpt))
            {
                method = remoteParser.value(jsonMethodOpt);
                params = remoteParser.value(jsonParamsOpt);
            }
            else
                givqt::splitRemoteCommand(remoteParser.value(remoteOpt), method, params);
            return givqt::runRemoteClient(remoteParser.value(hostOpt), remoteParser.value(portOpt).toInt(), method,
                                          params) == 0
                       ? 0
                       : 255;
        }
    }

    QApplication app(filteredArgc, filteredArgv.data());
    QApplication::setOrganizationName("wiv");
    QApplication::setApplicationName("wiv");
    QApplication::setWindowIcon(QIcon(":/wiv-app-icon.png"));
    givqt::PreferencesDialog::applyTheme();

    // Diagnostic: WIV_LOG_KEYS=1 logs every key press with the modifiers Qt
    // sees and the native virtual key / scan code (e.g. to debug key
    // remappers such as PowerToys).
    KeyLogger keyLogger;
    if (qEnvironmentVariableIsSet("WIV_LOG_KEYS"))
        app.installEventFilter(&keyLogger);

    {
        QStringList argList;
        for (int i = 0; i < argc; ++i) argList << argv[i];
        spdlog::info("Command line: {}", argList.join(' ').toStdString());
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("Vulkan-based, giv-format-compatible 2D vector viewer");
    parser.addHelpOption();
    QCommandLineOption geometryOption("geometry", "Initial window size WxH.", "WxH");
    parser.addOption(geometryOption);
    QCommandLineOption rpcPortOption(
        "rpc-port", "Start the json-rpc remote-control server on 127.0.0.1:PORT (see RpcServer.h). Not started by default.",
        "PORT");
    parser.addOption(rpcPortOption);
    QCommandLineOption portOption("port", "Alias of --rpc-port (giv's name for it).", "PORT");
    parser.addOption(portOption);
    QCommandLineOption joinOption("join", "Merge all the files on the command line into one scene.");
    parser.addOption(joinOption);
    QCommandLineOption autoReloadOption("auto-reload", "Start with View > Auto Reload on.");
    parser.addOption(autoReloadOption);
    QCommandLineOption actualPixelsOption("n", "Initially use 1:1 zoom instead of fitting to the window.");
    parser.addOption(actualPixelsOption);
    QCommandLineOption exportOption("export", "Render to this image file (size from --geometry, default 800x600) and exit.", "file");
    parser.addOption(exportOption);
    QCommandLineOption logFileOption("log_file", "Also log to this file.", "file");
    parser.addOption(logFileOption);
    // Handled above, before QApplication; declared so --help lists them and
    // the parser doesn't reject them.
    parser.addOption(QCommandLineOption("zoom", "Initial view: --zoom scale shift_x shift_y (pixels per unit, view center).",
                                        "scale"));
    for (const char* name : {"remote", "json_method", "json_params", "remote-host", "remote-port"})
        parser.addOption(QCommandLineOption(name, "Remote client mode: send a json-rpc command to a running viewer.", "arg"));
    parser.addPositionalArgument("files", "giv file(s) to open.", "[file.giv ...]");
    parser.process(app);
    giv::log::captureStdStreams();

    if (parser.isSet(logFileOption))
    {
        try
        {
            spdlog::default_logger()->sinks().push_back(
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(parser.value(logFileOption).toStdString(), true));
        }
        catch (const spdlog::spdlog_ex& e)
        {
            spdlog::error("Cannot open --log_file: {}", e.what());
        }
    }

    givqt::MainWindow window;

    if (hasGeometryArg)
    {
        QString geom = geometryArg;
        auto xpos = geom.indexOf('x', 0, Qt::CaseInsensitive);
        if (xpos > 0)
        {
            bool okW = false, okH = false;
            int w = geom.left(xpos).toInt(&okW);
            int h = geom.mid(xpos + 1).toInt(&okH);
            if (okW && okH)
            {
                window.resize(w, h);
                spdlog::info("Initial window geometry set from --geometry: {}x{}", w, h);
            }
        }
    }

    // Shown before loading any command-line file(s) below (reversing the
    // previous load-then-show order): VulkanViewport's constructor (run
    // inside the givqt::MainWindow construction above) already compiles and
    // presents one empty frame while the embedded viewport is still
    // unmapped, which is what actually matters for the NVIDIA present hang
    // this ordering used to work around (see VulkanViewport::loadFiles()'s
    // doc comment on its render() call) - a driver present() call only ever
    // hung on the *very first* present of the window's life, done while
    // already mapped/visible; every present after that first one, mapped or
    // not, is fine. So that invariant is already satisfied by the time we
    // get here, and showing first means a command-line file that fails to
    // load (bad path, parse error) pops its error dialog over an already-
    // visible (empty-canvas) main window, instead of over one that's not
    // shown yet - which otherwise left the dialog an orphaned, unresponsive
    // top-level with no visible parent underneath it.
    if (parser.isSet(exportOption))
    {
        int w = 800, h = 600;
        const QString geom = geometryArg;
        const int xpos = geom.indexOf('x', 0, Qt::CaseInsensitive);
        if (hasGeometryArg && xpos > 0)
        {
            w = geom.left(xpos).toInt();
            h = geom.mid(xpos + 1).toInt();
        }
        window.setStartupExport(parser.value(exportOption), w, h);
    }

    window.show();

    if (parser.isSet(autoReloadOption)) window.enableAutoReload();
    if (hasZoom) window.setStartupZoom(zoomValues[0], zoomValues[1], zoomValues[2]);
    else if (parser.isSet(actualPixelsOption)) window.setStartupActualPixels();

    if (parser.isSet(rpcPortOption) || parser.isSet(portOption))
    {
        const QString portText = parser.isSet(rpcPortOption) ? parser.value(rpcPortOption) : parser.value(portOption);
        bool ok = false;
        int port = portText.toInt(&ok);
        if (ok && port > 0 && port <= 65535)
            window.startRpcServer(port);
        else
            spdlog::error("Invalid --rpc-port value: {}", portText.toStdString());
    }

    auto files = parser.positionalArguments();
    if (!files.isEmpty())
    {
        std::vector<std::string> paths;
        for (const auto& f : expandWildcards(files)) paths.push_back(f.toStdString());
        spdlog::info("Opening {} file(s) from command line: {}", paths.size(), files.join(", ").toStdString());
        if (parser.isSet(joinOption))
            window.loadFilesJoined(paths);
        else
            window.loadFiles(paths);
    }

    else
        window.startWithoutFile();

    spdlog::info("Main window shown, entering event loop");
#ifndef _WIN32
    // Ctrl+C (SIGINT) / SIGTERM: quit through the event loop so windows close cleanly.
    QSocketNotifier* sigNotifier = nullptr;
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd) == 0)
    {
        sigNotifier = new QSocketNotifier(g_sigFd[1], QSocketNotifier::Read, &app);
        QObject::connect(sigNotifier, &QSocketNotifier::activated, &app, [&]() {
            char c;
            const ssize_t n = ::read(g_sigFd[1], &c, 1);
            (void)n;
            spdlog::info("Termination signal received, quitting");
            QApplication::quit();
        });
        struct sigaction sa = {};
        sa.sa_handler = onTermSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }
#endif
    int rc = app.exec();
    spdlog::info("Application exiting with code {}", rc);
    return rc;
}
