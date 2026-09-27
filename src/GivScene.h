#pragma once
//
// GivScene.h - the Dataset struct produced by GivParser and consumed by
// SceneBuilder. Points are stored as structure-of-arrays so SceneBuilder can
// upload straight into GPU buffers without a transform pass.
//
#include "GivColor.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace giv
{

enum class Op : uint8_t
{
    Move,      // moveto (also implicit at first point of a dataset)
    Draw,      // lineto
    Curve,     // first control point of a cubic bezier segment (x,y = c1)
    Cont,      // continuation point (bezier c2/end, or ellipse w/h & angle)
    Ellipse,   // start of an ellipse (x,y = center); followed by 2 Cont points
    Quiver,    // quiver vector (x,y = dx,dy) anchored at the previous point
    ClosePath, // close current subpath back to its start
    Text       // text mark; text content held out-of-line in Dataset::texts
};

enum class MarkType : uint8_t
{
    FCircle = 1,
    FSquare = 2,
    Circle = 3,
    Square = 4,
    Pixel = 5
};

enum class ArrowType : uint8_t
{
    None = 0,
    Start = 1,
    End = 2,
    Both = 3
};

enum class TextStyle : uint8_t
{
    Normal,
    DropShadow
};

struct TextItem
{
    size_t pointIndex = 0; // index into Dataset::x/y/op for placement (op==Text)
    int align = 1;         // numeric-keypad alignment, 1-9 (default 1 = bottom-left)
    std::string text;
};

// Per-dataset style state, matches the subset of giv_dataset_t relevant to
// Phase 1 rendering.
struct Dataset
{
    std::string pathName;
    std::string balloon;

    Color color = Color::opaque(1, 0, 0, 1);
    Color outlineColor = Color::opaque(0, 0, 0, 1);
    Color quiverColor = Color::opaque(0, 0, 0, 1);

    double lineWidth = 1.0;
    std::vector<float> dashes; // on,off,on,off,... in world units; empty = solid
    int lineCap = 1;           // 0=butt,1=round,2=square (matches giv's line_cap)

    MarkType markType = MarkType::FCircle;
    double markSize = 7.0; // giv's default_mark_size (giv-data.cc); full diameter in pixels when !doScaleMarks
    bool doScaleMarks = false;

    double textSize = 12.0;
    double textAngle = 0.0;
    bool doScaleFonts = false;
    std::string fontName;
    TextStyle textStyle = TextStyle::Normal;

    double quiverScale = 1.0;
    bool quiverHead = true;

    ArrowType arrowType = ArrowType::None;

    bool doDrawMarks = false;
    bool doDrawLines = true;
    bool doDrawPolygon = false;
    bool doDrawPolygonOutline = false;
    bool isVisible = true;

    // Structure-of-arrays point storage. op[i] describes how (x[i], y[i])
    // should be interpreted; see Op for the grammar (mirrors giv's point_t
    // stream, but flattened - no per-point text pointer).
    std::vector<float> x;
    std::vector<float> y;
    std::vector<Op> op;

    // Text marks, referenced by pointIndex into the arrays above (op==Text
    // at that index; x[i],y[i] give the anchor position).
    std::vector<TextItem> texts;

    // Pre-rasterized RGBA sprite drawn as a positioned textured quad
    // instead of the vector geometry above - used for SVG shapes with an
    // feGaussianBlur filter (see SvgLoader), which this renderer's flat-
    // colored triangle fills/lines can't express. Rasterized+blurred once
    // at load time; x/y/w/h are world-space (same Y-down convention as
    // the point arrays above). When isSprite is set, the x/y/op arrays
    // above are empty and doDrawMarks/doDrawLines/doDrawPolygon are moot.
    bool isSprite = false;
    std::vector<unsigned char> spriteRGBA; // spriteWidth*spriteHeight*4, top-to-bottom
    int spriteWidth = 0;
    int spriteHeight = 0;
    double spriteX = 0.0, spriteY = 0.0;
    double spriteW = 0.0, spriteH = 0.0;

    void reserve(size_t n)
    {
        x.reserve(n);
        y.reserve(n);
        op.reserve(n);
    }

    void addPoint(float px, float py, Op o)
    {
        x.push_back(px);
        y.push_back(py);
        op.push_back(o);
    }

    size_t pointCount() const { return x.size(); }
};

struct SceneData
{
    std::vector<Dataset> datasets;

    // $image reference filenames, in file order, as written in the .giv
    // file (not yet resolved to an absolute path - see
    // VulkanViewport::loadFiles, which resolves each one relative to the
    // .giv file's directory if it doesn't exist as given, mirroring giv's
    // own cb_image_reference). Whole-file/whole-view concept in giv (one
    // currently-displayed image, cycled by the user), not geometry
    // attached to a particular dataset.
    std::vector<std::string> images;

    // $pixelsize <value> [<unit>] - giv's calibration directive (see
    // GivParser::parseLine's "$pixelsize" handling and
    // VulkanViewport::loadFiles, which feeds this to
    // VulkanViewport::setPixelCalibration on load). Whole-file/whole-view
    // concept, like `images` above - not attached to a particular dataset.
    // std::nullopt if the file never sets it, so a load can tell "not
    // specified" apart from an explicit "$pixelsize 1" and leave whatever
    // calibration the user already dialed in alone.
    std::optional<double> pixelSize;
    std::string pixelSizeUnit;

    double minX = 1e30, minY = 1e30;
    double maxX = -1e30, maxY = -1e30;

    void updateBounds(double px, double py, double margin = 0.0)
    {
        minX = std::min(minX, px - margin);
        maxX = std::max(maxX, px + margin);
        minY = std::min(minY, py - margin);
        maxY = std::max(maxY, py + margin);
    }

    // Like updateBounds, but with independent (possibly asymmetric) offsets
    // on each side - used for text labels, whose extent relative to the
    // anchor point depends on the text alignment (see GivParser's 't'/'T'
    // handling).
    void updateBoundsRect(double px, double py, double x0, double x1, double y0, double y1)
    {
        minX = std::min(minX, px + x0);
        maxX = std::max(maxX, px + x1);
        minY = std::min(minY, py + y0);
        maxY = std::max(maxY, py + y1);
    }

    bool hasBounds() const { return minX <= maxX && minY <= maxY; }
};

} // namespace giv
