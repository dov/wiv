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
    static const std::map<std::string, unsigned> names = {
        {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000},     {"green", 0x008000},  {"blue", 0x0000ff},
        {"yellow", 0xffff00}, {"cyan", 0x00ffff},  {"aqua", 0x00ffff},   {"magenta", 0xff00ff}, {"fuchsia", 0xff00ff},
        {"gray", 0x808080},   {"grey", 0x808080},  {"silver", 0xc0c0c0}, {"maroon", 0x800000},  {"olive", 0x808000},
        {"lime", 0x00ff00},   {"teal", 0x008080},  {"navy", 0x000080},   {"purple", 0x800080},  {"orange", 0xffa500},
        {"pink", 0xffc0cb},   {"brown", 0xa52a2a}, {"gold", 0xffd700},   {"violet", 0xee82ee},  {"indigo", 0x4b0082}};
    std::string s = trim(in);
    unsigned v = 0;
    if (!s.empty() && s[0] == '#')
    {
        if (s.size() == 7 || s.size() == 4)
        {
            for (size_t i = 1; i < s.size(); ++i)
                if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
            if (s.size() == 7) v = static_cast<unsigned>(std::strtoul(s.c_str() + 1, nullptr, 16));
            else
            {
                unsigned r = std::strtoul(s.substr(1, 1).c_str(), nullptr, 16);
                unsigned g = std::strtoul(s.substr(2, 1).c_str(), nullptr, 16);
                unsigned b = std::strtoul(s.substr(3, 1).c_str(), nullptr, 16);
                v = (r * 17 << 16) | (g * 17 << 8) | (b * 17);
            }
        }
        else return false;
    }
    else if (s.compare(0, 4, "rgb(") == 0 && s.back() == ')')
    {
        std::string body = s.substr(4, s.size() - 5);
        std::vector<double> comp;
        for (const std::string& part : split(body, ','))
        {
            char* end = nullptr;
            double x = std::strtod(part.c_str(), &end);
            if (end == part.c_str()) return false;
            if (*end == '%') x = x * 255.0 / 100.0;
            comp.push_back(x);
        }
        if (comp.size() != 3) return false;
        c = {comp[0], comp[1], comp[2]};
        return true;
    }
    else
    {
        auto it = names.find(s);
        if (it == names.end()) return false;
        v = it->second;
    }
    c = {double((v >> 16) & 255), double((v >> 8) & 255), double(v & 255)};
    return true;
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

} // namespace

struct SvgAnimation::Anim
{
    enum class Kind { Animate, Set, Transform } kind = Kind::Animate;
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

    std::vector<BeginSpec> begins;
    for (size_t i = 0; i < flat.size(); ++i)
    {
        const SvgNode& n = *flat[i];
        Anim a;
        if (n.name == "animate") a.kind = Anim::Kind::Animate;
        else if (n.name == "set") a.kind = Anim::Kind::Set;
        else if (n.name == "animateTransform") a.kind = Anim::Kind::Transform;
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
        if (a.kind == Anim::Kind::Set)
        {
            if (!n.hasAttr("to")) continue;
            a.values = {a.to};
            a.hasValues = true;
        }
        else if (!a.hasValues && a.to.empty() && a.by.empty()) continue;

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
        a.additiveSum = n.attr("additive") == "sum";
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
            finiteEnd = std::max(finiteEnd, a.begin);
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
        const std::string base = target.attr(a.attr);

        std::vector<std::string> vals = a.values;
        if (!a.hasValues)
        {
            std::string from = a.from;
            if (from.empty()) from = a.kind == Anim::Kind::Transform ? transformDefault(a.transformType) : base;
            if (!a.to.empty()) vals = {from, a.to};
            else vals = {from, addValues(from, a.by)};
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
