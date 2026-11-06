// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "SvgCss.h"

#include "SvgPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>

namespace giv
{

namespace
{

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string fmt(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// Splits at `sep` (or at any white space if sep is ' ') outside parentheses and quotes.
std::vector<std::string> splitTop(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    auto flush = [&] {
        std::string t = trim(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
    };
    for (char c : s)
    {
        if (quote)
        {
            if (c == quote) quote = 0;
            cur += c;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            cur += c;
        }
        else if (c == '(') ++depth, cur += c;
        else if (c == ')') --depth, cur += c;
        else if (depth == 0 && (sep == ' ' ? std::isspace(static_cast<unsigned char>(c)) != 0 : c == sep)) flush();
        else cur += c;
    }
    flush();
    return out;
}

struct Decl
{
    std::string prop, value;
};

std::vector<Decl> parseDecls(const std::string& text)
{
    std::vector<Decl> out;
    for (const std::string& d : splitTop(text, ';'))
    {
        size_t colon = d.find(':');
        if (colon == std::string::npos) continue;
        Decl decl{lower(trim(d.substr(0, colon))), trim(d.substr(colon + 1))};
        size_t imp = decl.value.find("!important");
        if (imp != std::string::npos) decl.value = trim(decl.value.substr(0, imp));
        if (!decl.prop.empty() && !decl.value.empty()) out.push_back(std::move(decl));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Selectors and the stylesheet
// ---------------------------------------------------------------------------
struct Compound
{
    std::string tag, id;
    std::vector<std::string> classes;
};

struct Selector
{
    std::vector<Compound> parts;
    std::vector<char> combs; // between parts[i] and parts[i + 1]: ' ' or '>'
    int specificity = 0;
};

bool parseSelector(const std::string& text, Selector& out)
{
    out = Selector();
    Compound cur;
    bool have = false;
    char pendingComb = ' ';
    auto flush = [&]() {
        if (!have) return;
        out.specificity += (cur.id.empty() ? 0 : 100) + 10 * static_cast<int>(cur.classes.size()) + (cur.tag.empty() ? 0 : 1);
        if (!out.parts.empty()) out.combs.push_back(pendingComb);
        out.parts.push_back(std::move(cur));
        cur = Compound();
        have = false;
        pendingComb = ' ';
    };
    size_t i = 0;
    while (i < text.size())
    {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) { flush(); ++i; continue; }
        if (c == '>') { flush(); pendingComb = '>'; ++i; continue; }
        if (c == ':' || c == '[' || c == '+' || c == '~') return false; // unsupported
        size_t j = i + 1;
        auto isIdent = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_'; };
        if (c == '*') { have = true; i = j; continue; }
        while (j < text.size() && isIdent(text[j])) ++j;
        std::string word = text.substr(c == '.' || c == '#' ? i + 1 : i, j - (c == '.' || c == '#' ? i + 1 : i));
        if (word.empty()) return false;
        have = true;
        if (c == '.') cur.classes.push_back(word);
        else if (c == '#') cur.id = word;
        else cur.tag = word;
        i = j;
    }
    flush();
    return !out.parts.empty();
}

bool matchCompound(const Compound& c, const SvgNode& n)
{
    if (!c.tag.empty() && c.tag != n.name) return false;
    if (!c.id.empty() && c.id != n.attr("id")) return false;
    if (!c.classes.empty())
    {
        const std::vector<std::string> have = splitTop(n.attr("class"), ' ');
        for (const std::string& want : c.classes)
            if (std::find(have.begin(), have.end(), want) == have.end()) return false;
    }
    return true;
}

bool matchAt(const Selector& s, int i, const std::vector<const SvgNode*>& path, int p)
{
    if (!matchCompound(s.parts[static_cast<size_t>(i)], *path[static_cast<size_t>(p)])) return false;
    if (i == 0) return true;
    if (s.combs[static_cast<size_t>(i - 1)] == '>') return p > 0 && matchAt(s, i - 1, path, p - 1);
    for (int q = p - 1; q >= 0; --q)
        if (matchAt(s, i - 1, path, q)) return true;
    return false;
}

struct Rule
{
    Selector sel;
    std::vector<Decl> decls;
    int order = 0;
};

struct Frame
{
    double offset = 0;
    std::vector<Decl> decls;
};

struct Sheet
{
    std::vector<Rule> rules;
    std::map<std::string, std::vector<Frame>> keyframes;
};

// Index of the '}' matching the '{' at `open` (or the end of the text).
size_t matchingBrace(const std::string& t, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i)
    {
        if (t[i] == '{') ++depth;
        else if (t[i] == '}' && --depth == 0) return i;
    }
    return t.size();
}

std::string stripComments(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in.compare(i, 2, "/*") == 0)
        {
            size_t e = in.find("*/", i + 2);
            if (e == std::string::npos) break;
            i = e + 1;
        }
        else out += in[i];
    }
    return out;
}

void parseSheet(const std::string& text, Sheet& sheet)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        if (pos >= text.size()) break;
        if (text[pos] == '@')
        {
            size_t brace = text.find('{', pos), semi = text.find(';', pos);
            if (semi != std::string::npos && (brace == std::string::npos || semi < brace))
            {
                pos = semi + 1;
                continue;
            }
            if (brace == std::string::npos) break;
            const std::string head = trim(text.substr(pos + 1, brace - pos - 1));
            const size_t close = matchingBrace(text, brace);
            const std::string body = text.substr(brace + 1, close - brace - 1);
            const size_t sp = head.find_first_of(" \t\n");
            const std::string kind = lower(head.substr(0, sp));
            if (kind == "keyframes" || kind == "-webkit-keyframes")
            {
                const std::string name = sp == std::string::npos ? "" : trim(head.substr(sp));
                std::vector<Frame> frames;
                size_t p = 0;
                while (p < body.size())
                {
                    size_t b = body.find('{', p);
                    if (b == std::string::npos) break;
                    const size_t e = matchingBrace(body, b);
                    const std::vector<Decl> decls = parseDecls(body.substr(b + 1, e - b - 1));
                    for (const std::string& sel : splitTop(body.substr(p, b - p), ','))
                    {
                        const std::string s = lower(sel);
                        Frame f;
                        if (s == "from") f.offset = 0;
                        else if (s == "to") f.offset = 1;
                        else if (!s.empty() && s.back() == '%') f.offset = std::strtod(s.c_str(), nullptr) / 100.0;
                        else continue;
                        f.decls = decls;
                        frames.push_back(std::move(f));
                    }
                    p = e + 1;
                }
                std::stable_sort(frames.begin(), frames.end(), [](const Frame& a, const Frame& b) { return a.offset < b.offset; });
                if (!name.empty()) sheet.keyframes[name] = std::move(frames);
            }
            else if (kind == "media" || kind == "supports")
            {
                // Applied unconditionally; a viewer has no media to query.
                if (kind == "supports" || head.find("print") == std::string::npos) parseSheet(body, sheet);
            }
            pos = close + 1;
            continue;
        }
        const size_t brace = text.find('{', pos);
        if (brace == std::string::npos) break;
        const size_t close = matchingBrace(text, brace);
        const std::vector<Decl> decls = parseDecls(text.substr(brace + 1, close - brace - 1));
        for (const std::string& sel : splitTop(text.substr(pos, brace - pos), ','))
        {
            Rule r;
            if (!parseSelector(sel, r.sel)) continue;
            r.decls = decls;
            r.order = static_cast<int>(sheet.rules.size());
            sheet.rules.push_back(std::move(r));
        }
        pos = close + 1;
    }
}

void collectStyleText(const SvgNode& n, std::string& out)
{
    if (n.name == "style")
    {
        const std::string& type = n.attr("type");
        if (type.empty() || type == "text/css")
            for (const auto& c : n.children)
                if (c->name == "#text") out += c->text + "\n";
        return;
    }
    for (const auto& c : n.children) collectStyleText(*c, out);
}

// ---------------------------------------------------------------------------
// CSS transform -> SVG transform
// ---------------------------------------------------------------------------
struct TransformFn
{
    std::string name; // SVG function name
    std::vector<double> args;
};

bool parseCssNumber(const std::string& in, bool angle, double& out)
{
    const std::string s = trim(in);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    const std::string unit = lower(trim(end));
    if (unit.empty()) out = v;
    else if (angle)
    {
        if (unit == "deg") out = v;
        else if (unit == "rad") out = v * 180.0 / M_PI;
        else if (unit == "grad") out = v * 0.9;
        else if (unit == "turn") out = v * 360.0;
        else return false;
    }
    else if (unit == "px") out = v;
    else return false;
    return true;
}

bool parseCssTransform(const std::string& css, std::vector<TransformFn>& out)
{
    out.clear();
    const std::string s = trim(css);
    if (s == "none") return true;
    size_t pos = 0;
    while (pos < s.size())
    {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size()) break;
        const size_t open = s.find('(', pos), close = s.find(')', pos);
        if (open == std::string::npos || close == std::string::npos || close < open) return false;
        const std::string name = trim(s.substr(pos, open - pos));
        std::string argText = s.substr(open + 1, close - open - 1);
        std::replace(argText.begin(), argText.end(), ',', ' ');
        std::vector<std::string> raw = splitTop(argText, ' ');
        const bool angle = name == "rotate" || name == "rotateZ" || name.compare(0, 4, "skew") == 0;
        std::vector<double> a;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            double v;
            // matrix() arguments are plain numbers; the rest are lengths or angles.
            if (!parseCssNumber(raw[i], angle, v)) return false;
            a.push_back(v);
        }
        TransformFn f;
        if (name == "translate" && !a.empty()) f = {"translate", {a[0], a.size() > 1 ? a[1] : 0.0}};
        else if (name == "translateX" && a.size() == 1) f = {"translate", {a[0], 0.0}};
        else if (name == "translateY" && a.size() == 1) f = {"translate", {0.0, a[0]}};
        else if (name == "scale" && !a.empty()) f = {"scale", {a[0], a.size() > 1 ? a[1] : a[0]}};
        else if (name == "scaleX" && a.size() == 1) f = {"scale", {a[0], 1.0}};
        else if (name == "scaleY" && a.size() == 1) f = {"scale", {1.0, a[0]}};
        else if ((name == "rotate" || name == "rotateZ") && a.size() == 1) f = {"rotate", {a[0]}};
        else if (name == "skewX" && a.size() == 1) f = {"skewX", {a[0]}};
        else if (name == "skewY" && a.size() == 1) f = {"skewY", {a[0]}};
        else if (name == "skew" && !a.empty())
        {
            out.push_back({"skewX", {a[0]}});
            if (a.size() > 1) out.push_back({"skewY", {a[1]}});
            pos = close + 1;
            continue;
        }
        else if (name == "matrix" && a.size() == 6) f = {"matrix", a};
        else return false;
        out.push_back(std::move(f));
        pos = close + 1;
    }
    return true;
}

std::vector<double> identityArgs(const TransformFn& f)
{
    if (f.name == "scale") return std::vector<double>(f.args.size(), 1.0);
    if (f.name == "matrix") return {1, 0, 0, 1, 0, 0};
    return std::vector<double>(f.args.size(), 0.0);
}

std::string renderTransform(const std::vector<TransformFn>& fns, double ox, double oy)
{
    std::string out;
    auto add = [&](const std::string& piece) {
        if (!out.empty()) out += ' ';
        out += piece;
    };
    const bool origin = ox != 0 || oy != 0;
    if (origin) add("translate(" + fmt(ox) + " " + fmt(oy) + ")");
    for (const TransformFn& f : fns)
    {
        std::string p = f.name + "(";
        for (size_t i = 0; i < f.args.size(); ++i) p += (i ? " " : "") + fmt(f.args[i]);
        add(p + ")");
    }
    if (origin) add("translate(" + fmt(-ox) + " " + fmt(-oy) + ")");
    return out;
}

// ---------------------------------------------------------------------------
// Boxes, for transform-origin
// ---------------------------------------------------------------------------
struct Box
{
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool valid = false;
    void add(double x, double y)
    {
        if (!valid) x0 = x1 = x, y0 = y1 = y, valid = true;
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
};

std::vector<double> numbers(const std::string& s)
{
    std::vector<double> out;
    const char* p = s.c_str();
    while (*p)
    {
        char* e = nullptr;
        const double v = std::strtod(p, &e);
        if (e == p) ++p;
        else out.push_back(v), p = e;
    }
    return out;
}

double num(const SvgNode& n, const char* a) { return std::strtod(n.attr(a).c_str(), nullptr); }

void boxOf(const SvgNode& n, Box& b)
{
    const std::string& t = n.name;
    if (t == "rect" || t == "image" || t == "use")
    {
        b.add(num(n, "x"), num(n, "y"));
        b.add(num(n, "x") + num(n, "width"), num(n, "y") + num(n, "height"));
    }
    else if (t == "circle")
    {
        b.add(num(n, "cx") - num(n, "r"), num(n, "cy") - num(n, "r"));
        b.add(num(n, "cx") + num(n, "r"), num(n, "cy") + num(n, "r"));
    }
    else if (t == "ellipse")
    {
        b.add(num(n, "cx") - num(n, "rx"), num(n, "cy") - num(n, "ry"));
        b.add(num(n, "cx") + num(n, "rx"), num(n, "cy") + num(n, "ry"));
    }
    else if (t == "line")
    {
        b.add(num(n, "x1"), num(n, "y1"));
        b.add(num(n, "x2"), num(n, "y2"));
    }
    else if (t == "polygon" || t == "polyline")
    {
        const std::vector<double> v = numbers(n.attr("points"));
        for (size_t i = 0; i + 1 < v.size(); i += 2) b.add(v[i], v[i + 1]);
    }
    else if (t == "path")
    {
        for (const auto& p : flattenPath(n.attr("d")).pts) b.add(p[0], p[1]);
    }
    else if (t == "g" || t == "a" || t == "svg")
        for (const auto& c : n.children) boxOf(*c, b);
}

// ---------------------------------------------------------------------------
// The cascade for one element, and the animations it asks for
// ---------------------------------------------------------------------------
struct AnimLists
{
    std::vector<std::string> name, duration, delay, count, direction, fill, timing;
};

bool isTime(const std::string& s)
{
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    const std::string u(end);
    return u == "s" || u == "ms";
}

double timeSeconds(const std::string& s)
{
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return std::string(end) == "ms" ? v / 1000.0 : v;
}

bool isTimingFn(const std::string& s)
{
    return s == "ease" || s == "linear" || s == "ease-in" || s == "ease-out" || s == "ease-in-out" || s == "step-start" ||
           s == "step-end" || s.compare(0, 13, "cubic-bezier(") == 0 || s.compare(0, 6, "steps(") == 0;
}

void setShorthand(AnimLists& a, const std::string& value)
{
    a = AnimLists();
    for (const std::string& part : splitTop(value, ','))
    {
        std::string name = "none", dur = "0s", delay = "0s", count = "1", dir = "normal", fill = "none", timing = "ease";
        int times = 0;
        for (const std::string& tok : splitTop(part, ' '))
        {
            const std::string t = lower(tok);
            if (isTime(t)) (times++ == 0 ? dur : delay) = t;
            else if (isTimingFn(t)) timing = t;
            else if (t == "infinite" || std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.') count = t;
            else if (t == "normal" || t == "reverse" || t == "alternate" || t == "alternate-reverse") dir = t;
            else if (t == "forwards" || t == "backwards" || t == "both") fill = t;
            else if (t == "running" || t == "paused" || t == "none") continue;
            else name = tok;
        }
        a.name.push_back(name), a.duration.push_back(dur), a.delay.push_back(delay), a.count.push_back(count);
        a.direction.push_back(dir), a.fill.push_back(fill), a.timing.push_back(timing);
    }
}

std::vector<std::string> listOf(const std::string& v)
{
    std::vector<std::string> out;
    for (const std::string& s : splitTop(v, ',')) out.push_back(s);
    return out;
}

const std::string& pick(const std::vector<std::string>& l, size_t i, const std::string& fallback)
{
    return l.empty() ? fallback : l[i % l.size()];
}

struct Spline
{
    double x1, y1, x2, y2;
};

Spline timingSpline(const std::string& t)
{
    if (t == "ease") return {0.25, 0.1, 0.25, 1};
    if (t == "ease-in") return {0.42, 0, 1, 1};
    if (t == "ease-out") return {0, 0, 0.58, 1};
    if (t == "ease-in-out") return {0.42, 0, 0.58, 1};
    if (t.compare(0, 13, "cubic-bezier(") == 0)
    {
        std::vector<double> v = numbers(t.substr(13));
        if (v.size() >= 4) return {v[0], v[1], v[2], v[3]};
    }
    return {0, 0, 1, 1};
}

struct Document
{
    const SvgNode* root = nullptr;
    Sheet sheet;
    double vbX = 0, vbY = 0, vbW = 800, vbH = 600;
};

struct PropFrames
{
    std::vector<double> offsets;
    std::vector<std::string> values;
    std::vector<std::string> timing; // per frame: its own animation-timing-function ("" = the element's)
};

SvgNode* addChild(SvgNode& parent, const char* name)
{
    auto n = std::make_unique<SvgNode>();
    n->name = name;
    SvgNode* raw = n.get();
    parent.children.push_back(std::move(n));
    return raw;
}

// Resolves transform-origin against the right box; false if it cannot be done.
void resolveOrigin(const Document& doc, const SvgNode& el, const std::map<std::string, std::string>& props, double& ox,
                   double& oy)
{
    ox = oy = 0;
    auto it = props.find("transform-origin");
    if (it == props.end()) return;
    std::vector<std::string> tok = splitTop(lower(it->second), ' ');
    if (tok.empty()) return;
    auto isVert = [](const std::string& s) { return s == "top" || s == "bottom"; };
    auto isHorz = [](const std::string& s) { return s == "left" || s == "right"; };
    std::string xs = tok[0], ys = tok.size() > 1 ? tok[1] : "center";
    if (tok.size() == 1 && isVert(xs)) std::swap(xs, ys);
    else if (isVert(xs) || isHorz(ys)) std::swap(xs, ys);

    double bx = doc.vbX, by = doc.vbY, bw = doc.vbW, bh = doc.vbH;
    auto bt = props.find("transform-box");
    if (bt != props.end() && lower(bt->second) == "fill-box")
    {
        Box b;
        boxOf(el, b);
        if (b.valid) bx = b.x0, by = b.y0, bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    }
    auto resolve = [](const std::string& s, double origin, double size, double& out) {
        if (s == "left" || s == "top") out = origin;
        else if (s == "right" || s == "bottom") out = origin + size;
        else if (s == "center") out = origin + size / 2;
        else if (!s.empty() && s.back() == '%') out = origin + size * std::strtod(s.c_str(), nullptr) / 100.0;
        else
        {
            double v = 0;
            parseCssNumber(s, false, v);
            out = origin + v;
        }
    };
    resolve(xs, bx, bw, ox);
    resolve(ys, by, bh, oy);
}

void emitAnimations(const Document& doc, SvgNode& el, const std::map<std::string, std::string>& props, const AnimLists& lists)
{
    for (size_t ai = 0; ai < lists.name.size(); ++ai)
    {
        const std::string& name = lists.name[ai];
        auto kf = doc.sheet.keyframes.find(name);
        if (name == "none" || kf == doc.sheet.keyframes.end() || kf->second.empty()) continue;
        const double dur = timeSeconds(pick(lists.duration, ai, "0s"));
        if (dur <= 0) continue;
        const double delay = timeSeconds(pick(lists.delay, ai, "0s"));
        const std::string countStr = lower(pick(lists.count, ai, "1"));
        const bool infinite = countStr == "infinite";
        const double count = infinite ? 0.0 : std::max(0.0, std::strtod(countStr.c_str(), nullptr));
        if (!infinite && count <= 0) continue;
        const std::string dir = lower(pick(lists.direction, ai, "normal"));
        const std::string fill = lower(pick(lists.fill, ai, "none"));
        const std::string elementTiming = lower(pick(lists.timing, ai, "ease"));
        const bool reverseFirst = dir == "reverse" || dir == "alternate-reverse";
        const bool alternate = dir == "alternate" || dir == "alternate-reverse";

        // Property -> its keyframes, in first-seen order.
        std::vector<std::string> order;
        std::map<std::string, PropFrames> byProp;
        for (const Frame& f : kf->second)
        {
            std::string frameTiming;
            for (const Decl& d : f.decls)
                if (d.prop == "animation-timing-function") frameTiming = lower(d.value);
            for (const Decl& d : f.decls)
            {
                if (d.prop.compare(0, 10, "animation-") == 0 || d.prop == "transition" || d.prop == "transform-origin" ||
                    d.prop == "transform-box")
                    continue;
                if (!byProp.count(d.prop)) order.push_back(d.prop);
                PropFrames& pf = byProp[d.prop];
                pf.offsets.push_back(f.offset);
                pf.values.push_back(d.value);
                pf.timing.push_back(frameTiming);
            }
        }

        for (const std::string& prop : order)
        {
            PropFrames pf = byProp[prop];

            // Values in SVG syntax.
            std::vector<std::string> vals = pf.values;
            if (prop == "transform")
            {
                std::vector<std::vector<TransformFn>> fns(vals.size());
                size_t ref = 0;
                for (size_t i = 0; i < vals.size(); ++i)
                {
                    if (!parseCssTransform(vals[i], fns[i])) { fns.clear(); break; }
                    if (fns[i].size() > fns[ref].size()) ref = i;
                }
                if (fns.empty()) continue;
                for (auto& f : fns) // "none" (or a shorter list) becomes the identity of the longest one
                    if (f.empty())
                        for (const TransformFn& r : fns[ref]) f.push_back({r.name, identityArgs(r)});
                double ox, oy;
                resolveOrigin(doc, el, props, ox, oy);
                for (size_t i = 0; i < vals.size(); ++i) vals[i] = renderTransform(fns[i], ox, oy);
            }
            else
            {
                for (std::string& v : vals)
                {
                    if (prop == "d" && lower(v).compare(0, 5, "path(") == 0)
                    {
                        size_t a = v.find_first_of("\"'"), b = v.find_last_of("\"'");
                        if (a != std::string::npos && b > a) v = v.substr(a + 1, b - a - 1);
                    }
                    for (char& c : v)
                        if (c == ';') c = ' ';
                }
            }

            // Implicit first / last keyframes: the element's own value.
            std::string base;
            if (prop == "transform")
            {
                std::vector<TransformFn> fns;
                size_t ref = 0;
                std::vector<std::vector<TransformFn>> all(pf.values.size());
                for (size_t i = 0; i < pf.values.size(); ++i)
                {
                    parseCssTransform(pf.values[i], all[i]);
                    if (all[i].size() > all[ref].size()) ref = i;
                }
                for (const TransformFn& r : all[ref]) fns.push_back({r.name, identityArgs(r)});
                double ox, oy;
                resolveOrigin(doc, el, props, ox, oy);
                base = renderTransform(fns, ox, oy);
            }
            else
            {
                auto it = props.find(prop);
                base = it != props.end() ? it->second : el.attr(prop);
                if (base.empty()) base = vals.front();
            }
            if (pf.offsets.front() > 0)
            {
                pf.offsets.insert(pf.offsets.begin(), 0.0);
                vals.insert(vals.begin(), base);
                pf.timing.insert(pf.timing.begin(), "");
            }
            if (pf.offsets.back() < 1)
            {
                pf.offsets.push_back(1.0);
                vals.push_back(base);
                pf.timing.push_back("");
            }
            const size_t n = vals.size();
            if (n < 2) continue;

            // Easing per interval.
            std::vector<Spline> splines(n - 1);
            bool allLinear = true;
            for (size_t i = 0; i + 1 < n; ++i)
            {
                splines[i] = timingSpline(pf.timing[i].empty() ? elementTiming : pf.timing[i]);
                if (splines[i].x1 != 0 || splines[i].y1 != 0 || splines[i].x2 != 1 || splines[i].y2 != 1) allLinear = false;
            }

            // Direction: reversed (mirrored easing) and/or forward-then-back.
            auto reversed = [&](std::vector<std::string> v, std::vector<double> t, std::vector<Spline> s) {
                std::reverse(v.begin(), v.end());
                std::reverse(t.begin(), t.end());
                for (double& x : t) x = 1.0 - x;
                std::reverse(s.begin(), s.end());
                for (Spline& sp : s) sp = {1 - sp.x2, 1 - sp.y2, 1 - sp.x1, 1 - sp.y1};
                return std::make_tuple(v, t, s);
            };
            std::vector<std::string> fv = vals;
            std::vector<double> ft = pf.offsets;
            std::vector<Spline> fs = splines;
            if (reverseFirst) std::tie(fv, ft, fs) = reversed(fv, ft, fs);
            double simpleDur = dur;
            if (alternate)
            {
                auto [bv, bt, bs] = reversed(fv, ft, fs);
                std::vector<std::string> v2 = fv;
                std::vector<double> t2;
                std::vector<Spline> s2 = fs;
                for (double t : ft) t2.push_back(t / 2);
                for (size_t i = 1; i < bv.size(); ++i)
                {
                    v2.push_back(bv[i]);
                    t2.push_back(0.5 + bt[i] / 2);
                }
                s2.insert(s2.end(), bs.begin(), bs.end());
                fv = v2, ft = t2, fs = s2;
                simpleDur = dur * 2;
            }

            const bool discrete = prop == "display" || prop == "visibility";
            auto build = [&](SvgNode* a) {
                a->setAttr("attributeName", prop);
                std::string v, k, s;
                for (size_t i = 0; i < fv.size(); ++i)
                {
                    v += (i ? ";" : "") + fv[i];
                    k += (i ? ";" : "") + fmt(ft[i]);
                }
                for (size_t i = 0; i < fs.size(); ++i)
                    s += (i ? ";" : "") + fmt(fs[i].x1) + " " + fmt(fs[i].y1) + " " + fmt(fs[i].x2) + " " + fmt(fs[i].y2);
                a->setAttr("values", v);
                a->setAttr("keyTimes", k);
                if (discrete) a->setAttr("calcMode", "discrete");
                else if (!allLinear)
                {
                    a->setAttr("calcMode", "spline");
                    a->setAttr("keySplines", s);
                }
                a->setAttr("dur", fmt(simpleDur) + "s");
                a->setAttr("begin", fmt(delay) + "s");
                a->setAttr("repeatCount", infinite ? "indefinite" : fmt(alternate ? count / 2 : count));
                if (fill == "forwards" || fill == "both") a->setAttr("fill", "freeze");
            };
            if ((fill == "backwards" || fill == "both") && delay > 0)
            {
                SvgNode* hold = addChild(el, "set");
                hold->setAttr("attributeName", prop);
                hold->setAttr("to", fv.front());
                hold->setAttr("begin", "0s");
                hold->setAttr("dur", fmt(delay) + "s");
            }
            build(addChild(el, "animate"));
        }
    }
}

struct Walker
{
    Document& doc;
    bool changed = false;

    void visit(SvgNode& el, std::vector<const SvgNode*>& path)
    {
        path.push_back(&el);
        const size_t n = el.children.size(); // (animations added by process() need no visiting)
        process(el, path);
        for (size_t i = 0; i < n; ++i) visit(*el.children[i], path);
        path.pop_back();
    }

    void process(SvgNode& el, const std::vector<const SvgNode*>& path)
    {
        if (el.name == "#text" || el.name == "style") return;
        std::vector<const Rule*> matched;
        for (const Rule& r : doc.sheet.rules)
            if (matchAt(r.sel, static_cast<int>(r.sel.parts.size()) - 1, path, static_cast<int>(path.size()) - 1)) matched.push_back(&r);
        std::stable_sort(matched.begin(), matched.end(), [](const Rule* a, const Rule* b) { return a->sel.specificity < b->sel.specificity; });
        const std::vector<Decl> inlineDecls = parseDecls(el.attr("style"));
        if (matched.empty() && !hasAnimation(inlineDecls)) return;

        std::map<std::string, std::string> props;
        std::vector<Decl> staticDecls;
        AnimLists lists;
        auto apply = [&](const Decl& d, bool fromRule) {
            const std::string& p = d.prop;
            if (p == "animation") setShorthand(lists, d.value);
            else if (p == "animation-name") lists.name = listOf(d.value);
            else if (p == "animation-duration") lists.duration = listOf(d.value);
            else if (p == "animation-delay") lists.delay = listOf(d.value);
            else if (p == "animation-iteration-count") lists.count = listOf(d.value);
            else if (p == "animation-direction") lists.direction = listOf(d.value);
            else if (p == "animation-fill-mode") lists.fill = listOf(d.value);
            else if (p == "animation-timing-function") lists.timing = listOf(d.value);
            else if (p.compare(0, 10, "animation-") == 0 || p.compare(0, 10, "transition") == 0) return;
            else
            {
                props[p] = d.value;
                if (fromRule && p != "transform-origin" && p != "transform-box") staticDecls.push_back(d);
            }
        };
        for (const Rule* r : matched)
            for (const Decl& d : r->decls) apply(d, true);
        for (const Decl& d : inlineDecls) apply(d, false);

        // Static declarations become the element's `style`, ahead of its own.
        if (!staticDecls.empty())
        {
            std::string style;
            for (const Decl& d : staticDecls)
            {
                if (d.prop == "transform")
                {
                    std::vector<TransformFn> fns;
                    if (!parseCssTransform(d.value, fns)) continue;
                    double ox, oy;
                    resolveOrigin(doc, el, props, ox, oy);
                    el.setAttr("transform", renderTransform(fns, ox, oy));
                    continue;
                }
                el.attrs.erase(std::remove_if(el.attrs.begin(), el.attrs.end(), [&](const auto& a) { return a.first == d.prop; }),
                               el.attrs.end());
                style += d.prop + ":" + d.value + ";";
            }
            style += el.attr("style");
            el.setAttr("style", style);
            changed = true;
        }
        if (!lists.name.empty())
        {
            const size_t before = el.children.size();
            emitAnimations(doc, el, props, lists);
            if (el.children.size() != before) changed = true;
        }
    }

    static bool hasAnimation(const std::vector<Decl>& decls)
    {
        for (const Decl& d : decls)
            if (d.prop.compare(0, 9, "animation") == 0) return true;
        return false;
    }
};

} // namespace

bool applySvgCss(SvgNode& root)
{
    std::string text;
    collectStyleText(root, text);
    Document doc;
    doc.root = &root;
    if (!text.empty()) parseSheet(stripComments(text), doc.sheet);
    {
        std::vector<double> vb = numbers(root.attr("viewBox"));
        if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) doc.vbX = vb[0], doc.vbY = vb[1], doc.vbW = vb[2], doc.vbH = vb[3];
        else
        {
            const double w = num(root, "width"), h = num(root, "height");
            if (w > 0 && root.attr("width").find('%') == std::string::npos) doc.vbW = w;
            if (h > 0 && root.attr("height").find('%') == std::string::npos) doc.vbH = h;
        }
    }
    if (doc.sheet.rules.empty()) return false;
    Walker w{doc};
    std::vector<const SvgNode*> path;
    w.visit(root, path);
    return w.changed;
}

} // namespace giv
