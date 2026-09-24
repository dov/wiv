#pragma once
//
// GivScene.h - the Dataset struct produced by GivParser and consumed by
// SceneBuilder. Points are stored as structure-of-arrays so SceneBuilder can
// upload straight into GPU buffers without a transform pass.
//
#include "GivColor.h"

#include <algorithm>
#include <cstdint>
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
    double markSize = 4.0;
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

    double minX = 1e30, minY = 1e30;
    double maxX = -1e30, maxY = -1e30;

    void updateBounds(double px, double py, double margin = 0.0)
    {
        minX = std::min(minX, px - margin);
        maxX = std::max(maxX, px + margin);
        minY = std::min(minY, py - margin);
        maxY = std::max(maxY, py + margin);
    }

    bool hasBounds() const { return minX <= maxX && minY <= maxY; }
};

} // namespace giv
