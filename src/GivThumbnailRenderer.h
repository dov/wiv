#pragma once
//
// GivThumbnailRenderer.h - lightweight, GPU-free rasterizer that draws a
// parsed .giv/.svg SceneData (see GivScene.h) into a small QImage, for use
// as a file-browser thumbnail (see src/qt/OpenFileDialog.cpp). Deliberately
// independent of the VSG/Vulkan SceneBuilder pipeline (SceneBuilder.h/.cpp):
// approximates the same mark grammar with plain QPainter calls instead of
// building GPU geometry, so it can run on a background QtConcurrent thread
// without a live Vulkan device/window. Not pixel-identical to the real
// viewer - dash patterns are drawn solid and pixel-constant sizes are
// clamped to stay legible at thumbnail scale - just recognizable.
//
#include <QImage>

#include <string>

namespace giv
{

struct SceneData;

// Rasterizes `scene` fit into a `maxSize` x `maxSize` transparent canvas
// (aspect-preserved, centered, with a small margin). Returns a null QImage
// if the scene has no bounds (nothing drawable - e.g. a file that only
// references a whole-view $image, which this renderer does not decode).
QImage renderGivThumbnail(const SceneData& scene, int maxSize);

// Convenience: parses `filename` (.giv or .svg, via GivParser::parseFile)
// and renders its thumbnail directly. Returns a null QImage on parse
// failure or an empty scene.
QImage renderGivFileThumbnail(const std::string& filename, int maxSize);

} // namespace giv
