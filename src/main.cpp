// wiv - Vulkan-based, giv-format-compatible 2D vector viewer.
//
// Phase 1 CLI subset (see plan): positional .giv file(s), --geometry WxH,
// --zoom scale shift_x shift_y, -n (disable auto-fit-to-bounds).
#include "GivParser.h"
#include "PanZoomHandler.h"
#include "SceneBuilder.h"

#include <vsg/all.h>
#include <vsgXchange/all.h>
#include <vsgXchange/freetype.h>

#include <chrono>
#include <iostream>

namespace
{

bool parseGeometry(const std::string& s, uint32_t& w, uint32_t& h)
{
    auto xpos = s.find_first_of("xX");
    if (xpos == std::string::npos) return false;
    try
    {
        w = static_cast<uint32_t>(std::stoul(s.substr(0, xpos)));
        h = static_cast<uint32_t>(std::stoul(s.substr(xpos + 1)));
    }
    catch (...)
    {
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    vsg::CommandLine arguments(&argc, argv);

    auto windowTraits = vsg::WindowTraits::create(arguments);
    windowTraits->windowTitle = "wiv";

    // giv (Cairo/AGG) composites colors "raw": a named color like "green"
    // (0,128,0) is written to the framebuffer as literally 128/255 in each
    // channel, with no gamma/color-management pass. VSG's default swapchain
    // preference is VK_FORMAT_B8G8R8A8_SRGB, which makes the GPU treat our
    // fragment shaders' [0,1] outputs as *linear* light and re-encode them
    // to sRGB on write - e.g. a raw 0.502 (128/255) linear value gets
    // written out as ~0.735 (~188/255), visibly brightening/shifting every
    // non-extreme color relative to giv. Use a non-sRGB (UNORM) swapchain
    // format instead so shader color values map directly to the displayed
    // bytes, matching giv's naive (non-color-managed) output.
    windowTraits->swapchainPreferences.surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

    // giv/AGG rasterizes with software anti-aliasing (smooth edges on
    // lines, mark circles, polygon fills). Enable hardware MSAA to get
    // comparable geometric edge smoothing; combined with analytic
    // (fwidth-based) edge AA in the marks/lines shaders for the SDF-drawn
    // primitives, this gets wiv's edges close to AGG's. Clamp to the
    // device's supported color sample counts.
    windowTraits->samples = VK_SAMPLE_COUNT_4_BIT;

    std::string geomStr = arguments.value(std::string(), "--geometry");
    if (!geomStr.empty())
    {
        uint32_t w, h;
        if (parseGeometry(geomStr, w, h))
        {
            windowTraits->width = w;
            windowTraits->height = h;
        }
        else
        {
            std::cerr << "wiv: invalid --geometry '" << geomStr << "' (expected WxH)\n";
        }
    }

    double zoomScale = 1.0, zoomShiftX = 0.0, zoomShiftY = 0.0;
    bool hasZoom = arguments.read({"--zoom"}, zoomScale, zoomShiftX, zoomShiftY);

    bool noAutoFit = arguments.read("-n");

    if (arguments.errors()) return arguments.writeErrorMessages(std::cerr);

    std::vector<std::string> files;
    for (int i = 1; i < arguments.argc(); ++i) files.emplace_back(arguments[i]);

    if (files.empty())
    {
        std::cerr << "usage: wiv [--geometry WxH] [--zoom scale shift_x shift_y] [-n] file.giv [file2.giv ...]\n";
        return 1;
    }

    // -------------------------------------------------------------
    // Parse
    // -------------------------------------------------------------
    giv::SceneData scene;
    auto parseStart = std::chrono::steady_clock::now();
    size_t totalPoints = 0;
    for (const auto& f : files)
    {
        giv::GivParser parser;
        std::string error;
        if (!parser.parseFile(f, scene, error))
        {
            std::cerr << "wiv: " << error << "\n";
            return 1;
        }
    }
    for (const auto& ds : scene.datasets) totalPoints += ds.pointCount();
    auto parseEnd = std::chrono::steady_clock::now();
    double parseMs = std::chrono::duration<double, std::milli>(parseEnd - parseStart).count();
    std::cerr << "wiv: parsed " << scene.datasets.size() << " dataset(s), " << totalPoints
              << " points in " << parseMs << " ms\n";

    // -------------------------------------------------------------
    // Build scene graph
    // -------------------------------------------------------------
    auto options = vsg::Options::create();
    options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
    options->add(vsgXchange::all::create());
    // Widen vsgXchange's freetype glyph-atlas margins (defaults: 0.25/0.125)
    // to give the SDF distance field more empty border around each glyph
    // quad, reducing the faint box/edge artifacts that mipmapped sampling
    // of a tightly-packed atlas can otherwise bleed in at the quad edges.
    options->setValue(vsgXchange::freetype::texel_margin_ratio, 0.5f);
    options->setValue(vsgXchange::freetype::quad_margin_ratio, 0.25f);

#ifndef WIV_SHADER_DIR
#    define WIV_SHADER_DIR "shaders"
#endif
    std::string shaderDir = WIV_SHADER_DIR;

    auto buildStart = std::chrono::steady_clock::now();
    giv::SceneBuilder builder(options);
    vsg::ref_ptr<vsg::Group> sceneGraph;
    try
    {
        sceneGraph = builder.build(scene, shaderDir);
    }
    catch (const std::exception& e)
    {
        std::cerr << "wiv: failed to build scene: " << e.what() << "\n";
        return 1;
    }
    auto buildEnd = std::chrono::steady_clock::now();
    double buildMs = std::chrono::duration<double, std::milli>(buildEnd - buildStart).count();
    std::cerr << "wiv: scene build took " << buildMs << " ms\n";

    // -------------------------------------------------------------
    // Window / viewer setup
    // -------------------------------------------------------------
    auto viewer = vsg::Viewer::create();
    auto window = vsg::Window::create(windowTraits);
    if (!window)
    {
        std::cerr << "wiv: could not create window (no display / Vulkan device?)\n";
        return 1;
    }
    viewer->addWindow(window);

    // giv clears its canvas to plain white (see gtk-image-viewer.c:
    // gdk_rgba_parse(&background_color,"white")); vsg's window default
    // clear color is a dark blue-gray, so override it to match.
    window->clearColor() = vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f);

    double minX = scene.hasBounds() ? scene.minX : -100.0;
    double maxX = scene.hasBounds() ? scene.maxX : 100.0;
    // SceneBuilder renders with Y negated (giv uses image-style Y-down
    // coordinates; see SceneBuilder.cpp), so the camera must fit the
    // negated/swapped Y range, not the raw parsed bounds.
    double minY = scene.hasBounds() ? -scene.maxY : -100.0;
    double maxY = scene.hasBounds() ? -scene.minY : 100.0;

    double centerX = (minX + maxX) * 0.5;
    double centerY = (minY + maxY) * 0.5;

    // Match giv's fit_marks_in_window/gtk_image_viewer_zoom_to_box exactly:
    // a constant 10-screen-pixel margin on each side (not a percentage of
    // the data range), then preserve aspect ratio by using whichever axis's
    // resulting scale is more constraining (gtk-image-viewer.c:
    // new_scale_{x,y} = (canvas_dim - 2*margin_px) / data_dim, then the
    // smaller of the two is used for both axes).
    double dataW = std::max(maxX - minX, 2e-3);
    double dataH = std::max(maxY - minY, 2e-3);
    double canvasW = static_cast<double>(window->extent2D().width);
    double canvasH = static_cast<double>(window->extent2D().height);
    constexpr double kFitMarginPx = 10.0;
    double scaleX = (canvasW - 2.0 * kFitMarginPx) / dataW;
    double scaleY = (canvasH - 2.0 * kFitMarginPx) / dataH;
    double scale = std::min(scaleX, scaleY);
    if (scale <= 0.0) scale = std::min(canvasW, canvasH) / std::max(dataW, dataH); // pathological fallback (tiny window)
    double halfW = canvasW / (2.0 * scale);
    double halfH = canvasH / (2.0 * scale);

    if (noAutoFit)
    {
        // -n: skip fit-to-bounds, use a fixed default 1:1-ish view centered on the origin.
        centerX = 0.0;
        centerY = 0.0;
        halfW = window->extent2D().width * 0.5;
        halfH = window->extent2D().height * 0.5;
    }

    if (hasZoom)
    {
        halfW /= zoomScale;
        halfH /= zoomScale;
        centerX += zoomShiftX;
        centerY += zoomShiftY;
    }

    auto lookAt = vsg::LookAt::create(vsg::dvec3(centerX, centerY, 1.0), vsg::dvec3(centerX, centerY, 0.0), vsg::dvec3(0.0, 1.0, 0.0));
    auto projection = vsg::Orthographic::create(-halfW, halfW, -halfH, halfH, 0.01, 100.0);
    auto camera = vsg::Camera::create(projection, lookAt, vsg::ViewportState::create(window->extent2D()));

    auto panZoom = giv::PanZoomHandler::create(camera);
    viewer->addEventHandler(panZoom);
    viewer->addEventHandler(vsg::CloseHandler::create(viewer));

    auto commandGraph = vsg::createCommandGraphForView(window, camera, sceneGraph);
    viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});

    viewer->compile();

    // Marks that don't opt into $scale_marks, and all line/outline/quiver
    // widths, stay a constant size in screen pixels; since they're expanded
    // in world space in the vertex shaders (marks.vert / lines.vert), keep
    // their world-space size in sync with the current zoom level here. See
    // SceneBuilder.h (PixelSizeAnimator).
    auto markSizeAnimator = builder.markSizeAnimator();
    auto lineWidthAnimator = builder.lineWidthAnimator();
    auto arrowVertexAnimator = builder.arrowVertexAnimator();
    auto updateMarkSizes = [&]() {
        auto extent = window->extent2D();
        if (extent.width == 0) return;
        float worldPerPixel = static_cast<float>((projection->right - projection->left) / static_cast<double>(extent.width));
        if (markSizeAnimator) markSizeAnimator->update(worldPerPixel);
        if (lineWidthAnimator) lineWidthAnimator->update(worldPerPixel);
        if (arrowVertexAnimator) arrowVertexAnimator->update(worldPerPixel);
    };
    updateMarkSizes();

    std::cerr << "wiv: entering render loop (bounds: [" << minX << "," << minY << "] - [" << maxX << "," << maxY << "])\n";

    // Lightweight FPS reporting to stderr, matches the plan's "report frame
    // time, e.g. via stderr" verification requirement.
    size_t frameCount = 0;
    auto fpsWindowStart = std::chrono::steady_clock::now();
    double recordSubmitMsAccum = 0.0;

    while (viewer->advanceToNextFrame())
    {
        viewer->handleEvents();
        viewer->update();
        updateMarkSizes();

        auto rsStart = std::chrono::steady_clock::now();
        viewer->recordAndSubmit();
        auto rsEnd = std::chrono::steady_clock::now();
        recordSubmitMsAccum += std::chrono::duration<double, std::milli>(rsEnd - rsStart).count();

        viewer->present();

        ++frameCount;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fpsWindowStart).count();
        if (elapsed >= 2.0)
        {
            std::cerr << "wiv: " << (frameCount / elapsed) << " fps ("
                      << (elapsed * 1000.0 / frameCount) << " ms/frame avg over " << frameCount << " frames, "
                      << "recordAndSubmit avg " << (recordSubmitMsAccum / frameCount) << " ms/frame - "
                      << "remainder is event/update/present wait)\n";
            frameCount = 0;
            recordSubmitMsAccum = 0.0;
            fpsWindowStart = now;
        }
    }

    return 0;
}
