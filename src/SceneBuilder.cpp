#include "SceneBuilder.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace giv
{

void ArrowVertexAnimator::update(float worldPerPixel)
{
    if (indices.empty() || !posArray) return;
    if (std::abs(worldPerPixel - lastWorldPerPixel_) < 1e-9f) return;
    lastWorldPerPixel_ = worldPerPixel;

    for (size_t k = 0; k < indices.size(); ++k)
    {
        uint32_t idx = indices[k];
        posArray->set(idx, tip[k] + offsetPixels[k] * worldPerPixel);
    }
    posArray->dirty();
}

void PixelSizeAnimator::update(float worldPerPixel)
{
    if (indices.empty() || !array) return;
    if (std::abs(worldPerPixel - lastWorldPerPixel_) < 1e-9f) return;
    lastWorldPerPixel_ = worldPerPixel;

    for (size_t k = 0; k < indices.size(); ++k)
    {
        uint32_t idx = indices[k];
        vsg::vec4 v = array->at(idx);
        v[component] = pixelSize[k] * worldPerPixel;
        array->set(idx, v);
    }
    array->dirty();
}

namespace
{

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------

vsg::vec4 toVsg(const Color& c)
{
    return vsg::vec4(c.r, c.g, c.b, c.isNone ? 0.0f : c.a);
}

// Stable (process-to-process) FNV-1a hash used to name on-disk font-atlas
// cache files - deliberately not std::hash<std::string>, whose output isn't
// guaranteed stable across runs.
uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Directory for cached, pre-rasterized vsg::Font atlases (see resolveFont).
std::string fontCacheDir()
{
    const char* xdgCache = std::getenv("XDG_CACHE_HOME");
    if (xdgCache && *xdgCache) return std::string(xdgCache) + "/wiv/fonts";
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.cache/wiv/fonts";
    return "/tmp/wiv-font-cache";
}

vsg::ref_ptr<vsg::ShaderStage> loadShader(VkShaderStageFlagBits stage, const std::string& dir, const std::string& file)
{
    auto shader = vsg::ShaderStage::read(stage, "main", dir + "/" + file);
    if (!shader)
        throw std::runtime_error("failed to load shader: " + dir + "/" + file);
    return shader;
}

// Builds a StateGroup holding a graphics pipeline with the given vertex
// bindings/attributes, no descriptor sets, and the standard VSG-provided
// {projection, modelview} push-constant matrices. No depth test (flat 2D
// scene), no back-face culling (triangle winding isn't guaranteed by our
// simple fan triangulation), alpha blending enabled.
vsg::ref_ptr<vsg::StateGroup> makePipeline(const std::string& shaderDir,
                                            const std::string& vertFile,
                                            const std::string& fragFile,
                                            const vsg::VertexInputState::Bindings& bindings,
                                            const vsg::VertexInputState::Attributes& attributes,
                                            bool blend = true)
{
    auto vertexShader = loadShader(VK_SHADER_STAGE_VERTEX_BIT, shaderDir, vertFile);
    auto fragmentShader = loadShader(VK_SHADER_STAGE_FRAGMENT_BIT, shaderDir, fragFile);

    vsg::PushConstantRanges pushConstantRanges{
        {VK_SHADER_STAGE_VERTEX_BIT, 0, 128} // projection + modelview, auto-supplied by RecordTraversal
    };

    auto rasterization = vsg::RasterizationState::create();
    rasterization->cullMode = VK_CULL_MODE_NONE;

    auto depthStencil = vsg::DepthStencilState::create();
    depthStencil->depthTestEnable = VK_FALSE;
    depthStencil->depthWriteEnable = VK_FALSE;

    // The label-picking pass (blend == false) writes exact, unblended id
    // colors - any blending there would let a discard-free but partially
    // transparent fragment mix with whatever was already in the attachment,
    // corrupting the id readback (see marks_label.frag).
    auto colorBlend = vsg::ColorBlendState::create();
    colorBlend->configureAttachments(blend);

    vsg::GraphicsPipelineStates pipelineStates{
        vsg::VertexInputState::create(bindings, attributes),
        vsg::InputAssemblyState::create(),
        rasterization,
        vsg::MultisampleState::create(),
        colorBlend,
        depthStencil};

    auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{}, pushConstantRanges);
    auto graphicsPipeline = vsg::GraphicsPipeline::create(pipelineLayout, vsg::ShaderStages{vertexShader, fragmentShader}, pipelineStates);

    auto stateGroup = vsg::StateGroup::create();
    stateGroup->add(vsg::BindGraphicsPipeline::create(graphicsPipeline));
    return stateGroup;
}

// ---------------------------------------------------------------------
// Per-batch instance/vertex records (match the shader vertex layouts)
// ---------------------------------------------------------------------

struct MarkInstance
{
    vsg::vec4 posSizeMode; // xy = position, z = half size, w = mark mode
    vsg::vec4 color;
    vsg::vec4 labelColor; // this instance's dataset, encoded for label picking - see labelColorFor()
};

struct LineInstance
{
    vsg::vec4 p0p1;      // xy = p0, zw = p1
    vsg::vec4 widthDash; // x = half width, y = dashOn, z = dashOff, w = cumulative length at p0
    vsg::vec4 color;
    vsg::vec4 labelColor;
};

struct FillVertex
{
    vsg::vec2 pos;
    vsg::vec4 color;
    vsg::vec4 labelColor;
};

// Encodes a dataset index as a flat opaque color for the offscreen label-
// picking pass, matching giv's GivPainterAgg::label_to_color exactly (id+1
// packed big-endian into RGB, so an empty/background pixel - cleared to
// (0,0,0,1) - decodes to id -1): b = (id+1)&0xFF, g = ((id+1)>>8)&0xFF,
// r = ((id+1)>>16)&0xFF. LabelPicker::decode() is the inverse.
vsg::vec4 labelColorFor(size_t datasetIndex)
{
    uint32_t v = static_cast<uint32_t>(datasetIndex + 1);
    float r = static_cast<float>((v >> 16) & 0xFFu) / 255.0f;
    float g = static_cast<float>((v >> 8) & 0xFFu) / 255.0f;
    float b = static_cast<float>(v & 0xFFu) / 255.0f;
    return vsg::vec4(r, g, b, 1.0f);
}

float markModeFor(MarkType type)
{
    switch (type)
    {
    case MarkType::FCircle: return 0.0f;
    case MarkType::FSquare: return 1.0f;
    case MarkType::Circle: return 2.0f;
    case MarkType::Square: return 3.0f;
    case MarkType::Pixel: return 4.0f;
    }
    return 0.0f;
}

// Tessellates a cubic bezier (p0..p3) into `segments` line pieces, appended
// to `out` (p0 assumed already present as the current path point).
void tessellateCubic(const vsg::vec2& p0, const vsg::vec2& p1, const vsg::vec2& p2, const vsg::vec2& p3,
                      int segments, std::vector<vsg::vec2>& out)
{
    for (int i = 1; i <= segments; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(segments);
        float u = 1.0f - t;
        vsg::vec2 pt = p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
        out.push_back(pt);
    }
}

std::vector<vsg::vec2> tessellateEllipse(const vsg::vec2& center, float rx, float ry, float angleDeg, int segments)
{
    std::vector<vsg::vec2> out;
    out.reserve(segments + 1);
    float angleRad = angleDeg * kPi / 180.0f;
    float ca = std::cos(angleRad), sa = std::sin(angleRad);
    for (int i = 0; i <= segments; ++i)
    {
        float t = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(segments);
        float ex = rx * std::cos(t);
        float ey = ry * std::sin(t);
        vsg::vec2 p(center.x + ex * ca - ey * sa, center.y + ex * sa + ey * ca);
        out.push_back(p);
    }
    return out;
}

void addLineSegment(std::vector<LineInstance>& lines, const vsg::vec2& p0, const vsg::vec2& p1,
                     float halfWidth, float dashOn, float dashOff, float& cumLen, const vsg::vec4& color,
                     const vsg::vec4& labelColor)
{
    LineInstance li;
    li.p0p1 = vsg::vec4(p0.x, p0.y, p1.x, p1.y);
    li.widthDash = vsg::vec4(halfWidth, dashOn, dashOff, cumLen);
    li.color = color;
    li.labelColor = labelColor;
    lines.push_back(li);
    cumLen += vsg::length(p1 - p0);
}

// Appends a filled arrowhead at `tip`, pointing along direction `dir` (unit
// vector, direction of travel - i.e. the arrow points *along* `dir`), sized
// relative to `hw` (== the same pixel-constant `lineHalfWidth` value lines
// are built with - see ArrowVertexAnimator/PixelSizeAnimator).
//
// Shape/sizing matches giv's AGG arrowhead (giv_agg_arrowhead.cc, driven
// from GivPainterAgg::set_arrow with the default d1..d5 = 0,3,2,2,1 scaled
// by the stroke width W = 2*hw, and d5 overridden to W/2 = hw):
//   d1 = 0        (tip is flush with the path endpoint)
//   d2 = 3W = 6hw
//   d3 = 2W = 4hw (half-width of the swept-back wingtips)
//   d4 = 2W = 4hw
//   d5 = W/2 = hw (half-width of the flat tip edge)
// Local +x runs *backward* from the tip (opposite `dir`); local y is
// perpendicular. Vertices (giv's arrowhead::rewind order), plus the
// geometric tip T at the local origin used to fan-triangulate the
// concave (swallow-tail) polygon:
//   T  = (0, 0)
//   V0 = (-d1, -d5) = (0,   -hw)
//   V1 = (d2+d4, -d3) = (10hw, -4hw)
//   V2 = (d2, 0)      = (6hw,  0)
//   V3 = (d2+d4, d3)  = (10hw, 4hw)
//   V4 = (-d1, d5)    = (0,   hw)
//
// Unlike addPolygonFill, these vertices must stay pixel-constant in size as
// the view zooms (matching giv's device-pixel line/arrow sizing), so rather
// than baking final world-space positions here, each vertex is recorded as
// (tip, offset-in-pixel-units) into `arrowTip`/`arrowOffsetPixels` (parallel
// to the newly-appended `tris` entries) for ArrowVertexAnimator to resolve
// to world space whenever the view's world-per-pixel scale changes. The
// position written into `tris` here is just a same-scale-as-lines initial
// placeholder, good enough until the first update() call.
void addArrowHead(std::vector<FillVertex>& tris, std::vector<uint32_t>& arrowIndex, std::vector<vsg::vec2>& arrowTip,
                   std::vector<vsg::vec2>& arrowOffsetPixels, const vsg::vec2& tip, const vsg::vec2& dir,
                   float hw, const vsg::vec4& color, const vsg::vec4& labelColor)
{
    vsg::vec2 back(-dir.x, -dir.y);
    vsg::vec2 normal(-dir.y, dir.x);

    auto offset = [&](float lx, float ly) {
        return back * lx + normal * ly;
    };

    vsg::vec2 oT(0.0f, 0.0f);
    vsg::vec2 oV0 = offset(0.0f, -hw);
    vsg::vec2 oV1 = offset(10.0f * hw, -4.0f * hw);
    vsg::vec2 oV2 = offset(6.0f * hw, 0.0f);
    vsg::vec2 oV3 = offset(10.0f * hw, 4.0f * hw);
    vsg::vec2 oV4 = offset(0.0f, hw);

    auto tri = [&](const vsg::vec2& oa, const vsg::vec2& ob, const vsg::vec2& oc) {
        for (const vsg::vec2& o : {oa, ob, oc})
        {
            arrowIndex.push_back(static_cast<uint32_t>(tris.size()));
            tris.push_back({tip + o, color, labelColor});
            arrowTip.push_back(tip);
            arrowOffsetPixels.push_back(o);
        }
    };
    tri(oT, oV0, oV1);
    tri(oT, oV1, oV2);
    tri(oT, oV2, oV3);
    tri(oT, oV3, oV4);
    tri(oT, oV4, oV0);
}

// Fan-triangulates a (assumed simple-ish) closed polygon from vertex 0.
// Not a full earcut - concave polygons may triangulate incorrectly, noted
// as a Phase 1 simplification.
void addPolygonFill(std::vector<FillVertex>& tris, const std::vector<vsg::vec2>& poly, const vsg::vec4& color,
                     const vsg::vec4& labelColor)
{
    if (poly.size() < 3) return;
    for (size_t i = 1; i + 1 < poly.size(); ++i)
    {
        tris.push_back({poly[0], color, labelColor});
        tris.push_back({poly[i], color, labelColor});
        tris.push_back({poly[i + 1], color, labelColor});
    }
}

} // namespace

SceneBuilder::SceneBuilder(vsg::ref_ptr<vsg::Options> options) : options_(options) {}

vsg::ref_ptr<vsg::Font> SceneBuilder::resolveFont(const std::string& fontSpec, double& outSize)
{
    std::string family = fontSpec.empty() ? "Sans" : fontSpec;
    outSize = -1.0;

    // Pull a trailing numeric token off as the point size (giv/Pango style
    // font descriptions embed the size at the end, e.g. "Sans Bold 18").
    size_t lastSpace = family.find_last_of(' ');
    if (lastSpace != std::string::npos)
    {
        std::string tail = family.substr(lastSpace + 1);
        if (!tail.empty() && (std::isdigit(static_cast<unsigned char>(tail[0]))))
        {
            outSize = std::atof(tail.c_str());
            family = family.substr(0, lastSpace);
        }
    }

    // giv passes the whole spec through to Pango, which parses out
    // weight/style keywords (Bold, Italic, Oblique) embedded among the
    // family words (e.g. "Sans Bold Italic"). fontconfig's own pattern
    // syntax doesn't understand those as part of the family name - it needs
    // them as separate ":weight=...:slant=..." fields - so pull them out of
    // the remaining words here and build the equivalent fontconfig pattern
    // (see `man fontconfig-pattern` / `fc-match --help`).
    bool bold = false, italic = false, oblique = false;
    std::vector<std::string> familyWords;
    {
        std::istringstream iss(family);
        std::string word;
        while (iss >> word)
        {
            std::string lower = word;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "bold") bold = true;
            else if (lower == "italic") italic = true;
            else if (lower == "oblique") oblique = true;
            else familyWords.push_back(word);
        }
    }
    family.clear();
    for (size_t i = 0; i < familyWords.size(); ++i)
    {
        if (i) family += ' ';
        family += familyWords[i];
    }
    if (family.empty()) family = "Sans";

    std::string cacheKey = family;
    if (bold) cacheKey += ":bold";
    if (italic) cacheKey += ":italic";
    else if (oblique) cacheKey += ":oblique";

    auto cached = fontCache_.find(cacheKey);
    if (cached != fontCache_.end()) return cached->second;

    std::string pattern = family;
    if (bold) pattern += ":weight=bold";
    if (italic) pattern += ":slant=italic";
    else if (oblique) pattern += ":slant=oblique";

    // Resolve an actual font file via fontconfig (Linux). Falls back to
    // whatever vsgXchange's default search turns up if fc-match is absent.
    std::string path;
    std::string cmd = "fc-match -f '%{file}' \"" + pattern + "\" 2>/dev/null";
    if (FILE* p = popen(cmd.c_str(), "r"))
    {
        char buf[1024] = {0};
        if (fgets(buf, sizeof(buf), p)) path = buf;
        pclose(p);
    }

    vsg::ref_ptr<vsg::Font> font;
    if (!path.empty())
    {
        // vsgXchange's freetype reader rasterizes *every* glyph in the font
        // file into the atlas (it walks the whole charmap via
        // FT_Get_First_Char/FT_Get_Next_Char, sized off face->num_glyphs),
        // not just the glyphs a given label actually uses. For a large
        // Unicode-coverage font like Noto Sans that's thousands of glyphs
        // and costs ~1-2 seconds - independent of, and much larger than,
        // the fc-match lookup above. fontCache_ already avoids paying that
        // cost twice *within* one run for the same font spec, but every
        // fresh wiv process pays it again. The built vsg::Font is a plain
        // data object (atlas image + glyph metrics) that round-trips
        // losslessly through VSG's native binary (.vsgb) format, so persist
        // it on disk, keyed by the resolved font file's path+mtime, and
        // load that on subsequent runs instead of re-rasterizing.
        std::error_code ec;
        std::string trimmedPath = path;
        while (!trimmedPath.empty() && (trimmedPath.back() == '\n' || trimmedPath.back() == '\r'))
            trimmedPath.pop_back();

        std::string cacheDir = fontCacheDir();
        std::filesystem::create_directories(cacheDir, ec);

        auto mtime = std::filesystem::last_write_time(trimmedPath, ec);
        std::ostringstream keyStream;
        keyStream << trimmedPath << '|' << (ec ? int64_t{0} : mtime.time_since_epoch().count());
        std::string cacheFile = cacheDir + "/" + std::to_string(fnv1a(keyStream.str())) + ".vsgb";

        if (std::filesystem::exists(cacheFile, ec))
            font = vsg::read_cast<vsg::Font>(cacheFile, options_);

        if (!font)
        {
            font = vsg::read_cast<vsg::Font>(path, options_);
            if (font)
            {
                std::error_code writeEc;
                try
                {
                    vsg::write(font, cacheFile, options_);
                }
                catch (...)
                {
                    // Non-fatal - just means this run doesn't get to prime
                    // the cache (e.g. unwritable cache dir); font loading
                    // itself already succeeded above.
                }
            }
        }
    }

    fontCache_[cacheKey] = font; // cache even nullptr, so we don't keep retrying
    return font;
}

vsg::ref_ptr<vsg::Group> SceneBuilder::build(const SceneData& scene, const std::string& shaderDir)
{
    auto root = vsg::Group::create();

    std::vector<MarkInstance> marks;
    std::vector<bool> markScalesWithZoom; // parallel to `marks`; see PixelSizeAnimator
    std::vector<LineInstance> lines;
    std::vector<FillVertex> fillTris;
    // Indices into `fillTris` of just the arrowhead vertices (polygon-fill
    // vertices are not pixel-constant and have no entry here), with
    // `arrowTip`/`arrowOffsetPixels` parallel to `arrowIndex`; see
    // addArrowHead's doc comment and ArrowVertexAnimator.
    std::vector<uint32_t> arrowIndex;
    std::vector<vsg::vec2> arrowTip;
    std::vector<vsg::vec2> arrowOffsetPixels;

    // Text nodes are created directly (not batched); typical giv scenes have
    // at most thousands of labels, not millions.
    auto textGroup = vsg::Group::create();

    // vsg::Text::setup() calls vsg::createTextShaderSet(options_) whenever
    // text->shaderSet isn't already set, and that function only consults
    // options_->shaderSets["text"] as a cache - if it's empty (the default),
    // every single text item re-deserializes the embedded text shader (a
    // binary VSG blob with several GLSL stages) from scratch instead of
    // reusing one instance. That's cheap next to the font-atlas cost fixed
    // in resolveFont() below, but it's still wasted, redundant work per
    // label, so avoid it too: populate the cache once up front so every
    // Text::setup() call below hits it.
    if (options_ && options_->shaderSets.find("text") == options_->shaderSets.end())
        options_->shaderSets["text"] = vsg::createTextShaderSet(options_);

    for (size_t dsIdx = 0; dsIdx < scene.datasets.size(); ++dsIdx)
    {
        const Dataset& ds = scene.datasets[dsIdx];
        if (!ds.isVisible) continue;

        // Every mark/line/fill vertex emitted for this dataset also carries
        // this flat id color alongside its real color, so the parallel
        // label batches built after the main ones (below) can reuse these
        // same geometry-building code paths - see labelColorFor().
        const vsg::vec4 labelColor = labelColorFor(dsIdx);

        const vsg::vec4 lineColor = toVsg(ds.color);
        const vsg::vec4 markColor = toVsg(ds.color);
        const vsg::vec4 fillColor = toVsg(ds.color);
        const vsg::vec4 outlineColor = toVsg(ds.outlineColor);
        const vsg::vec4 quiverColor = toVsg(ds.quiverColor);

        float markHalfSize = static_cast<float>(std::max(0.5, ds.markSize * 0.5));
        if (ds.markType == MarkType::Pixel) markHalfSize = std::min(markHalfSize, 0.5f);
        float markMode = markModeFor(ds.markType);

        float lineHalfWidth = static_cast<float>(std::max(0.1, ds.lineWidth * 0.5));
        float dashOn = 1e9f, dashOff = 0.0f;
        if (ds.dashes.size() >= 2)
        {
            dashOn = ds.dashes[0];
            dashOff = ds.dashes[1];
        }

        const bool needSubpaths = ds.doDrawLines || ds.doDrawPolygon || ds.arrowType != ArrowType::None;

        std::vector<std::vector<vsg::vec2>> subpaths;
        // Parallel to `subpaths`: true if the subpath was explicitly closed
        // (giv `z` / Op::ClosePath), in which case `subpaths[i]` has a
        // synthetic duplicate of its first point appended as the last
        // point (see Op::ClosePath below) purely so the line/outline
        // geometry draws the closing edge. That duplicate must NOT be
        // treated as the "real" last vertex when placing an end-arrow -
        // giv's marker generator places the end arrowhead at the actual
        // last emitted vertex (using the tangent of the real last segment),
        // ignoring the synthetic closing segment. See the arrow placement
        // code below, which uses this flag to look one point further back.
        std::vector<bool> subpathClosed;
        std::vector<vsg::vec2> cur;
        vsg::vec2 currentPoint(0.0f, 0.0f);

        const size_t n = ds.pointCount();
        size_t textIdx = 0;

        for (size_t i = 0; i < n; ++i)
        {
            const Op op = ds.op[i];
            // giv uses image-style Y-down coordinates; negate Y here so our
            // renderer's standard math-convention (Y-up) camera shows the
            // scene the same way giv does. Ellipse radii/angle (read
            // separately below, not via `p`) are sizes/angles, not
            // positions, and are intentionally left un-negated.
            const vsg::vec2 p(ds.x[i], -ds.y[i]);

            switch (op)
            {
            case Op::Move:
                if (ds.doDrawMarks)
                {
                    marks.push_back({vsg::vec4(p.x, p.y, markHalfSize, markMode), markColor, labelColor});
                    markScalesWithZoom.push_back(ds.doScaleMarks);
                }
                if (needSubpaths)
                {
                    if (!cur.empty())
                    {
                        subpaths.push_back(cur);
                        subpathClosed.push_back(false);
                    }
                    cur.clear();
                    cur.push_back(p);
                }
                currentPoint = p;
                break;

            case Op::Draw:
                if (ds.doDrawMarks)
                {
                    marks.push_back({vsg::vec4(p.x, p.y, markHalfSize, markMode), markColor, labelColor});
                    markScalesWithZoom.push_back(ds.doScaleMarks);
                }
                if (needSubpaths)
                {
                    if (cur.empty()) cur.push_back(p);
                    else cur.push_back(p);
                }
                currentPoint = p;
                break;

            case Op::ClosePath:
                if (needSubpaths && !cur.empty())
                {
                    cur.push_back(cur.front());
                    subpaths.push_back(cur);
                    subpathClosed.push_back(true);
                    cur.clear();
                }
                break;

            case Op::Curve:
            {
                vsg::vec2 c1 = p;
                vsg::vec2 c2 = (i + 1 < n) ? vsg::vec2(ds.x[i + 1], -ds.y[i + 1]) : c1;
                vsg::vec2 end = (i + 2 < n) ? vsg::vec2(ds.x[i + 2], -ds.y[i + 2]) : c2;
                if (needSubpaths)
                {
                    vsg::vec2 start = cur.empty() ? currentPoint : cur.back();
                    if (cur.empty()) cur.push_back(start);
                    tessellateCubic(start, c1, c2, end, 16, cur);
                }
                currentPoint = end;
                i += 2; // consumed two Cont points
                break;
            }

            case Op::Cont:
                // Orphan continuation point (shouldn't normally occur - Curve
                // consumes its own Cont points); ignore.
                break;

            case Op::Ellipse:
            {
                vsg::vec2 center = p;
                vsg::vec2 wh = (i + 1 < n) ? vsg::vec2(ds.x[i + 1], ds.y[i + 1]) : vsg::vec2(1, 1);
                float angle = (i + 2 < n) ? ds.x[i + 2] : 0.0f;
                if (needSubpaths)
                {
                    if (!cur.empty())
                    {
                        subpaths.push_back(cur);
                        subpathClosed.push_back(false);
                        cur.clear();
                    }
                    subpaths.push_back(tessellateEllipse(center, wh.x, wh.y, angle, 48));
                    subpathClosed.push_back(true); // tessellated ellipse loop has no "real" last point to special-case
                }
                currentPoint = center;
                i += 2;
                break;
            }

            case Op::Quiver:
            {
                vsg::vec2 tip = currentPoint + p * static_cast<float>(ds.quiverScale);
                float cum = 0.0f;
                addLineSegment(lines, currentPoint, tip, lineHalfWidth, 1e9f, 0.0f, cum, quiverColor, labelColor);
                if (ds.quiverHead)
                {
                    vsg::vec2 dir = tip - currentPoint;
                    float len = vsg::length(dir);
                    if (len > 1e-6f) addArrowHead(fillTris, arrowIndex, arrowTip, arrowOffsetPixels, tip, dir / len, lineHalfWidth, quiverColor, labelColor);
                }
                break;
            }

            case Op::Text:
                if (textIdx < ds.texts.size() && ds.texts[textIdx].pointIndex == i)
                {
                    const TextItem& item = ds.texts[textIdx];
                    ++textIdx;

                    double size = ds.textSize;
                    auto font = resolveFont(ds.fontName, size);
                    if (size <= 0) size = ds.textSize > 0 ? ds.textSize : 12.0;

                    auto layout = vsg::StandardLayout::create();
                    int align = item.align;
                    int col = (align - 1) % 3; // 0=left,1=center,2=right
                    int row = (align - 1) / 3; // 0=bottom,1=center,2=top
                    layout->horizontalAlignment = (col == 0) ? vsg::StandardLayout::LEFT_ALIGNMENT
                                                              : (col == 1) ? vsg::StandardLayout::CENTER_ALIGNMENT
                                                                            : vsg::StandardLayout::RIGHT_ALIGNMENT;
                    layout->verticalAlignment = (row == 0) ? vsg::StandardLayout::BOTTOM_ALIGNMENT
                                                            : (row == 1) ? vsg::StandardLayout::CENTER_ALIGNMENT
                                                                          : vsg::StandardLayout::TOP_ALIGNMENT;
                    layout->position = vsg::vec3(p.x, p.y, 0.0f);
                    float fsize = static_cast<float>(size);
                    layout->horizontal = vsg::vec3(fsize, 0.0f, 0.0f);
                    layout->vertical = vsg::vec3(0.0f, fsize, 0.0f);
                    layout->color = markColor;

                    if (font)
                    {
                        auto text = vsg::Text::create();
                        text->text = vsg::stringValue::create(item.text);
                        text->font = font;
                        text->layout = layout;
                        text->setup(0, options_);
                        textGroup->addChild(text);
                    }
                }
                break;
            }
        }

        if (needSubpaths && !cur.empty())
        {
            subpaths.push_back(cur);
            subpathClosed.push_back(false);
        }

        if (needSubpaths)
        {
            for (size_t spIdx = 0; spIdx < subpaths.size(); ++spIdx)
            {
                const auto& sp = subpaths[spIdx];
                const bool spClosed = subpathClosed[spIdx];

                if (ds.doDrawPolygon) addPolygonFill(fillTris, sp, fillColor, labelColor);
                if (ds.doDrawPolygonOutline && ds.doDrawPolygon)
                {
                    float cum = 0.0f;
                    for (size_t i = 0; i + 1 < sp.size(); ++i)
                        addLineSegment(lines, sp[i], sp[i + 1], lineHalfWidth, dashOn, dashOff, cum, outlineColor, labelColor);
                }
                else if (ds.doDrawLines)
                {
                    float cum = 0.0f;
                    for (size_t i = 0; i + 1 < sp.size(); ++i)
                        addLineSegment(lines, sp[i], sp[i + 1], lineHalfWidth, dashOn, dashOff, cum, lineColor, labelColor);
                }

                if (ds.doDrawLines && ds.arrowType != ArrowType::None && sp.size() >= 2)
                {
                    if (ds.arrowType == ArrowType::End || ds.arrowType == ArrowType::Both)
                    {
                        // For a closed subpath, sp.back() is a synthetic
                        // duplicate of sp.front() (added purely to draw the
                        // closing edge - see subpathClosed's doc comment
                        // above) rather than a real path vertex. giv places
                        // the end-arrow at the actual last emitted vertex
                        // using the tangent of the real last segment, so
                        // skip past that duplicate here to match.
                        const bool haveTriple = spClosed ? sp.size() >= 3 : sp.size() >= 2;
                        if (haveTriple)
                        {
                            size_t tipIdx = spClosed ? sp.size() - 2 : sp.size() - 1;
                            size_t prevIdx = tipIdx - 1;
                            vsg::vec2 tip = sp[tipIdx];
                            vsg::vec2 dir = tip - sp[prevIdx];
                            float len = vsg::length(dir);
                            if (len > 1e-6f) addArrowHead(fillTris, arrowIndex, arrowTip, arrowOffsetPixels, tip, dir / len, lineHalfWidth, lineColor, labelColor);
                        }
                    }
                    if (ds.arrowType == ArrowType::Start || ds.arrowType == ArrowType::Both)
                    {
                        vsg::vec2 dir = sp.front() - sp[1];
                        float len = vsg::length(dir);
                        if (len > 1e-6f) addArrowHead(fillTris, arrowIndex, arrowTip, arrowOffsetPixels, sp.front(), dir / len, lineHalfWidth, lineColor, labelColor);
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------
    // Shared unit-quad base geometry, reused by both marks and lines
    // -------------------------------------------------------------
    auto quadCorners = vsg::vec2Array::create({{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}});
    auto quadIndices = vsg::ushortArray::create({0, 1, 2, 2, 3, 0});

    // labelRoot mirrors root's marks/lines/fill batches geometrically
    // (reusing the very same position/shape buffers built below), but is
    // painted with each vertex's labelColor instead of its color, using
    // hard-edged (no blend, no AA) pipelines - see labelGraph()'s doc
    // comment in SceneBuilder.h and marks_label.frag.
    auto labelRoot = vsg::Group::create();

    // -------------------------------------------------------------
    // Marks batch
    // -------------------------------------------------------------
    if (!marks.empty())
    {
        vsg::VertexInputState::Bindings bindings{
            VkVertexInputBindingDescription{0, sizeof(vsg::vec2), VK_VERTEX_INPUT_RATE_VERTEX},
            VkVertexInputBindingDescription{1, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_INSTANCE},
            VkVertexInputBindingDescription{2, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_INSTANCE}};
        // NOTE: each attribute below is backed by its own tightly-packed
        // vec4Array (not one interleaved MarkInstance buffer), so binding
        // stride is sizeof(vec4) and attribute offset is always 0.
        vsg::VertexInputState::Attributes attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
            VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            VkVertexInputAttributeDescription{2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};

        auto stateGroup = makePipeline(shaderDir, "marks.vert.spv", "marks.frag.spv", bindings, attributes);

        auto posSizeModeArray = vsg::vec4Array::create(static_cast<uint32_t>(marks.size()));
        // Marked dynamic: fixed-pixel-size marks (do_scale_marks == false,
        // giv's default) have their z (half-size) component rewritten every
        // time the view zoom changes - see PixelSizeAnimator::update() and
        // its call site in main.cpp's render loop.
        posSizeModeArray->properties.dataVariance = vsg::DYNAMIC_DATA;
        auto colorArray = vsg::vec4Array::create(static_cast<uint32_t>(marks.size()));
        auto labelColorArray = vsg::vec4Array::create(static_cast<uint32_t>(marks.size()));
        for (size_t i = 0; i < marks.size(); ++i)
        {
            posSizeModeArray->set(i, marks[i].posSizeMode);
            colorArray->set(i, marks[i].color);
            labelColorArray->set(i, marks[i].labelColor);
        }

        markSizeAnimator_ = PixelSizeAnimator::create();
        markSizeAnimator_->array = posSizeModeArray;
        markSizeAnimator_->component = 2; // z = half size
        for (size_t i = 0; i < marks.size(); ++i)
        {
            if (!markScalesWithZoom[i])
            {
                markSizeAnimator_->indices.push_back(static_cast<uint32_t>(i));
                // marks[i].posSizeMode.z currently holds ds.markSize * 0.5
                // interpreted as a *pixel* half-size (giv's do_scale_marks
                // == false semantics); PixelSizeAnimator converts this to
                // world units on the first update() call.
                markSizeAnimator_->pixelSize.push_back(marks[i].posSizeMode.z);
            }
        }

        auto drawCommands = vsg::Commands::create();
        drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, posSizeModeArray, colorArray}));
        drawCommands->addChild(vsg::BindIndexBuffer::create(quadIndices));
        drawCommands->addChild(vsg::DrawIndexed::create(6, static_cast<uint32_t>(marks.size()), 0, 0, 0));

        stateGroup->addChild(drawCommands);
        root->addChild(stateGroup);

        // Label variant: same quad/position/mode buffers (marks_label.frag
        // reads fragMode/fragCorner exactly like marks.frag), labelColor in
        // place of color, hard edges, no blending.
        auto labelStateGroup = makePipeline(shaderDir, "marks.vert.spv", "marks_label.frag.spv", bindings, attributes, /*blend=*/false);
        auto labelDrawCommands = vsg::Commands::create();
        labelDrawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, posSizeModeArray, labelColorArray}));
        labelDrawCommands->addChild(vsg::BindIndexBuffer::create(quadIndices));
        labelDrawCommands->addChild(vsg::DrawIndexed::create(6, static_cast<uint32_t>(marks.size()), 0, 0, 0));
        labelStateGroup->addChild(labelDrawCommands);
        labelRoot->addChild(labelStateGroup);
    }

    // -------------------------------------------------------------
    // Lines batch
    // -------------------------------------------------------------
    if (!lines.empty())
    {
        vsg::VertexInputState::Bindings bindings{
            VkVertexInputBindingDescription{0, sizeof(vsg::vec2), VK_VERTEX_INPUT_RATE_VERTEX},
            VkVertexInputBindingDescription{1, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_INSTANCE},
            VkVertexInputBindingDescription{2, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_INSTANCE},
            VkVertexInputBindingDescription{3, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_INSTANCE}};
        // Each attribute has its own tightly-packed vec4Array; see note above.
        vsg::VertexInputState::Attributes attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
            VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            VkVertexInputAttributeDescription{2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            VkVertexInputAttributeDescription{3, 3, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};

        auto stateGroup = makePipeline(shaderDir, "lines.vert.spv", "lines.frag.spv", bindings, attributes);

        auto p0p1Array = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        auto widthDashArray = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        // giv always draws line/outline/quiver-shaft width in constant
        // device pixels regardless of zoom (GivPainterCairo::set_line_width
        // sets cairo's line width directly with no CTM scale applied, and
        // there's no $scale_marks-equivalent toggle for line width - see
        // giv-data.cc default_line_width). lines.vert expands half-width in
        // world space, so keep it dynamic and recompute every zoom change.
        widthDashArray->properties.dataVariance = vsg::DYNAMIC_DATA;
        auto colorArray = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        auto labelColorArray = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        for (size_t i = 0; i < lines.size(); ++i)
        {
            p0p1Array->set(i, lines[i].p0p1);
            widthDashArray->set(i, lines[i].widthDash);
            colorArray->set(i, lines[i].color);
            labelColorArray->set(i, lines[i].labelColor);
        }

        lineWidthAnimator_ = PixelSizeAnimator::create();
        lineWidthAnimator_->array = widthDashArray;
        lineWidthAnimator_->component = 0; // x = half width
        for (size_t i = 0; i < lines.size(); ++i)
        {
            lineWidthAnimator_->indices.push_back(static_cast<uint32_t>(i));
            // lines[i].widthDash.x currently holds lineHalfWidth interpreted
            // as a *pixel* half-width; PixelSizeAnimator converts it to
            // world units on the first update() call.
            lineWidthAnimator_->pixelSize.push_back(lines[i].widthDash.x);
        }

        // Label-pass copy of widthDashArray, but with the half-width
        // widened to a 1.5px floor (3px total) - matches giv's own
        // do_paint_by_index picking pass (GivPainterAgg::set_line_width /
        // GivPainterCairo::set_line_width: `if (do_paint_by_index &&
        // line_width < 3) line_width = 3;`), which gives thin lines a
        // bigger hit area to hover without changing how thick they actually
        // look on screen. giv applies no equivalent widening to marks.
        auto labelWidthDashArray = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        labelWidthDashArray->properties.dataVariance = vsg::DYNAMIC_DATA;
        for (size_t i = 0; i < lines.size(); ++i) labelWidthDashArray->set(i, lines[i].widthDash);

        labelLineWidthAnimator_ = PixelSizeAnimator::create();
        labelLineWidthAnimator_->array = labelWidthDashArray;
        labelLineWidthAnimator_->component = 0;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            labelLineWidthAnimator_->indices.push_back(static_cast<uint32_t>(i));
            labelLineWidthAnimator_->pixelSize.push_back(std::max(lines[i].widthDash.x, 1.5f));
        }

        auto drawCommands = vsg::Commands::create();
        drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, p0p1Array, widthDashArray, colorArray}));
        drawCommands->addChild(vsg::BindIndexBuffer::create(quadIndices));
        drawCommands->addChild(vsg::DrawIndexed::create(6, static_cast<uint32_t>(lines.size()), 0, 0, 0));

        stateGroup->addChild(drawCommands);
        root->addChild(stateGroup);

        // Label variant: lines.frag already has no AA (only a hard dash
        // discard), so it's reused as-is; just swap in labelColor, the
        // widened labelWidthDashArray (see above), and disable blending.
        auto labelStateGroup = makePipeline(shaderDir, "lines.vert.spv", "lines.frag.spv", bindings, attributes, /*blend=*/false);
        auto labelDrawCommands = vsg::Commands::create();
        labelDrawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, p0p1Array, labelWidthDashArray, labelColorArray}));
        labelDrawCommands->addChild(vsg::BindIndexBuffer::create(quadIndices));
        labelDrawCommands->addChild(vsg::DrawIndexed::create(6, static_cast<uint32_t>(lines.size()), 0, 0, 0));
        labelStateGroup->addChild(labelDrawCommands);
        labelRoot->addChild(labelStateGroup);
    }

    // -------------------------------------------------------------
    // Fill (polygons + arrow/quiver heads) batch
    // -------------------------------------------------------------
    if (!fillTris.empty())
    {
        vsg::VertexInputState::Bindings bindings{
            VkVertexInputBindingDescription{0, sizeof(vsg::vec2), VK_VERTEX_INPUT_RATE_VERTEX},
            VkVertexInputBindingDescription{1, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_VERTEX}};
        // posArray/colorArray are separate tightly-packed buffers; see note above.
        vsg::VertexInputState::Attributes attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
            VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};

        auto stateGroup = makePipeline(shaderDir, "fill.vert.spv", "fill.frag.spv", bindings, attributes);

        auto posArray = vsg::vec2Array::create(static_cast<uint32_t>(fillTris.size()));
        auto colorArray = vsg::vec4Array::create(static_cast<uint32_t>(fillTris.size()));
        auto labelColorArray = vsg::vec4Array::create(static_cast<uint32_t>(fillTris.size()));
        for (size_t i = 0; i < fillTris.size(); ++i)
        {
            posArray->set(i, fillTris[i].pos);
            colorArray->set(i, fillTris[i].color);
            labelColorArray->set(i, fillTris[i].labelColor);
        }
        // Arrowhead vertices must stay pixel-constant in size as the view
        // zooms (see addArrowHead's doc comment) - mark the position buffer
        // dynamic and hand ArrowVertexAnimator what it needs to rewrite
        // just those vertices whenever the view's world-per-pixel changes.
        if (!arrowIndex.empty())
        {
            posArray->properties.dataVariance = vsg::DYNAMIC_DATA;
            arrowVertexAnimator_ = ArrowVertexAnimator::create();
            arrowVertexAnimator_->posArray = posArray;
            arrowVertexAnimator_->indices = arrowIndex;
            arrowVertexAnimator_->tip = arrowTip;
            arrowVertexAnimator_->offsetPixels = arrowOffsetPixels;
        }

        auto drawCommands = vsg::Commands::create();
        drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{posArray, colorArray}));
        drawCommands->addChild(vsg::Draw::create(static_cast<uint32_t>(fillTris.size()), 1, 0, 0));

        stateGroup->addChild(drawCommands);
        root->addChild(stateGroup);

        // Label variant: fill.frag has no AA already; swap in labelColor and
        // disable blending. Reuses `posArray` directly, so arrowVertexAnimator_
        // rewriting arrowhead positions on zoom (it holds a ref to this same
        // array) keeps this batch in sync automatically.
        auto labelStateGroup = makePipeline(shaderDir, "fill.vert.spv", "fill.frag.spv", bindings, attributes, /*blend=*/false);
        auto labelDrawCommands = vsg::Commands::create();
        labelDrawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{posArray, labelColorArray}));
        labelDrawCommands->addChild(vsg::Draw::create(static_cast<uint32_t>(fillTris.size()), 1, 0, 0));
        labelStateGroup->addChild(labelDrawCommands);
        labelRoot->addChild(labelStateGroup);
    }

    if (textGroup->children.size() > 0) root->addChild(textGroup);

    labelGraph_ = labelRoot;

    return root;
}

} // namespace giv
