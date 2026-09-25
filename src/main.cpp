// wiv - Vulkan-based, giv-format-compatible 2D vector viewer.
//
// Qt application shell (see plan): QMainWindow with menu/toolbar/dataset
// dock panel/status bar/preferences, wrapping a VulkanViewport that owns the
// actual VSG/Vulkan rendering. CLI subset (see plan/README): positional
// .giv file(s); --geometry WxH sets the initial window size.
#include "qt/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("wiv");
    QApplication::setApplicationName("wiv");

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
            if (okW && okH) window.resize(w, h);
        }
    }

    auto files = parser.positionalArguments();
    if (!files.isEmpty())
    {
        std::vector<std::string> paths;
        for (const auto& f : files) paths.push_back(f.toStdString());
        // Called before window.show() (matching the reference application's Widget3D
        // construction order): the scene is parsed/built and the first
        // Vulkan frame is compiled/recorded/presented while the embedded
        // viewport is still unmapped, then show() reveals the
        // already-rendered window in one shot. Doing this the other way
        // around - showing the window first and presenting into it once
        // already mapped/visible - reliably hung forever in the NVIDIA
        // driver's present() path (see VulkanViewport::loadFiles()'s doc
        // comment on the render() call it makes for this same reason).
        window.loadFiles(paths);
    }

    window.show();

    return app.exec();
}
