#pragma once
// Text layout for vector output: fontconfig font lookup (with per-character
// fallback), FriBidi bidirectional reordering, HarfBuzz shaping, and glyph
// outlines as path points. Qt-free. Thread-safe (calls are serialized).
//
// Used for SVG <text>, which is drawn as filled vector outlines rather than
// through the glyph atlas the .giv text marks use.

#include "GivScene.h" // Op

#include <string>
#include <vector>

namespace giv
{

struct TextRequest
{
    std::string text;               // UTF-8, already whitespace-collapsed
    std::string family = "Sans";    // CSS-like family name (fontconfig resolves it)
    int weight = 400;               // CSS weight 100..900
    bool italic = false;
    double sizePx = 16.0;           // em size in the output units
    double letterSpacing = 0.0;     // added after every glyph (output units)
    bool rtlBase = false;           // paragraph direction (SVG direction="rtl")
    std::string lang;               // BCP-47 tag (xml:lang), may be empty
};

struct TextOutline
{
    // Glyph contours as path points, in output units. The origin is the start
    // of the baseline of the (visually) left end of the line; y grows
    // downwards. Ops used: Move, Draw, Curve (+2 Cont: cubic), ClosePath
    // (whose x/y are 0 and must not be transformed).
    std::vector<float> x, y;
    std::vector<Op> op;
    // Index into the arrays after each glyph's points (for chunking).
    std::vector<size_t> glyphEnd;
    double advance = 0.0; // total line width
    double ascent = 0.0;  // of the primary font, > 0
    double descent = 0.0; // of the primary font, > 0
    bool empty() const { return op.empty(); }
};

// Lays out one line of text. Returns false if no font could be found at all.
bool layoutText(const TextRequest& req, TextOutline& out);

} // namespace giv
