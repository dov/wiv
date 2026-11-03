#pragma once
// Text layout for vector output: fontconfig font lookup (with per-character
// fallback), FriBidi bidirectional reordering, HarfBuzz shaping, and glyph
// outlines as path points. Qt-free. Thread-safe (calls are serialized).
//
// Used for SVG <text>, which is drawn as filled vector outlines rather than
// through the glyph atlas the .giv text marks use.

#include "GivScene.h" // Op
#include "TextMarkup.h"

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
    // Per glyph: where its origin is (the glyph's points minus this give its
    // shape on its own) and an id of the shape (font face, weight, glyph,
    // synthetic slant) - equal ids have equal shapes at equal sizes, so
    // callers can cache per-glyph work.
    std::vector<float> glyphOx, glyphOy;
    std::vector<unsigned long long> glyphKey;
    double advance = 0.0; // total line width
    double ascent = 0.0;  // of the primary font, > 0
    double descent = 0.0; // of the primary font, > 0
    // Decoration metrics of the primary font, in output units: where underline /
    // strikethrough strokes go (distance below / above the baseline) and how thick.
    double underlinePos = 0.0, underlineThick = 0.0;
    double strikePos = 0.0, strikeThick = 0.0;
    bool empty() const { return op.empty(); }
};

// A Pango-style font description ("Sans Bold Italic 14", "Serif 10") as giv's
// $font uses: family, weight, slant and the size in points (or -1 if absent).
struct FontSpec
{
    std::string family = "Sans";
    int weight = 400;
    bool italic = false;
    double sizePt = -1.0;
    bool hasFamily = false; // the description named a family (else `family` is the default)
};
FontSpec parseFontSpec(const std::string& spec);

// A label laid out as lines of styled pieces (Pango markup gives several pieces
// per line; plain text one). giv semantics: the logical box is the widest line
// by the sum of the line heights, each line (ascent + descent of its tallest
// piece, rise included) stacked from the top, left aligned.
struct RichPiece
{
    TextOutline outline; // laid out at `emPx`; origin = the piece's start on its baseline
    double x = 0.0;      // from the line's left edge
    double emPx = 0.0;
    MarkupStyle style;
};
struct RichLine
{
    std::vector<RichPiece> pieces;
    double width = 0.0, ascent = 0.0, descent = 0.0;
    double top = 0.0; // from the box top
};
struct RichLabel
{
    std::vector<RichLine> lines;
    double width = 0.0, height = 0.0;
};

// Lays out `text` ('\n' separates lines). With `markup` it is parsed as Pango
// markup first (and shown literally if it is malformed).
bool layoutRichLabel(const std::string& text, const FontSpec& base, double baseEmPx, bool markup, RichLabel& out);

// Size of a label's logical box, as giv computes it (Pango). False if no font was found.
bool measureLabel(const std::string& text, const FontSpec& font, double emPx, bool markup, double& width, double& height);

// Lays out one line of text. Returns false if no font could be found at all.
bool layoutText(const TextRequest& req, TextOutline& out);

} // namespace giv
