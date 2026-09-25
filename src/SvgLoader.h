#pragma once
//
// SvgLoader.h - loads an .svg file's shapes as vector Datasets (cubic
// bezier control points, not a rasterized bitmap), so SVG art scales and
// tessellates the same way giv's own $curve/$ellipse marks do.
//
#include "GivScene.h"

#include <string>

namespace giv
{

bool loadSvgFile(const std::string& filename, SceneData& scene, std::string& error);

} // namespace giv
