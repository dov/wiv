#include "TextMarkup.h"

#include "TextLayout.h" // parseFontSpec

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace giv
{

namespace
{

constexpr double kPtToPx = 96.0 / 72.0;
constexpr double kSizeStep = 1.2; // Pango's PANGO_SCALE_LARGE / 1/PANGO_SCALE_SMALL

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void appendUtf8(std::string& out, unsigned long cp)
{
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decodes the entities in `in` into `out`; false on a malformed one.
bool decodeEntities(const std::string& in, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] != '&')
        {
            out += in[i];
            continue;
        }
        size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) return false;
        const std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#')
        {
            char* end = nullptr;
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const unsigned long cp = std::strtoul(ent.c_str() + (hex ? 2 : 1), &end, hex ? 16 : 10);
            if (end == ent.c_str() + (hex ? 2 : 1) || *end != '\0' || cp == 0 || cp > 0x10FFFF) return false;
            appendUtf8(out, cp);
        }
        else return false;
        i = semi;
    }
    return true;
}

// A number, optionally followed by a unit ("12", "12pt", "3px"). `unit` is lower-cased.
bool parseNumberUnit(const std::string& s, double& value, std::string& unit)
{
    const char* p = s.c_str();
    char* end = nullptr;
    value = std::strtod(p, &end);
    if (end == p) return false;
    unit = lower(end);
    while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.back()))) unit.pop_back();
    return true;
}

// Pango lengths (rise, letter_spacing): 1024ths of a point, or with pt/px units.
bool parsePangoLengthPx(const std::string& s, double& px)
{
    double v;
    std::string unit;
    if (!parseNumberUnit(s, v, unit)) return false;
    if (unit.empty()) px = v / 1024.0 * kPtToPx;
    else if (unit == "pt") px = v * kPtToPx;
    else if (unit == "px") px = v;
    else return false;
    return true;
}

bool parseWeight(const std::string& v, int& w)
{
    static const std::map<std::string, int> names = {
        {"ultralight", 200}, {"light", 300}, {"normal", 400}, {"semibold", 600}, {"bold", 700}, {"ultrabold", 800}, {"heavy", 900}};
    auto it = names.find(lower(v));
    if (it != names.end())
    {
        w = it->second;
        return true;
    }
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || n < 1 || n > 1000) return false;
    w = static_cast<int>(n);
    return true;
}

bool parseColorAttr(const std::string& v, Color& c)
{
    // Pango also allows 16-bit hex channels (#rrrrggggbbbb): keep the high bytes.
    if (v.size() == 13 && v[0] == '#')
    {
        const std::string hex = "#" + v.substr(1, 2) + v.substr(5, 2) + v.substr(9, 2);
        return parseGivColor(hex, c);
    }
    return parseGivColor(v, c);
}

struct Attr
{
    std::string name, value;
};

// Parses `name="value" ...` from a tag body; false on malformed input.
bool parseAttributes(const std::string& body, std::vector<Attr>& out)
{
    size_t i = 0;
    auto skipWs = [&] { while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i; };
    while (true)
    {
        skipWs();
        if (i >= body.size()) return true;
        size_t ns = i;
        while (i < body.size() && !std::isspace(static_cast<unsigned char>(body[i])) && body[i] != '=') ++i;
        std::string name = body.substr(ns, i - ns);
        skipWs();
        if (name.empty() || i >= body.size() || body[i] != '=') return false;
        ++i;
        skipWs();
        if (i >= body.size() || (body[i] != '"' && body[i] != '\'')) return false;
        const char q = body[i++];
        size_t close = body.find(q, i);
        if (close == std::string::npos) return false;
        std::string raw = body.substr(i, close - i), val;
        if (!decodeEntities(raw, val)) return false;
        out.push_back({lower(name), val});
        i = close + 1;
    }
}

bool applySpanAttr(MarkupStyle& st, const Attr& a)
{
    const std::string& n = a.name;
    const std::string& v = a.value;
    if (n == "font" || n == "font_desc")
    {
        const FontSpec fs = parseFontSpec(v);
        if (fs.hasFamily) st.family = fs.family;
        if (fs.weight != 400) st.weight = fs.weight;
        if (fs.italic) st.italic = true;
        if (fs.sizePt > 0)
        {
            st.sizePt = fs.sizePt;
            st.sizeScale = 1.0;
        }
        return true;
    }
    if (n == "family" || n == "face" || n == "font_family")
    {
        st.family = v;
        return true;
    }
    if (n == "size" || n == "font_size")
    {
        static const std::map<std::string, double> keywords = {
            {"xx-small", 1.0 / (kSizeStep * kSizeStep * kSizeStep)}, {"x-small", 1.0 / (kSizeStep * kSizeStep)},
            {"small", 1.0 / kSizeStep}, {"medium", 1.0}, {"large", kSizeStep},
            {"x-large", kSizeStep * kSizeStep}, {"xx-large", kSizeStep * kSizeStep * kSizeStep}};
        const std::string l = lower(v);
        auto it = keywords.find(l);
        if (it != keywords.end())
        {
            st.sizePt.reset();
            st.sizeScale = it->second;
            return true;
        }
        if (l == "smaller") st.sizeScale /= kSizeStep;
        else if (l == "larger") st.sizeScale *= kSizeStep;
        else
        {
            double num;
            std::string unit;
            if (!parseNumberUnit(v, num, unit) || num <= 0) return false;
            if (unit.empty()) st.sizePt = num / 1024.0; // Pango units
            else if (unit == "pt") st.sizePt = num;
            else if (unit == "px") st.sizePt = num / kPtToPx;
            else return false;
            st.sizeScale = 1.0;
        }
        return true;
    }
    if (n == "style" || n == "font_style")
    {
        const std::string l = lower(v);
        if (l == "italic" || l == "oblique") st.italic = true;
        else if (l == "normal") st.italic = false;
        else return false;
        return true;
    }
    if (n == "weight" || n == "font_weight")
    {
        int w;
        if (!parseWeight(v, w)) return false;
        st.weight = w;
        return true;
    }
    if (n == "foreground" || n == "fgcolor" || n == "color")
    {
        Color c;
        if (!parseColorAttr(v, c)) return false;
        st.fg = c;
        return true;
    }
    if (n == "background" || n == "bgcolor")
    {
        Color c;
        if (!parseColorAttr(v, c)) return false;
        st.bg = c;
        return true;
    }
    if (n == "alpha" || n == "fgalpha")
    {
        double num;
        std::string unit;
        if (!parseNumberUnit(v, num, unit)) return false;
        const double alpha = unit == "%" ? num / 100.0 : num / 65535.0;
        st.fgAlpha = std::clamp(alpha, 0.0, 1.0);
        return true;
    }
    if (n == "underline")
    {
        const std::string l = lower(v);
        if (l == "none") st.underline = MarkupStyle::Underline::None;
        else if (l == "single" || l == "error") st.underline = MarkupStyle::Underline::Single;
        else if (l == "double") st.underline = MarkupStyle::Underline::Double;
        else if (l == "low") st.underline = MarkupStyle::Underline::Low;
        else return false;
        return true;
    }
    if (n == "underline_color")
    {
        Color c;
        if (!parseColorAttr(v, c)) return false;
        st.underlineColor = c;
        return true;
    }
    if (n == "strikethrough")
    {
        const std::string l = lower(v);
        if (l == "true") st.strike = true;
        else if (l == "false") st.strike = false;
        else return false;
        return true;
    }
    if (n == "strikethrough_color")
    {
        Color c;
        if (!parseColorAttr(v, c)) return false;
        st.strikeColor = c;
        return true;
    }
    if (n == "rise") return parsePangoLengthPx(v, st.risePx);
    if (n == "letter_spacing") return parsePangoLengthPx(v, st.letterSpacingPx);
    // Attributes that only matter to Pango's own shaping/layout (variant, stretch,
    // lang, gravity, ...) are accepted and ignored.
    static const char* ignored[] = {"variant", "font_variant", "stretch", "font_stretch", "lang", "gravity",
                                    "gravity_hint", "font_features", "overline", "overline_color", "fallback",
                                    "allow_breaks", "insert_hyphens", "show", "text_transform", "line_height"};
    for (const char* ig : ignored)
        if (n == ig) return true;
    return false;
}

} // namespace

bool parsePangoMarkup(const std::string& markup, std::vector<MarkupSpan>& out, std::string* error)
{
    out.clear();
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        out.clear();
        return false;
    };

    struct Open
    {
        std::string tag;
        MarkupStyle saved;
    };
    std::vector<Open> stack;
    MarkupStyle cur;
    std::string pending; // decoded text since the last tag

    auto flush = [&] {
        if (!pending.empty()) out.push_back({pending, cur});
        pending.clear();
    };

    size_t i = 0;
    while (i < markup.size())
    {
        const char c = markup[i];
        if (c != '<')
        {
            size_t next = markup.find('<', i);
            if (next == std::string::npos) next = markup.size();
            std::string decoded;
            if (!decodeEntities(markup.substr(i, next - i), decoded)) return fail("bad entity");
            pending += decoded;
            i = next;
            continue;
        }
        // A tag: up to the first '>' outside quotes.
        size_t j = i + 1;
        char quote = 0;
        for (; j < markup.size(); ++j)
        {
            const char ch = markup[j];
            if (quote)
            {
                if (ch == quote) quote = 0;
            }
            else if (ch == '"' || ch == '\'') quote = ch;
            else if (ch == '>') break;
        }
        if (j >= markup.size()) return fail("unterminated tag");
        std::string body = markup.substr(i + 1, j - i - 1);
        i = j + 1;

        if (body.compare(0, 3, "!--") == 0) continue; // (a comment containing '>' ends early; Pango forbids those anyway)
        const bool closing = !body.empty() && body[0] == '/';
        const bool selfClosing = !body.empty() && body.back() == '/';
        if (closing) body.erase(0, 1);
        if (selfClosing) body.pop_back();
        size_t nameEnd = 0;
        while (nameEnd < body.size() && !std::isspace(static_cast<unsigned char>(body[nameEnd]))) ++nameEnd;
        const std::string name = lower(body.substr(0, nameEnd));
        if (name.empty()) return fail("empty tag");

        if (closing)
        {
            if (stack.empty() || stack.back().tag != name) return fail("mismatched </" + name + ">");
            flush();
            cur = stack.back().saved;
            stack.pop_back();
            continue;
        }

        flush();
        const MarkupStyle saved = cur;
        std::vector<Attr> attrs;
        if (!parseAttributes(body.substr(nameEnd), attrs)) return fail("bad attributes in <" + name + ">");

        if (name == "b") cur.weight = 700;
        else if (name == "i") cur.italic = true;
        else if (name == "u") cur.underline = MarkupStyle::Underline::Single;
        else if (name == "s") cur.strike = true;
        else if (name == "big") cur.sizeScale *= kSizeStep;
        else if (name == "small") cur.sizeScale /= kSizeStep;
        else if (name == "sub" || name == "sup")
        {
            const double rise = 5000.0 / 1024.0 * kPtToPx; // Pango's own offset: 5000 units
            cur.risePx += name == "sup" ? rise : -rise;
            cur.sizeScale /= kSizeStep;
        }
        else if (name == "tt") cur.family = "Monospace";
        else if (name == "span")
        {
            for (const Attr& a : attrs)
                if (!applySpanAttr(cur, a)) return fail("bad attribute " + a.name + " in <span>");
        }
        else if (name == "a" || name == "markup")
        {
        }
        else return fail("unknown tag <" + name + ">");

        if (selfClosing) cur = saved;
        else stack.push_back({name, saved});
    }
    if (!stack.empty()) return fail("unclosed <" + stack.back().tag + ">");
    flush();
    return true;
}

} // namespace giv
