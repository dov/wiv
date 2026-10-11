// wiv - Vulkan-based, giv-format-compatible 2D vector viewer.
//
// Qt application shell (see plan): QMainWindow with menu/toolbar/dataset
// dock panel/status bar/preferences, wrapping a VulkanViewport that owns the
// actual VSG/Vulkan rendering. CLI subset (see plan/README): positional
// .giv file(s); --geometry WxH sets the initial window size.
#include "Logging.h"
#include "SceneBuilder.h"
#include "qt/MainWindow.h"

#include <QApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QCommandLineParser>

#include <spdlog/spdlog.h>

#include <vsg/all.h>
#include <vsgXchange/all.h>
#include <vsgXchange/freetype.h>

#include <string>

namespace
{
class KeyLogger : public QObject
{
public:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
        {
            auto* ke = static_cast<QKeyEvent*>(event);
            spdlog::info("Key {} key=0x{:x} text='{}' mods=0x{:x} vk=0x{:x} scan=0x{:x} nativeMods=0x{:x} app mods=0x{:x}",
                         event->type() == QEvent::KeyPress ? "press" : "release", ke->key(),
                         ke->text().toStdString(), static_cast<int>(ke->modifiers()),
                         ke->nativeVirtualKey(), ke->nativeScanCode(), ke->nativeModifiers(),
                         static_cast<int>(QGuiApplication::keyboardModifiers()));
        }
        return false;
    }
};
} // namespace

int main(int argc, char** argv)
{
    giv::log::init();

    // --warm-fonts: rasterize the stock font faces into the on-disk font
    // cache and exit, so the first real scene doesn't pay ~3 s per face (see
    // SceneBuilder::resolveFont). Run by the Windows installer after copying
    // files. Handled before QApplication: no GUI is needed.
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) != "--warm-fonts") continue;
        auto options = vsg::Options::create();
        options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
        options->add(vsgXchange::all::create());
        options->setValue(vsgXchange::freetype::texel_margin_ratio, 0.5f);
        options->setValue(vsgXchange::freetype::quad_margin_ratio, 0.25f);
        giv::SceneBuilder builder(options);
        for (const char* family : {"Sans", "Serif", "Monospace"})
            for (const char* style : {"", " Bold", " Italic", " Bold Italic"})
            {
                double size = -1;
                builder.resolveFont(std::string(family) + style, size);
            }
        return 0;
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName("wiv");
    QApplication::setApplicationName("wiv");
    QApplication::setWindowIcon(QIcon(":/wiv-app-icon.png"));

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
    parser.addPositionalArgument("files", "giv file(s) to open.", "[file.giv ...]");
    parser.process(app);
    giv::log::captureStdStreams();

    givqt::MainWindow window;

    if (parser.isSet(geometryOption))
    {
        QString geom = parser.value(geometryOption);
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
    window.show();

    if (parser.isSet(rpcPortOption))
    {
        bool ok = false;
        int port = parser.value(rpcPortOption).toInt(&ok);
        if (ok && port > 0 && port <= 65535)
            window.startRpcServer(port);
        else
            spdlog::error("Invalid --rpc-port value: {}", parser.value(rpcPortOption).toStdString());
    }

    auto files = parser.positionalArguments();
    if (!files.isEmpty())
    {
        std::vector<std::string> paths;
        for (const auto& f : files) paths.push_back(f.toStdString());
        spdlog::info("Opening {} file(s) from command line: {}", paths.size(), files.join(", ").toStdString());
        window.loadFiles(paths);
    }

    spdlog::info("Main window shown, entering event loop");
    int rc = app.exec();
    spdlog::info("Application exiting with code {}", rc);
    return rc;
}
