// wiv - Vulkan-based, giv-format-compatible 2D vector viewer.
//
// Qt application shell (see plan): QMainWindow with menu/toolbar/dataset
// dock panel/status bar/preferences, wrapping a VulkanViewport that owns the
// actual VSG/Vulkan rendering. CLI subset (see plan/README): positional
// .giv file(s); --geometry WxH sets the initial window size.
#include "Logging.h"
#include "qt/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>

#include <spdlog/spdlog.h>

int main(int argc, char** argv)
{
    giv::log::init();

    QApplication app(argc, argv);
    QApplication::setOrganizationName("wiv");
    QApplication::setApplicationName("wiv");

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
    parser.addPositionalArgument("files", "giv file(s) to open.", "[file.giv ...]");
    parser.process(app);

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
