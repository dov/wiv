#include "GivParser.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace giv
{

namespace
{

constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

// Cycles through these default colors per-dataset, same palette giv uses
// when no explicit $color directive is given.
const Color kDefaultColors[] = {
    Color::opaque(1, 0, 0),
    Color::opaque(0, 1, 0),
    Color::opaque(0, 0, 1),
    Color::opaque(1, 1, 0),
    Color::opaque(0, 1, 1),
    Color::opaque(1, 0, 1),
};

std::string toLower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool startsWithCI(const std::string& s, const std::string& prefix)
{
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

// Minimal reimplementation of giv's WordBoundaries: splits a line into
// whitespace-separated word offsets and provides typed accessors, including
// "rest of line from word N" (preserving original inter-word spacing).
class Tokens
{
public:
    Tokens(const char* line, size_t len) : line_(line), len_(len)
    {
        size_t p = 0;
        while (p < len_)
        {
            while (p < len_ && std::isspace(static_cast<unsigned char>(line_[p]))) ++p;
            size_t start = p;
            while (p < len_ && !std::isspace(static_cast<unsigned char>(line_[p]))) ++p;
            if (p != start) bounds_.emplace_back(start, p);
        }
    }

    size_t size() const { return bounds_.size(); }
    bool empty() const { return bounds_.empty(); }

    std::string get(size_t i) const
    {
        if (i >= bounds_.size()) return {};
        return std::string(line_ + bounds_[i].first, line_ + bounds_[i].second);
    }

    std::string getLower(size_t i) const { return toLower(get(i)); }

    // Rest of the line starting at word i's first character, through the
    // end of the line (preserves spacing between words, like giv's
    // GetRestAsString).
    std::string getRest(size_t i) const
    {
        if (i >= bounds_.size()) return {};
        size_t start = bounds_[i].first;
        return std::string(line_ + start, line_ + len_);
    }

    double getFloat(size_t i) const
    {
        if (i >= bounds_.size()) return std::nan("");
        double v = 0;
        auto [p, ec] = std::from_chars(line_ + bounds_[i].first, line_ + bounds_[i].second, v);
        (void)p;
        if (ec != std::errc()) return std::nan("");
        return v;
    }

    int getInt(size_t i) const
    {
        if (i >= bounds_.size()) return 0;
        return std::atoi(get(i).c_str());
    }

private:
    const char* line_;
    size_t len_;
    std::vector<std::pair<size_t, size_t>> bounds_;
};

// Mirrors SceneBuilder::resolveFont's extraction of a trailing point-size
// token from a Pango-style "$font" spec (e.g. "Serif Bold 50" -> 50), since
// that's what actually determines the rendered glyph size - an explicit
// $text_size is only a fallback when the font spec has no trailing number.
double effectiveTextSize(const std::string& fontName, double textSize)
{
    if (!fontName.empty())
    {
        size_t lastSpace = fontName.find_last_of(' ');
        if (lastSpace != std::string::npos)
        {
            std::string tail = fontName.substr(lastSpace + 1);
            if (!tail.empty() && std::isdigit(static_cast<unsigned char>(tail[0])))
                return std::atof(tail.c_str());
        }
    }
    return textSize > 0 ? textSize : 12.0;
}

MarkType parseMarkType(const std::string& lower)
{
    if (lower == "circle") return MarkType::Circle;
    if (lower == "fcircle") return MarkType::FCircle;
    if (lower == "square") return MarkType::Square;
    if (lower == "fsquare") return MarkType::FSquare;
    if (lower == "pixel") return MarkType::Pixel;
    return MarkType::Circle;
}

// Splits a comma-or-space separated list of numbers, e.g. giv's $linedash
// "10,5" syntax.
std::vector<float> parseDashList(std::string s)
{
    for (char& c : s)
        if (c == ',') c = ' ';
    std::vector<float> out;
    Tokens t(s.data(), s.size());
    for (size_t i = 0; i < t.size(); ++i) out.push_back(static_cast<float>(t.getFloat(i)));
    return out;
}

struct MappedFile
{
    const char* data = nullptr;
    size_t size = 0;
    void* mmapBase = nullptr;
    size_t mmapLen = 0;
    std::vector<char> fallbackBuffer;

    ~MappedFile()
    {
        if (mmapBase) munmap(mmapBase, mmapLen);
    }

    bool load(const std::string& filename)
    {
        int fd = ::open(filename.c_str(), O_RDONLY);
        if (fd < 0) return false;

        struct stat st{};
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
        {
            void* addr = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
            ::close(fd);
            if (addr != MAP_FAILED)
            {
                mmapBase = addr;
                mmapLen = static_cast<size_t>(st.st_size);
                data = static_cast<const char*>(addr);
                size = mmapLen;
                return true;
            }
            // fall through to buffered read below on mmap failure
            fd = ::open(filename.c_str(), O_RDONLY);
            if (fd < 0) return false;
        }

        // Fallback: non-seekable input (pipe/stdin) or zero-size regular file.
        char buf[1 << 16];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0)
            fallbackBuffer.insert(fallbackBuffer.end(), buf, buf + n);
        ::close(fd);
        data = fallbackBuffer.data();
        size = fallbackBuffer.size();
        return true;
    }
};

} // namespace

void GivParser::defStyle(const std::string& styleName, const std::string& propString)
{
    if (propString.empty()) return;
    Tokens t(propString.data(), propString.size());
    if (t.empty()) return;
    std::string key = t.getLower(0);

    auto& props = styleDefs_[styleName];
    for (auto& existing : props)
    {
        Tokens et(existing.data(), existing.size());
        if (!et.empty() && et.getLower(0) == key)
        {
            existing = propString;
            return;
        }
    }
    props.push_back(propString);
}

void GivParser::applyStyleLine(Dataset& ds, const std::string& key, const std::string& rest)
{
    Tokens t(rest.data(), rest.size());
    if (key == "lw")
    {
        ds.lineWidth = t.getFloat(0);
    }
    else if (key == "color")
    {
        Color c;
        if (parseGivColor(t.get(0), c)) ds.color = c;
    }
    else if (key == "outline_color")
    {
        Color c;
        if (parseGivColor(t.get(0), c)) ds.outlineColor = c;
        ds.doDrawPolygonOutline = true;
    }
    else if (key == "marks")
    {
        ds.doDrawMarks = true;
        ds.markType = parseMarkType(t.getLower(0));
    }
    else if (key == "noline")
    {
        ds.doDrawLines = false;
    }
    else if (key == "line")
    {
        ds.doDrawLines = true;
    }
    else if (key == "scale_marks")
    {
        ds.doScaleMarks = t.empty() ? true : (t.getInt(0) != 0);
    }
    else if (key == "mark_size")
    {
        if (!t.empty()) ds.markSize = t.getFloat(0);
    }
    else if (key == "nomark")
    {
        ds.doDrawMarks = false;
    }
    else if (key == "polygon")
    {
        ds.doDrawPolygon = true;
    }
    else if (key == "arrow")
    {
        std::string a = t.empty() ? "end" : t.getLower(0);
        if (a == "start") ds.arrowType = ArrowType::Start;
        else if (a == "both") ds.arrowType = ArrowType::Both;
        else ds.arrowType = ArrowType::End;
    }
    // unrecognized style keys are ignored (compatibility no-op)
}

void GivParser::applyStyle(Dataset& ds, const std::string& styleName)
{
    auto it = styleDefs_.find(styleName);
    if (it == styleDefs_.end()) return;
    for (const auto& propString : it->second)
    {
        Tokens t(propString.data(), propString.size());
        if (t.empty()) continue;
        std::string key = t.getLower(0);
        std::string rest = t.size() > 1 ? propString.substr(propString.find_first_of(" \t", 0) + 1) : std::string();
        // trim any leading whitespace left over
        size_t s = rest.find_first_not_of(" \t");
        if (s != std::string::npos) rest = rest.substr(s);
        else rest.clear();
        applyStyleLine(ds, key, rest);
    }
}

void GivParser::parseLine(Dataset& ds, const char* line, size_t len, SceneData& scene)
{
    if (len == 0) return;

    double marksHalf = ds.doScaleMarks ? 0.5 * ds.markSize : 0.0;
    auto pushPoint = [&](double px, double py, Op op) {
        ds.addPoint(static_cast<float>(px), static_cast<float>(py), op);
        scene.updateBounds(px, py, marksHalf);
    };

    char ch = line[0];

    // Fast path for bare "x y" coordinate lines (the hot path for point clouds).
    if ((ch >= '0' && ch <= '9') || ch == '-')
    {
        double x = 0, y = 0;
        auto [p1, ec1] = std::from_chars(line, line + len, x);
        if (ec1 == std::errc())
        {
            const char* p = p1;
            const char* end = line + len;
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            auto [p2, ec2] = std::from_chars(p, end, y);
            if (ec2 == std::errc())
            {
                pushPoint(x, y, ds.pointCount() == 0 ? Op::Move : Op::Draw);
                return;
            }
        }
        // fall through to full tokenizer if the fast path didn't fully match
    }

    if (ch == '#') return; // comment
    if (ch == '"') return; // dead/no-op in giv itself; accept-and-ignore

    Tokens tok(line, len);
    if (tok.empty()) return;

    if (ch == '$')
    {
        std::string w0 = tok.getLower(0);
        if (w0 == "$lw") ds.lineWidth = tok.getFloat(1);
        else if (w0 == "$line_cap")
        {
            std::string capStr = toLower(tok.getRest(1));
            if (startsWithCI(capStr, "but")) ds.lineCap = 0;
            else if (startsWithCI(capStr, "square")) ds.lineCap = 2;
            else ds.lineCap = 1;
        }
        else if (w0 == "$balloon")
        {
            std::string s = tok.getRest(1);
            if (!ds.balloon.empty()) ds.balloon += "\n";
            ds.balloon += s;
        }
        else if (w0 == "$color")
        {
            Color c;
            std::string spec = tok.getRest(1);
            if (parseGivColor(spec, c)) ds.color = c;
        }
        else if (w0 == "$outline_color")
        {
            Color c;
            std::string spec = tok.getRest(1);
            if (parseGivColor(spec, c)) ds.outlineColor = c;
            ds.doDrawPolygonOutline = true;
        }
        else if (w0 == "$quiver_color")
        {
            Color c;
            std::string spec = tok.getRest(1);
            if (parseGivColor(spec, c)) ds.quiverColor = c;
        }
        else if (w0 == "$shadow_color" || w0 == "$shadow_offset")
        {
            // Phase 1: drop-shadow rendering not implemented; accept-and-ignore.
        }
        else if (w0 == "$quiver_scale")
        {
            ds.quiverScale = tok.getFloat(1);
        }
        else if (w0 == "$quiver_head")
        {
            std::string a = tok.getRest(1);
            ds.quiverHead = (a != "none");
        }
        else if (w0 == "$marks")
        {
            ds.doDrawMarks = true;
            ds.markType = parseMarkType(tok.getLower(1));
        }
        else if (w0 == "$noline") ds.doDrawLines = false;
        else if (w0 == "$scale_marks") ds.doScaleMarks = (tok.size() == 1) ? true : (tok.getInt(1) != 0);
        else if (w0 == "$scale_font") ds.doScaleFonts = (tok.size() == 1) ? true : (tok.getInt(1) != 0);
        else if (w0 == "$pango_markup")
        {
            // Phase 1: markup parsing not implemented, text drawn as plain.
        }
        else if (w0 == "$path") ds.pathName = tok.getRest(1);
        else if (w0 == "$mark_size") ds.markSize = tok.getFloat(1);
        else if (w0 == "$text_size") ds.textSize = tok.getFloat(1);
        else if (w0 == "$text_angle") ds.textAngle = tok.getFloat(1) * kDeg2Rad;
        else if (w0 == "$font") ds.fontName = tok.getRest(1);
        else if (w0 == "$text_style")
        {
            std::string s = toLower(tok.getRest(1));
            ds.textStyle = (s == "shadow") ? TextStyle::DropShadow : TextStyle::Normal;
        }
        else if (w0 == "$nomark") ds.doDrawMarks = false;
        else if (w0 == "$line") ds.doDrawLines = true;
        else if (w0 == "$image" || w0 == "$marks_file" || w0 == "$svg" || w0 == "$svgmarks")
        {
            // Phase 1 scope excludes images/SVG references; accept-and-ignore.
        }
        else if (w0 == "$polygon") ds.doDrawPolygon = true;
        else if (w0 == "$low_contrast")
        {
            // colormap/pseudo-color out of Phase 1 scope; accept-and-ignore.
        }
        else if (w0 == "$vflip" || w0 == "$vlock" || w0 == "$novflip" || w0 == "$hflip" || w0 == "$nohflip")
        {
            // orientation flip callbacks not implemented in Phase 1; accept-and-ignore.
        }
        else if (w0 == "$linedash") ds.dashes = parseDashList(tok.getRest(1));
        else if (w0 == "$arrow")
        {
            std::string a = toLower(tok.getRest(1));
            if (a.empty()) ds.arrowType = ArrowType::End;
            else if (startsWithCI(a, "start")) ds.arrowType = ArrowType::Start;
            else if (startsWithCI(a, "both")) ds.arrowType = ArrowType::Both;
            else ds.arrowType = ArrowType::End;
        }
        else if (w0 == "$def_style")
        {
            if (tok.size() >= 2)
            {
                std::string name = tok.get(1);
                std::string rest = tok.size() > 2 ? tok.getRest(2) : std::string();
                defStyle(name, rest);
            }
        }
        else if (w0 == "$style")
        {
            applyStyle(ds, tok.get(1));
        }
        else if (w0 == "$hide") ds.isVisible = false;
        else if (w0 == "$name" || w0 == "$title" || w0 == "$pixelsize")
        {
            // Documented as dead/no-op (or calibration-only, Phase 2) in giv itself.
        }
        // unrecognized directives are silently accepted for compatibility.
        return;
    }

    switch (ch)
    {
    case 'm':
    case 'M':
        if (tok.size() == 3) pushPoint(tok.getFloat(1), tok.getFloat(2), Op::Move);
        return;
    case 'z':
    case 'Z':
        pushPoint(0, 0, Op::ClosePath);
        return;
    case 'e':
    case 'E':
        if (tok.size() == 6)
        {
            double x = tok.getFloat(1), y = tok.getFloat(2);
            double xw = tok.getFloat(3), yh = tok.getFloat(4), angle = tok.getFloat(5);
            pushPoint(x, y, Op::Ellipse);
            pushPoint(xw, yh, Op::Cont);
            pushPoint(angle, angle, Op::Cont);
        }
        return;
    case 'c':
    case 'C':
        if (tok.size() == 7)
        {
            pushPoint(tok.getFloat(1), tok.getFloat(2), Op::Curve);
            pushPoint(tok.getFloat(3), tok.getFloat(4), Op::Cont);
            pushPoint(tok.getFloat(5), tok.getFloat(6), Op::Cont);
        }
        return;
    case 'r':
    case 'R':
        if (tok.size() == 5)
        {
            double p0x = 0, p0y = 0;
            if (ds.pointCount() > 0)
            {
                p0x = ds.x.back();
                p0y = ds.y.back();
            }
            double p1x = tok.getFloat(1), p1y = tok.getFloat(2);
            double p2x = tok.getFloat(3), p2y = tok.getFloat(4);
            double q1x = p0x + (p1x - p0x) * 2.0 / 3.0;
            double q1y = p0y + (p1y - p0y) * 2.0 / 3.0;
            double q2x = p2x + (p1x - p2x) * 2.0 / 3.0;
            double q2y = p2y + (p1y - p2y) * 2.0 / 3.0;
            pushPoint(q1x, q1y, Op::Curve);
            pushPoint(q2x, q2y, Op::Cont);
            pushPoint(p2x, p2y, Op::Cont);
        }
        return;
    case 'q':
    case 'Q':
        if (tok.size() == 3) pushPoint(tok.getFloat(1), tok.getFloat(2), Op::Quiver);
        return;
    case 't':
    case 'T':
    {
        int align = 1;
        if (len > 1 && line[1] >= '1' && line[1] <= '9') align = line[1] - '0';
        if (tok.size() >= 3)
        {
            double x = tok.getFloat(1), y = tok.getFloat(2);
            std::string text = tok.size() > 3 ? tok.getRest(3) : std::string();
            // giv escapes literal "\n" sequences as real newlines.
            std::string out;
            out.reserve(text.size());
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n')
                {
                    out.push_back('\n');
                    ++i;
                }
                else
                {
                    out.push_back(text[i]);
                }
            }
            ds.texts.push_back(TextItem{ds.pointCount(), align, out});
            pushPoint(x, y, Op::Text);

            // giv's own bbox computation ignores text entirely (relying on
            // incidental coverage from other drawn marks/lines in the
            // scene), which is why real giv can still clip labels that
            // don't happen to be covered - but this looks much more
            // reliably correct, and giv-compatible output isn't harmed by
            // being more generous here. Estimate the label's screen-space
            // footprint from its (approximate) glyph metrics and expand the
            // bbox so auto-fit doesn't clip large/edge-positioned text
            // (e.g. the 50pt "GIV" logo in example.giv).
            {
                double maxLineChars = 0, curLineChars = 0;
                int lineCount = 1;
                for (char c : out)
                {
                    if (c == '\n') { lineCount++; curLineChars = 0; }
                    else { curLineChars += 1; if (curLineChars > maxLineChars) maxLineChars = curLineChars; }
                }
                // Rough average glyph width for proportional fonts is
                // ~0.55x the em size; line height (ascent+descent+leading)
                // is ~1.2x. Not exact glyph metrics, but close enough to
                // avoid clipping without needing to query the actual font.
                double effSize = effectiveTextSize(ds.fontName, ds.textSize);
                double textW = maxLineChars * effSize * 0.55;
                double textH = lineCount * effSize * 1.2;

                int col = (align - 1) % 3; // 0=left,1=center,2=right
                int row = (align - 1) / 3; // 0=bottom,1=center,2=top
                double x0 = (col == 0) ? 0.0 : (col == 1) ? -textW * 0.5 : -textW;
                double x1 = (col == 0) ? textW : (col == 1) ? textW * 0.5 : 0.0;
                // SceneBuilder renders with y negated (see its "giv uses
                // image-style Y-down coordinates" comment), so in the
                // un-negated parser-space y used here, bottom-aligned text
                // extends toward smaller y and top-aligned text toward
                // larger y.
                double y0 = (row == 0) ? -textH : (row == 1) ? -textH * 0.5 : 0.0;
                double y1 = (row == 0) ? 0.0 : (row == 1) ? textH * 0.5 : textH;
                scene.updateBoundsRect(x, y, x0, x1, y0, y1);
            }
        }
        return;
    }
    default:
        // bare "x y" that fell through the fast path (e.g. leading '+')
        if (tok.size() == 2)
            pushPoint(tok.getFloat(0), tok.getFloat(1), ds.pointCount() == 0 ? Op::Move : Op::Draw);
        return;
    }
}

bool GivParser::parseFile(const std::string& filename, SceneData& scene, std::string& error)
{
    MappedFile file;
    if (!file.load(filename))
    {
        error = "could not open file: " + filename;
        return false;
    }

    const char* data = file.data;
    size_t size = file.size;
    size_t pos = 0;

    bool needNewDataset = true;
    Dataset* current = nullptr;

    while (pos < size)
    {
        size_t lineStart = pos;
        size_t nl = lineStart;
        while (nl < size && data[nl] != '\n') ++nl;
        size_t lineEnd = nl; // exclusive, before '\n'
        if (lineEnd > lineStart && data[lineEnd - 1] == '\r') --lineEnd;

        size_t len = lineEnd - lineStart;

        if (len == 0)
        {
            needNewDataset = true;
        }
        else
        {
            if (needNewDataset || current == nullptr)
            {
                scene.datasets.emplace_back();
                current = &scene.datasets.back();
                current->color = kDefaultColors[scene.datasets.size() % 6];
                needNewDataset = false;
            }
            parseLine(*current, data + lineStart, len, scene);
        }

        pos = (nl < size) ? nl + 1 : nl;
    }

    // Drop a trailing empty dataset (matches giv dropping datasets with no points).
    if (!scene.datasets.empty())
    {
        Dataset& last = scene.datasets.back();
        if (last.pointCount() == 0 && last.texts.empty())
            scene.datasets.pop_back();
    }

    return true;
}

} // namespace giv
