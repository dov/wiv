#pragma once
// A small Pango-markup parser (the subset giv labels use). Qt- and
// Pango-free. Turns "<b>bold</b> and <span foreground=\"red\" size=\"x-large\">big</span>"
// into a flat list of styled text spans.
//
// Supported tags: <b> <i> <u> <s> <big> <small> <sub> <sup> <tt> <span>
// (and <a>/<markup>, which just group). <span> attributes: font / font_desc,
// family / face / font_family, size / font_size (1024ths of a point, a size
// keyword, or a length with pt/px), style / font_style, weight / font_weight,
// foreground / fgcolor / color, background / bgcolor, alpha / fgalpha,
// underline, underline_color, strikethrough, strikethrough_color, rise,
// letter_spacing. Entities: &amp; &lt; &gt; &quot; &apos; &#N; &#xH;.

#include "GivColor.h"

#include <optional>
#include <string>
#include <vector>

namespace giv
{

struct MarkupStyle
{
    std::optional<std::string> family;
    std::optional<int> weight;     // CSS-like 100..900
    std::optional<bool> italic;
    std::optional<double> sizePt;  // absolute size in points (font=, size="12pt")
    double sizeScale = 1.0;        // relative to the base/absolute size (<big>, size="large", ...)
    std::optional<Color> fg;
    std::optional<Color> bg;
    std::optional<double> fgAlpha; // 0..1, applied to the foreground colour (span's, else the label's)
    enum class Underline { None, Single, Double, Low } underline = Underline::None;
    std::optional<Color> underlineColor;
    bool strike = false;
    std::optional<Color> strikeColor;
    double risePx = 0.0;           // positive = up
    double letterSpacingPx = 0.0;
};

struct MarkupSpan
{
    std::string text; // entities decoded; may contain '\n'
    MarkupStyle style;
};

// Parses `markup`. Returns false (with `error` set) if it is not well formed
// - unbalanced tags, an unknown tag, a bad entity - in which case the caller
// should show the text literally, as Pango does not render a broken string.
bool parsePangoMarkup(const std::string& markup, std::vector<MarkupSpan>& out, std::string* error = nullptr);

} // namespace giv
