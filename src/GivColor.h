#pragma once
//
// GivColor.h - X11 color name lookup + giv's "/alpha" suffix extension.
//
// giv color syntax (see giv-parser.cc color_parse()):
//   <name>                  -> opaque X11-named color
//   #rrggbb / #rgb          -> hex color
//   none                    -> fully transparent / "do not draw"
//   <name>/0xhh             -> named color, alpha given as a single hex byte
//   <name>/0xhhhh           -> named color, alpha given as a 16-bit hex value
//                              (top byte replicated, matching giv's behavior)
//   <name>/<float with '.'> -> named color, alpha in [0,1]
//   <name>/<int>            -> named color, alpha = int * 255 (giv's integer
//                              alpha convention - matches giv-parser.cc)
//
#include <cstdint>
#include <string>

namespace giv
{

struct Color
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
    bool isNone = false; // "none" - explicit request to not draw

    static Color opaque(float r_, float g_, float b_, float a_ = 1.0f)
    {
        Color c;
        c.r = r_;
        c.g = g_;
        c.b = b_;
        c.a = a_;
        return c;
    }
};

// Parse a giv color spec (name, #hex, or name/alpha form). Returns false
// (and leaves out unchanged) if the name could not be resolved at all.
// "none" is recognized specially and sets out.isNone = true.
bool parseGivColor(const std::string& spec, Color& out);

} // namespace giv
