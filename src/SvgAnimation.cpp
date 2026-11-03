#include "SvgAnimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>

namespace giv
{

namespace
{

constexpr double kInf = std::numeric_limits<double>::infinity();

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        size_t p = s.find(sep, start);
        std::string tok = trim(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (!tok.empty() || p != std::string::npos) out.push_back(tok);
        if (p == std::string::npos) break;
        start = p + 1;
    }
    if (!out.empty() && out.back().empty()) out.pop_back(); // trailing ';'
    return out;
}

// "2s", "500ms", "1.5", "0.5min", "1h", "00:01:30" -> seconds.
bool parseClock(const std::string& in, double& out)
{
    std::string s = trim(in);
    if (s.empty()) return false;
    if (s.find(':') != std::string::npos)
    {
        double secs = 0;
        for (const std::string& part : split(s, ':'))
        {
            char* end = nullptr;
            double v = std::strtod(part.c_str(), &end);
            if (end == part.c_str()) return false;
            secs = secs * 60 + v;
        }
        out = secs;
        return true;
    }
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    std::string unit = trim(end);
    if (unit.empty() || unit == "s") out = v;
    else if (unit == "ms") out = v / 1000.0;
    else if (unit == "min") out = v * 60.0;
    else if (unit == "h") out = v * 3600.0;
    else return false;
    return true;
}

// ---------------------------------------------------------------------------
// Value interpolation
// ---------------------------------------------------------------------------
struct Rgb
{
    double r, g, b;
};

bool parseColor(const std::string& in, Rgb& c)
{
    return parseSvgColor(in, c.r, c.g, c.b);
}

std::string formatNumber(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// A string split into literal text and numbers, so that two values with the
// same skeleton ("M10 20L30 40" vs "M0 0L5 5", "10px", "1 2 3 4") can be
// interpolated number by number.
struct Templated
{
    std::vector<std::string> lit; // lit.size() == nums.size() + 1
    std::vector<double> nums;
};

Templated templatize(const std::string& s)
{
    Templated t;
    std::string cur;
    size_t i = 0;
    while (i < s.size())
    {
        char c = s[i];
        bool startsNum = std::isdigit(static_cast<unsigned char>(c)) ||
                         (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) ||
                         ((c == '-' || c == '+') && i + 1 < s.size() &&
                          (std::isdigit(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '.'));
        if (startsNum)
        {
            char* end = nullptr;
            double v = std::strtod(s.c_str() + i, &end);
            t.lit.push_back(cur);
            cur.clear();
            t.nums.push_back(v);
            i = static_cast<size_t>(end - s.c_str());
        }
        else
        {
            cur += c;
            ++i;
        }
    }
    t.lit.push_back(cur);
    return t;
}

std::string render(const Templated& t, const std::vector<double>& nums)
{
    std::string out;
    for (size_t i = 0; i < nums.size(); ++i)
    {
        out += t.lit[i];
        out += formatNumber(nums[i]);
    }
    out += t.lit.back();
    return out;
}

// a + f * (b - a), or the discrete switch at 50% when the two can't be blended.
std::string lerpValue(const std::string& a, const std::string& b, double f)
{
    Rgb ca, cb;
    if (parseColor(a, ca) && parseColor(b, cb))
    {
        auto ch = [&](double x, double y) { return std::clamp(int(std::lround(x + f * (y - x))), 0, 255); };
        char buf[16];
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x", ch(ca.r, cb.r), ch(ca.g, cb.g), ch(ca.b, cb.b));
        return buf;
    }
    Templated ta = templatize(a), tb = templatize(b);
    if (!ta.nums.empty() && ta.nums.size() == tb.nums.size() && ta.lit == tb.lit)
    {
        std::vector<double> n(ta.nums.size());
        for (size_t i = 0; i < n.size(); ++i) n[i] = ta.nums[i] + f * (tb.nums[i] - ta.nums[i]);
        return render(ta, n);
    }
    return f < 0.5 ? a : b;
}

// a + b (for `by` animations); non-numeric values just take b.
std::string addValues(const std::string& a, const std::string& b)
{
    Rgb ca, cb;
    if (parseColor(a, ca) && parseColor(b, cb))
    {
        char buf[16];
        auto cl = [](double v) { return std::clamp(int(std::lround(v)), 0, 255); };
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x", cl(ca.r + cb.r), cl(ca.g + cb.g), cl(ca.b + cb.b));
        return buf;
    }
    Templated ta = templatize(a), tb = templatize(b);
    if (!ta.nums.empty() && ta.nums.size() == tb.nums.size())
    {
        std::vector<double> n(ta.nums.size());
        for (size_t i = 0; i < n.size(); ++i) n[i] = ta.nums[i] + tb.nums[i];
        return render(ta, n);
    }
    return b;
}

// Cubic Bezier easing (keySplines "x1 y1 x2 y2"): y for a given x.
double splineEase(double x1, double y1, double x2, double y2, double x)
{
    auto bez = [](double a, double b, double t) { return 3 * a * t * (1 - t) * (1 - t) + 3 * b * t * t * (1 - t) + t * t * t; };
    double lo = 0, hi = 1, t = x;
    for (int i = 0; i < 40; ++i)
    {
        t = 0.5 * (lo + hi);
        if (bez(x1, x2, t) < x) lo = t;
        else hi = t;
    }
    return bez(y1, y2, t);
}

std::string transformDefault(const std::string& type)
{
    if (type == "scale") return "1";
    return "0";
}


// ---------------------------------------------------------------------------
// Motion paths (animateMotion)
// ---------------------------------------------------------------------------
struct MotionPath
{
    std::vector<std::array<double, 2>> pts;
    std::vector<double> cum; // cumulative length at each point
    double total() const { return cum.empty() ? 0.0 : cum.back(); }

    void add(double x, double y, bool jump)
    {
        double len = 0;
        if (!pts.empty() && !jump) len = std::hypot(x - pts.back()[0], y - pts.back()[1]);
        pts.push_back({x, y});
        cum.push_back((cum.empty() ? 0.0 : cum.back()) + len);
    }

    // Position and tangent angle (degrees) at fraction f (0..1) of the length.
    void at(double f, double& x, double& y, double& angle) const
    {
        x = y = angle = 0;
        if (pts.empty()) return;
        if (pts.size() == 1 || total() <= 0)
        {
            x = pts[0][0];
            y = pts[0][1];
            return;
        }
        const double d = std::clamp(f, 0.0, 1.0) * total();
        size_t i = 1;
        while (i + 1 < pts.size() && cum[i] < d) ++i;
        double seg = cum[i] - cum[i - 1];
        double u = seg > 0 ? (d - cum[i - 1]) / seg : 0.0;
        x = pts[i - 1][0] + u * (pts[i][0] - pts[i - 1][0]);
        y = pts[i - 1][1] + u * (pts[i][1] - pts[i - 1][1]);
        // tangent of the nearest segment with length
        size_t j = i;
        while (j > 1 && cum[j] - cum[j - 1] <= 0) --j;
        angle = std::atan2(pts[j][1] - pts[j - 1][1], pts[j][0] - pts[j - 1][0]) * 180.0 / M_PI;
    }
};

// SVG path data -> polyline. Curves are subdivided; arcs are approximated by
// their chord.
MotionPath flattenPath(const std::string& d)
{
    MotionPath mp;
    const char* p = d.c_str();
    auto skip = [&] { while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p; };
    auto num = [&](double& v) {
        skip();
        char* e = nullptr;
        v = std::strtod(p, &e);
        if (e == p) return false;
        p = e;
        return true;
    };
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; // current, subpath start, last control
    char cmd = 0, prev = 0;
    constexpr int kSteps = 20;
    while (true)
    {
        skip();
        if (!*p) break;
        if (std::isalpha(static_cast<unsigned char>(*p))) cmd = *p++;
        else if (!cmd) break;
        const bool rel = std::islower(static_cast<unsigned char>(cmd));
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        double v[7];
        auto need = [&](int n) {
            for (int i = 0; i < n; ++i)
                if (!num(v[i])) return false;
            return true;
        };
        const double ox = rel ? cx : 0, oy = rel ? cy : 0;
        bool ok = true;
        switch (up)
        {
        case 'M':
            if (!(ok = need(2))) break;
            cx = sx = ox + v[0];
            cy = sy = oy + v[1];
            mp.add(cx, cy, true);
            cmd = rel ? 'l' : 'L'; // further pairs are line-tos
            break;
        case 'L':
            if (!(ok = need(2))) break;
            cx = ox + v[0];
            cy = oy + v[1];
            mp.add(cx, cy, false);
            break;
        case 'H':
            if (!(ok = need(1))) break;
            cx = ox + v[0];
            mp.add(cx, cy, false);
            break;
        case 'V':
            if (!(ok = need(1))) break;
            cy = oy + v[0];
            mp.add(cx, cy, false);
            break;
        case 'C':
        case 'S':
        {
            double x1, y1;
            if (up == 'C')
            {
                if (!(ok = need(6))) break;
                x1 = ox + v[0]; y1 = oy + v[1];
                v[0] = ox + v[2]; v[1] = oy + v[3]; v[2] = ox + v[4]; v[3] = oy + v[5];
            }
            else
            {
                if (!(ok = need(4))) break;
                bool smooth = prev == 'C' || prev == 'S';
                x1 = smooth ? 2 * cx - lcx : cx;
                y1 = smooth ? 2 * cy - lcy : cy;
                v[0] = ox + v[0]; v[1] = oy + v[1]; v[2] = ox + v[2]; v[3] = oy + v[3];
            }
            const double x2 = v[0], y2 = v[1], x3 = v[2], y3 = v[3];
            for (int i = 1; i <= kSteps; ++i)
            {
                double t = double(i) / kSteps, u = 1 - t;
                mp.add(u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                       u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3, false);
            }
            lcx = x2; lcy = y2; cx = x3; cy = y3;
            break;
        }
        case 'Q':
        case 'T':
        {
            double x1, y1;
            if (up == 'Q')
            {
                if (!(ok = need(4))) break;
                x1 = ox + v[0]; y1 = oy + v[1];
                v[0] = ox + v[2]; v[1] = oy + v[3];
            }
            else
            {
                if (!(ok = need(2))) break;
                bool smooth = prev == 'Q' || prev == 'T';
                x1 = smooth ? 2 * cx - lcx : cx;
                y1 = smooth ? 2 * cy - lcy : cy;
                v[0] = ox + v[0]; v[1] = oy + v[1];
            }
            for (int i = 1; i <= kSteps; ++i)
            {
                double t = double(i) / kSteps, u = 1 - t;
                mp.add(u * u * cx + 2 * u * t * x1 + t * t * v[0], u * u * cy + 2 * u * t * y1 + t * t * v[1], false);
            }
            lcx = x1; lcy = y1; cx = v[0]; cy = v[1];
            break;
        }
        case 'A':
            if (!(ok = need(7))) break;
            cx = ox + v[5];
            cy = oy + v[6];
            mp.add(cx, cy, false);
            break;
        case 'Z':
            cx = sx;
            cy = sy;
            mp.add(cx, cy, false);
            break;
        default: ok = false;
        }
        if (!ok) break;
        prev = up;
        if (up != 'C' && up != 'S' && up != 'Q' && up != 'T') { lcx = cx; lcy = cy; }
    }
    return mp;
}

// ---------------------------------------------------------------------------
// Lengths: "50px" / "90%" -> plain pixel numbers for geometry attributes, so
// that from="50" to="90%" can be interpolated. (nanosvg has no viewport to
// resolve percentages against.)
// ---------------------------------------------------------------------------
enum class Axis { None, X, Y, Diag };

Axis axisOf(const std::string& attr)
{
    static const std::map<std::string, Axis> axes = {
        {"x", Axis::X},   {"cx", Axis::X},  {"x1", Axis::X}, {"x2", Axis::X}, {"width", Axis::X}, {"rx", Axis::X},
        {"y", Axis::Y},   {"cy", Axis::Y},  {"y1", Axis::Y}, {"y2", Axis::Y}, {"height", Axis::Y}, {"ry", Axis::Y},
        {"r", Axis::Diag}};
    auto it = axes.find(attr);
    return it == axes.end() ? Axis::None : it->second;
}

std::string resolveLength(const std::string& in, Axis axis, double vw, double vh)
{
    if (axis == Axis::None) return in;
    std::string s = trim(in);
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return in;
    std::string unit = trim(end);
    if (unit.empty() || unit == "px") return formatNumber(v);
    if (unit == "%")
    {
        double ref = axis == Axis::X ? vw : axis == Axis::Y ? vh : std::sqrt((vw * vw + vh * vh) / 2.0);
        return formatNumber(v * ref / 100.0);
    }
    return in;
}

} // namespace

struct SvgAnimation::Anim
{
    enum class Kind { Animate, Set, Transform, Motion } kind = Kind::Animate;
    size_t target = 0; // preorder index of the animated element
    std::string attr;
    std::string transformType;
    std::vector<std::string> values; // always >= 1 (after resolving from/to/by)
    bool hasValues = false;          // values given explicitly (vs. derived from the base value)
    std::string from, to, by;
    std::vector<double> keyTimes;
    std::vector<std::array<double, 4>> keySplines;
    enum class Calc { Linear, Discrete, Spline } calc = Calc::Linear;
    bool additiveSum = false;
    bool freeze = false;
    double begin = 0;
    double dur = kInf; // simple duration (kInf = indefinite)
    double repeatCount = 1;
    double repeatDur = kInf;
    // animateMotion
    MotionPath path;
    std::string rotate; // "", "auto", "auto-reverse" or degrees
    std::vector<double> keyPoints;
    std::string id;

    double activeDur() const
    {
        double a = std::isinf(dur) ? kInf : dur * repeatCount;
        return std::min(a, repeatDur);
    }
    double end() const { return begin + activeDur(); }
};

namespace
{

void flatten(const SvgNode& n, std::vector<const SvgNode*>& out)
{
    out.push_back(&n);
    for (const auto& c : n.children) flatten(*c, out);
}

void flattenMut(SvgNode& n, std::vector<SvgNode*>& out)
{
    out.push_back(&n);
    for (auto& c : n.children) flattenMut(*c, out);
}

struct BeginSpec
{
    bool resolved = false;
    double offset = 0;
    std::string ref; // syncbase element id
    bool refEnd = false;
};

// begin="2s" or "id.end+1s" / "id.begin"; event-based/unknown values fail.
bool parseBegin(const std::string& attr, BeginSpec& out)
{
    if (attr.empty())
    {
        out.resolved = true;
        return true;
    }
    for (const std::string& tok : split(attr, ';'))
    {
        double v;
        if (parseClock(tok, v))
        {
            out.resolved = true;
            out.offset = v;
            return true;
        }
        size_t dot = tok.find('.');
        if (dot == std::string::npos) continue;
        std::string ref = trim(tok.substr(0, dot));
        std::string rest = trim(tok.substr(dot + 1));
        bool isEnd = rest.compare(0, 3, "end") == 0;
        if (!isEnd && rest.compare(0, 5, "begin") != 0) continue;
        rest = trim(rest.substr(isEnd ? 3 : 5));
        double off = 0;
        if (!rest.empty())
        {
            std::string num = rest;
            if (num[0] == '+') num = trim(num.substr(1));
            else if (num[0] == '-') num = "-" + trim(num.substr(1));
            if (!parseClock(num, off)) continue;
        }
        out.ref = ref;
        out.refEnd = isEnd;
        out.offset = off;
        return true;
    }
    return false;
}

void setStyleFree(SvgNode& n, const std::string& attr, const std::string& value)
{
    // A style="attr:..." declaration would override the attribute; drop it.
    if (n.hasAttr("style"))
    {
        std::string out;
        for (const std::string& decl : split(n.attr("style"), ';'))
        {
            size_t colon = decl.find(':');
            if (colon != std::string::npos && trim(decl.substr(0, colon)) == attr) continue;
            if (!decl.empty()) out += decl + ";";
        }
        n.setAttr("style", out);
    }
    n.setAttr(attr, value);
}

} // namespace

SvgAnimation::SvgAnimation(const SvgNode& root) : root_(root)
{
    std::vector<const SvgNode*> flat;
    flatten(root, flat);
    std::unordered_map<const SvgNode*, size_t> indexOf;
    std::unordered_map<std::string, size_t> idToIndex;
    for (size_t i = 0; i < flat.size(); ++i)
    {
        indexOf[flat[i]] = i;
        if (flat[i]->hasAttr("id")) idToIndex.emplace(flat[i]->attr("id"), i);
    }
    // parent index of each node
    std::vector<size_t> parent(flat.size(), 0);
    for (size_t i = 0; i < flat.size(); ++i)
        for (const auto& c : flat[i]->children) parent[indexOf[c.get()]] = i;

    {
        std::vector<double> vb = {};
        {
            Templated t = templatize(root.attr("viewBox"));
            vb = t.nums;
        }
        auto px = [](const std::string& v, double& out) {
            Templated t = templatize(v);
            if (t.nums.size() != 1 || t.lit.back().find('%') != std::string::npos) return false;
            out = t.nums[0];
            return true;
        };
        if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0)
        {
            vw_ = vb[2];
            vh_ = vb[3];
        }
        else
        {
            double v;
            if (px(root.attr("width"), v) && v > 0) vw_ = v;
            if (px(root.attr("height"), v) && v > 0) vh_ = v;
        }
    }

    std::vector<BeginSpec> begins;
    for (size_t i = 0; i < flat.size(); ++i)
    {
        const SvgNode& n = *flat[i];
        Anim a;
        if (n.name == "animate") a.kind = Anim::Kind::Animate;
        else if (n.name == "set") a.kind = Anim::Kind::Set;
        else if (n.name == "animateTransform") a.kind = Anim::Kind::Transform;
        else if (n.name == "animateMotion") a.kind = Anim::Kind::Motion;
        else continue;

        std::string href = n.hasAttr("xlink:href") ? n.attr("xlink:href") : n.attr("href");
        if (!href.empty() && href[0] == '#')
        {
            auto it = idToIndex.find(href.substr(1));
            if (it == idToIndex.end()) continue;
            a.target = it->second;
        }
        else if (i == 0) continue;
        else a.target = parent[i];

        if (a.kind == Anim::Kind::Transform)
        {
            a.attr = "transform";
            a.transformType = n.attr("type").empty() ? "translate" : n.attr("type");
        }
        else if (a.kind == Anim::Kind::Motion) a.attr = "transform";
        else a.attr = n.attr("attributeName");
        if (a.attr.empty()) continue;

        a.id = n.attr("id");
        a.from = n.attr("from");
        a.to = n.attr("to");
        a.by = n.attr("by");
        if (n.hasAttr("values"))
        {
            a.values = split(n.attr("values"), ';');
            a.hasValues = !a.values.empty();
        }
        if (a.kind == Anim::Kind::Motion)
        {
            std::string d = n.attr("path");
            for (const auto& c : n.children) // <mpath xlink:href="#id"/>
                if (c->name == "mpath")
                {
                    std::string h = c->hasAttr("xlink:href") ? c->attr("xlink:href") : c->attr("href");
                    auto it = h.empty() || h[0] != '#' ? idToIndex.end() : idToIndex.find(h.substr(1));
                    if (it != idToIndex.end()) d = flat[it->second]->attr("d");
                }
            if (d.empty() && !a.values.empty())
            {
                d = "M" + a.values[0];
                for (size_t k = 1; k < a.values.size(); ++k) d += " L" + a.values[k];
            }
            else if (d.empty() && !a.to.empty()) d = "M" + (a.from.empty() ? std::string("0,0") : a.from) + " L" + a.to;
            else if (d.empty() && !a.by.empty()) d = "M0,0 l" + a.by;
            a.path = flattenPath(d);
            if (a.path.pts.empty()) continue;
            a.rotate = n.attr("rotate");
            for (const std::string& k : split(n.attr("keyPoints"), ';')) a.keyPoints.push_back(std::strtod(k.c_str(), nullptr));
        }
        else if (a.kind == Anim::Kind::Set)
        {
            if (!n.hasAttr("to")) continue;
            a.values = {a.to};
            a.hasValues = true;
        }
        else if (a.kind != Anim::Kind::Motion && !a.hasValues && a.to.empty() && a.by.empty()) continue;

        for (const std::string& k : split(n.attr("keyTimes"), ';')) a.keyTimes.push_back(std::strtod(k.c_str(), nullptr));
        const std::string calc = n.attr("calcMode");
        if (calc == "discrete") a.calc = Anim::Calc::Discrete;
        else if (calc == "spline") a.calc = Anim::Calc::Spline;
        if (a.calc == Anim::Calc::Spline)
            for (const std::string& ks : split(n.attr("keySplines"), ';'))
            {
                std::array<double, 4> sp{0, 0, 1, 1};
                const char* p = ks.c_str();
                for (double& v : sp)
                {
                    char* e = nullptr;
                    v = std::strtod(p, &e);
                    p = e;
                    while (*p == ',' || std::isspace(static_cast<unsigned char>(*p))) ++p;
                }
                a.keySplines.push_back(sp);
            }
        a.additiveSum = n.attr("additive") == "sum" || a.kind == Anim::Kind::Motion;
        a.freeze = n.attr("fill") == "freeze";

        if (n.hasAttr("dur") && n.attr("dur") != "indefinite")
        {
            double d;
            if (!parseClock(n.attr("dur"), d) || d <= 0) continue;
            a.dur = d;
        }
        else if (a.kind != Anim::Kind::Set) continue; // animate without a duration does nothing
        const std::string& rc = n.attr("repeatCount");
        if (rc == "indefinite") a.repeatCount = kInf;
        else if (!rc.empty()) a.repeatCount = std::max(0.0, std::strtod(rc.c_str(), nullptr));
        double rd;
        if (n.attr("repeatDur") == "indefinite") a.repeatDur = kInf;
        else if (parseClock(n.attr("repeatDur"), rd)) a.repeatDur = rd;
        if (a.kind == Anim::Kind::Set && std::isinf(a.dur)) a.freeze = true;

        BeginSpec b;
        if (!parseBegin(n.attr("begin"), b)) continue;
        a.begin = b.offset;
        anims_.push_back(std::move(a));
        begins.push_back(b);
    }

    // Resolve syncbase begin times (begin="other.end+1s"); a few passes handle chains.
    for (int pass = 0; pass < 8; ++pass)
        for (size_t i = 0; i < anims_.size(); ++i)
        {
            if (begins[i].resolved || begins[i].ref.empty()) continue;
            for (size_t j = 0; j < anims_.size(); ++j)
            {
                if (i == j || anims_[j].id != begins[i].ref || !begins[j].resolved) continue;
                double base = begins[i].refEnd ? anims_[j].end() : anims_[j].begin;
                if (std::isinf(base)) break;
                anims_[i].begin = base + begins[i].offset;
                begins[i].resolved = true;
                break;
            }
        }
    {
        std::vector<Anim> kept;
        for (size_t i = 0; i < anims_.size(); ++i)
            if (begins[i].resolved || begins[i].ref.empty()) kept.push_back(std::move(anims_[i]));
        // (an unresolved reference leaves begin at the plain offset, i.e. 0)
        anims_ = std::move(kept);
    }

    // Timeline length: finite ends, plus one common period of the repeating ones.
    double finiteEnd = 0, repeatStart = 0;
    long long periodMs = 1;
    for (const Anim& a : anims_)
    {
        if (a.kind == Anim::Kind::Set && std::isinf(a.dur))
        {
            finiteEnd = std::max(finiteEnd, a.begin + 0.5); // leave time to show the new state
            continue;
        }
        if (std::isinf(a.activeDur()))
        {
            loops_ = true;
            repeatStart = std::max(repeatStart, a.begin);
            long long ms = std::max<long long>(1, std::llround(a.dur * 1000.0));
            long long g = std::gcd(periodMs, ms);
            periodMs = std::min<long long>(periodMs / g * ms, 120000);
        }
        else finiteEnd = std::max(finiteEnd, a.end());
    }
    duration_ = loops_ ? std::max(finiteEnd, repeatStart + periodMs / 1000.0) : finiteEnd;
}

SvgAnimation::~SvgAnimation() = default;

std::unique_ptr<SvgNode> SvgAnimation::evaluate(double t) const
{
    auto dom = root_.clone();
    std::vector<SvgNode*> flat;
    flattenMut(*dom, flat);

    // Per (target, attribute): the transform string being composed.
    std::map<std::pair<size_t, std::string>, std::string> transforms;

    for (const Anim& a : anims_)
    {
        if (t < a.begin) continue;
        const double local = t - a.begin;
        const double active = a.activeDur();
        double progress; // position within the current simple duration, 0..1
        if (local >= active)
        {
            if (!a.freeze) continue;
            progress = 1.0;
        }
        else if (std::isinf(a.dur)) progress = 0.0;
        else progress = std::fmod(local, a.dur) / a.dur;

        SvgNode& target = *flat[a.target];
        const Axis axis = a.kind == Anim::Kind::Animate || a.kind == Anim::Kind::Set ? axisOf(a.attr) : Axis::None;
        const std::string base = resolveLength(target.attr(a.attr), axis, vw_, vh_);

        if (a.kind == Anim::Kind::Motion)
        {
            double f = progress;
            if (a.keyPoints.size() >= 2 && a.keyPoints.size() == a.keyTimes.size())
            {
                const auto& kt = a.keyTimes;
                size_t seg = 0;
                while (seg + 2 < kt.size() && progress >= kt[seg + 1]) ++seg;
                double span = kt[seg + 1] - kt[seg];
                double u = span > 0 ? std::clamp((progress - kt[seg]) / span, 0.0, 1.0) : 0.0;
                f = a.keyPoints[seg] + u * (a.keyPoints[seg + 1] - a.keyPoints[seg]);
            }
            double x, y, angle;
            a.path.at(f, x, y, angle);
            std::string piece = "translate(" + formatNumber(x) + " " + formatNumber(y) + ")";
            if (a.rotate == "auto") piece += " rotate(" + formatNumber(angle) + ")";
            else if (a.rotate == "auto-reverse") piece += " rotate(" + formatNumber(angle + 180.0) + ")";
            else if (!a.rotate.empty()) piece += " rotate(" + formatNumber(std::strtod(a.rotate.c_str(), nullptr)) + ")";
            auto key = std::make_pair(a.target, a.attr);
            auto it = transforms.find(key);
            std::string prev = it != transforms.end() ? it->second : base;
            transforms[key] = prev.empty() ? piece : prev + " " + piece;
            setStyleFree(target, "transform", transforms[key]);
            continue;
        }

        std::vector<std::string> vals = a.values;
        for (std::string& v : vals) v = resolveLength(v, axis, vw_, vh_);
        if (!a.hasValues)
        {
            std::string from = resolveLength(a.from, axis, vw_, vh_);
            if (from.empty()) from = a.kind == Anim::Kind::Transform ? transformDefault(a.transformType) : base;
            if (!a.to.empty()) vals = {from, resolveLength(a.to, axis, vw_, vh_)};
            else vals = {from, addValues(from, resolveLength(a.by, axis, vw_, vh_))};
        }
        if (vals.empty()) continue;

        // keyTimes for this animation
        const size_t n = vals.size();
        std::vector<double> kt = a.keyTimes;
        if (kt.size() != n)
        {
            kt.assign(n, 0.0);
            for (size_t i = 0; i < n; ++i)
                kt[i] = a.calc == Anim::Calc::Discrete ? double(i) / n : (n > 1 ? double(i) / (n - 1) : 0.0);
        }

        std::string value;
        if (n == 1) value = vals[0];
        else if (a.calc == Anim::Calc::Discrete)
        {
            size_t idx = 0;
            for (size_t i = 0; i < n; ++i)
                if (progress >= kt[i]) idx = i;
            value = vals[idx];
        }
        else if (progress >= 1.0) value = vals.back();
        else
        {
            size_t seg = 0;
            while (seg + 2 < n && progress >= kt[seg + 1]) ++seg;
            double span = kt[seg + 1] - kt[seg];
            double f = span > 0 ? std::clamp((progress - kt[seg]) / span, 0.0, 1.0) : 0.0;
            if (a.calc == Anim::Calc::Spline && seg < a.keySplines.size())
            {
                const auto& sp = a.keySplines[seg];
                f = splineEase(sp[0], sp[1], sp[2], sp[3], f);
            }
            value = lerpValue(vals[seg], vals[seg + 1], f);
        }

        if (a.kind == Anim::Kind::Transform)
        {
            std::string piece = a.transformType + "(" + value + ")";
            auto key = std::make_pair(a.target, a.attr);
            auto it = transforms.find(key);
            if (a.additiveSum)
            {
                std::string prev = it != transforms.end() ? it->second : base;
                transforms[key] = prev.empty() ? piece : prev + " " + piece;
            }
            else transforms[key] = piece;
            setStyleFree(target, "transform", transforms[key]);
        }
        else
        {
            if (a.additiveSum) value = addValues(base, value);
            setStyleFree(target, a.attr, value);
        }
    }
    return dom;
}

} // namespace giv
