#include "SceneBuilder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace giv
{

void ViewParams::update(float worldPerPixel)
{
    if (!value) return;
    value->value().x = worldPerPixel;
    // Written (16 bytes, whatever the scene's size) and dirtied on *every*
    // frame, not just when the value actually changes: vsg::TransferTask
    // keeps one GPU copy of a DYNAMIC_DATA buffer per in-flight frame and
    // refreshes only the copy belonging to the frame being recorded, so a
    // one-off dirty() leaves the other copies holding a stale scale and the
    // rendered mark/line sizes flicker between the old and new zoom.
    value->dirty();
}

void ImageFilterAnimator::update(float worldPerPixel)
{
    if (filterSwitches.empty()) return;
    if (std::abs(worldPerPixel - lastWorldPerPixel_) < 1e-9f) return;
    lastWorldPerPixel_ = worldPerPixel;

    // 0 = nearest branch, 1 = linear branch - see ImageFilterAnimator's doc
    // comment in SceneBuilder.h for the worldPerPixel <= 1 threshold.
    unsigned int idx = worldPerPixel <= 1.0f ? 0 : 1;
    for (auto& sw : filterSwitches)
        sw->setSingleChildOn(idx);
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

// Builds a graphics pipeline bind command with the given vertex
// bindings/attributes, no descriptor sets, and the standard VSG-provided
// {projection, modelview} push-constant matrices. No depth test (flat 2D
// scene), no back-face culling (triangle winding isn't guaranteed by our
// simple fan triangulation), alpha blending enabled.
//
// Returns just the bind command (not a StateGroup) so callers can wrap it
// in as many separate per-dataset StateGroups as they need - see the
// marks/fill/lines "interleave" loop in build(), which draws each
// dataset's sub-range of a batch under its own StateGroup so datasets can
// be interleaved in painter's-algorithm (submission) order while still
// sharing one pipeline object.
vsg::ref_ptr<vsg::BindGraphicsPipeline> makePipeline(const std::string& shaderDir,
                                            const std::string& vertFile,
                                            const std::string& fragFile,
                                            const vsg::VertexInputState::Bindings& bindings,
                                            const vsg::VertexInputState::Attributes& attributes,
                                            vsg::ref_ptr<vsg::PipelineLayout> pipelineLayout,
                                            bool blend = true)
{
    auto vertexShader = loadShader(VK_SHADER_STAGE_VERTEX_BIT, shaderDir, vertFile);
    auto fragmentShader = loadShader(VK_SHADER_STAGE_FRAGMENT_BIT, shaderDir, fragFile);

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

    auto graphicsPipeline = vsg::GraphicsPipeline::create(pipelineLayout, vsg::ShaderStages{vertexShader, fragmentShader}, pipelineStates);

    return vsg::BindGraphicsPipeline::create(graphicsPipeline);
}

// Wraps a (shared, reusable) pipeline bind command and one Commands node
// in a fresh StateGroup. Used to add one sub-range draw per dataset as its
// own scene-graph node, so per-dataset draws from different batches
// (marks/fill/lines) can be interleaved as siblings under `root`/
// `labelRoot` in submission order - see the "interleave" loop below.
vsg::ref_ptr<vsg::StateGroup> wrapDraw(vsg::ref_ptr<vsg::BindGraphicsPipeline> bind, vsg::ref_ptr<vsg::Commands> draw)
{
    auto stateGroup = vsg::StateGroup::create();
    stateGroup->add(bind);
    stateGroup->addChild(draw);
    return stateGroup;
}

// Like wrapDraw(), plus a per-draw descriptor set bind - used for the
// per-dataset raster-sprite draws (see the "sprite" handling in the
// interleave loop below), where each sprite has its own texture/descriptor
// set but shares one pipeline bind.
vsg::ref_ptr<vsg::StateGroup> wrapImageDraw(vsg::ref_ptr<vsg::BindGraphicsPipeline> bind,
                                             vsg::ref_ptr<vsg::BindDescriptorSet> descriptorBind,
                                             vsg::ref_ptr<vsg::Commands> draw)
{
    auto stateGroup = vsg::StateGroup::create();
    stateGroup->add(bind);
    stateGroup->add(descriptorBind);
    stateGroup->addChild(draw);
    return stateGroup;
}

struct ImagePipeline
{
    vsg::ref_ptr<vsg::StateGroup> stateGroup;
    vsg::ref_ptr<vsg::PipelineLayout> pipelineLayout;
};

// Like makePipeline(), but with one combined-image-sampler descriptor set
// binding (fragment stage) - needed for textured $image quads, which no
// other batch in this file uses. Returns the pipelineLayout alongside the
// stateGroup since each image's per-filter vsg::DescriptorSet is built
// against it (see the "Image batch" section of build()).
ImagePipeline makeImagePipeline(const std::string& shaderDir)
{
    auto vertexShader = loadShader(VK_SHADER_STAGE_VERTEX_BIT, shaderDir, "image.vert.spv");
    auto fragmentShader = loadShader(VK_SHADER_STAGE_FRAGMENT_BIT, shaderDir, "image.frag.spv");

    vsg::PushConstantRanges pushConstantRanges{
        {VK_SHADER_STAGE_VERTEX_BIT, 0, 128} // projection + modelview, auto-supplied by RecordTraversal
    };

    auto descriptorSetLayout = vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});

    auto rasterization = vsg::RasterizationState::create();
    rasterization->cullMode = VK_CULL_MODE_NONE;

    auto depthStencil = vsg::DepthStencilState::create();
    depthStencil->depthTestEnable = VK_FALSE;
    depthStencil->depthWriteEnable = VK_FALSE;

    auto colorBlend = vsg::ColorBlendState::create();
    colorBlend->configureAttachments(true);

    vsg::VertexInputState::Bindings bindings{
        VkVertexInputBindingDescription{0, sizeof(vsg::vec2), VK_VERTEX_INPUT_RATE_VERTEX},
        VkVertexInputBindingDescription{1, sizeof(vsg::vec2), VK_VERTEX_INPUT_RATE_VERTEX}};
    vsg::VertexInputState::Attributes attributes{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32G32_SFLOAT, 0}};

    vsg::GraphicsPipelineStates pipelineStates{
        vsg::VertexInputState::create(bindings, attributes),
        vsg::InputAssemblyState::create(),
        rasterization,
        vsg::MultisampleState::create(),
        colorBlend,
        depthStencil};

    auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{descriptorSetLayout}, pushConstantRanges);
    auto graphicsPipeline = vsg::GraphicsPipeline::create(pipelineLayout, vsg::ShaderStages{vertexShader, fragmentShader}, pipelineStates);

    auto stateGroup = vsg::StateGroup::create();
    stateGroup->add(vsg::BindGraphicsPipeline::create(graphicsPipeline));
    return {stateGroup, pipelineLayout};
}

// vsg::createTextShaderSet()'s built-in pipeline states fall back to
// vsg::DepthStencilState's default (depthTestEnable/depthWriteEnable both
// true), unlike every other batch here (see makePipeline()'s "no depth
// test (flat 2D scene)" above) - since giv's balloon-tooltip overlay is a
// second vsg::View sharing the same window RenderGraph and painted with
// depth testing disabled (so it always wins the fragments it touches
// regardless of submission order relative to that depth buffer's
// contents), a text glyph that passed *its own* depth test and wrote a
// depth value earlier in the same frame can still show back through the
// overlay's non-depth-tested box/label afterwards. Appending a disabled
// DepthStencilState - appended, not replacing in place, so
// GraphicsPipelineConfigurator's "last DepthStencilState of this type
// wins" merge (see AssignGraphicsPipelineStates::apply(DepthStencilState&))
// picks it over the shaderSet's own built-in one - keeps text flat/2D like
// every other mark/line/fill batch. Only called when options_->shaderSets
// has no "text" entry yet, so createTextShaderSet() here returns a fresh,
// uniquely-owned ShaderSet (freshly deserialized from vsg's embedded
// binary, not a shared/cached singleton) - safe to mutate directly. Once
// cached into options_->shaderSets["text"] below, this same flattened
// instance is what BalloonOverlay's own tooltip text resolves too, since
// it looks up its shaderSet through the same vsg::createTextShaderSet(options_).
vsg::ref_ptr<vsg::ShaderSet> makeFlatTextShaderSet(vsg::ref_ptr<vsg::Options> options)
{
    auto shaderSet = vsg::createTextShaderSet(options);
    auto depthStencil = vsg::DepthStencilState::create();
    depthStencil->depthTestEnable = VK_FALSE;
    depthStencil->depthWriteEnable = VK_FALSE;
    shaderSet->defaultGraphicsPipelineStates.push_back(depthStencil);
    return shaderSet;
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
// are built with - see giv::ViewParams).
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
// to the newly-appended `tris` entries); build() feeds those to the fill
// batch's position/offset vertex buffers and fill.vert resolves them to
// world space against the current zoom - see giv::ViewParams. The position
// written into `tris` here is overwritten with the tip by that same code.
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

// Ear-clipping triangulation of a single simple (non-self-intersecting)
// polygon contour - handles concave outlines correctly, unlike a vertex-0
// fan (which overfills across any concavity). O(n^2); fine at the point
// counts a single dataset/SVG-shape subpath produces.
bool pointInTriangle(const vsg::vec2& p, const vsg::vec2& a, const vsg::vec2& b, const vsg::vec2& c)
{
    float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
    float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
    float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
    bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(hasNeg && hasPos);
}

void triangulatePolygon(std::vector<FillVertex>& tris, const std::vector<vsg::vec2>& poly, const vsg::vec4& color,
                         const vsg::vec4& labelColor)
{
    if (poly.size() < 3) return;

    vsg::vec2 bbMin = poly[0], bbMax = poly[0];
    for (const auto& p : poly)
    {
        bbMin.x = std::min(bbMin.x, p.x);
        bbMin.y = std::min(bbMin.y, p.y);
        bbMax.x = std::max(bbMax.x, p.x);
        bbMax.y = std::max(bbMax.y, p.y);
    }
    const float diag = std::max(1e-6f, std::hypot(bbMax.x - bbMin.x, bbMax.y - bbMin.y));
    const float eps = diag * 1e-5f; // scale-relative, not a fixed absolute tolerance

    // Collapse near-duplicate consecutive points - bezier flattening plus
    // the synthetic duplicate Op::ClosePath appends after a subpath whose
    // curve already lands back on its start point (common for hand-drawn
    // SVG art, e.g. inkscape control points close to the anchor) otherwise
    // leaves a near-zero-length edge. That edge's degenerate cross product
    // can misclassify its vertex as reflex/blocked under exact arithmetic,
    // which can stall the ear search below and leave the polygon partially
    // untriangulated - a visible notch cut into the fill.
    std::vector<vsg::vec2> ring;
    ring.reserve(poly.size());
    for (const auto& p : poly)
    {
        if (ring.empty() || std::hypot(p.x - ring.back().x, p.y - ring.back().y) > eps) ring.push_back(p);
    }
    if (ring.size() > 3 && std::hypot(ring.front().x - ring.back().x, ring.front().y - ring.back().y) <= eps)
        ring.pop_back();
    if (ring.size() < 3) return;

    double signedArea = 0.0;
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const vsg::vec2& a = ring[i];
        const vsg::vec2& b = ring[(i + 1) % ring.size()];
        signedArea += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    const bool ccw = signedArea > 0.0;

    std::vector<int> idx(ring.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);

    const float crossTol = diag * diag * 1e-7f; // near-collinear vertices count as convex, not reflex

    int guard = static_cast<int>(idx.size()) * static_cast<int>(idx.size()) + 8;
    while (idx.size() > 3 && guard-- > 0)
    {
        int bestI = -1;
        int bestViolations = -1;
        for (size_t i = 0; i < idx.size(); ++i)
        {
            size_t iPrev = (i + idx.size() - 1) % idx.size();
            size_t iNext = (i + 1) % idx.size();
            const vsg::vec2& a = ring[idx[iPrev]];
            const vsg::vec2& b = ring[idx[i]];
            const vsg::vec2& c = ring[idx[iNext]];

            float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (ccw ? (cross <= -crossTol) : (cross >= crossTol)) continue; // clearly reflex, not an ear

            int violations = 0;
            for (size_t k = 0; k < idx.size(); ++k)
            {
                if (k == i || k == iPrev || k == iNext) continue;
                if (pointInTriangle(ring[idx[k]], a, b, c)) ++violations;
            }
            if (violations == 0)
            {
                bestI = static_cast<int>(i);
                break;
            }
            // No clean ear this pass (can happen on self-intersecting or
            // numerically borderline input) - remember the least-bad convex
            // vertex so the loop still makes progress instead of stalling
            // and leaving the remaining vertices unfilled.
            if (bestViolations < 0 || violations < bestViolations)
            {
                bestViolations = violations;
                bestI = static_cast<int>(i);
            }
        }

        if (bestI < 0) break; // no convex vertex at all left (shouldn't happen for a simple polygon)

        size_t i = static_cast<size_t>(bestI);
        size_t iPrev = (i + idx.size() - 1) % idx.size();
        size_t iNext = (i + 1) % idx.size();
        tris.push_back({ring[idx[iPrev]], color, labelColor});
        tris.push_back({ring[idx[i]], color, labelColor});
        tris.push_back({ring[idx[iNext]], color, labelColor});
        idx.erase(idx.begin() + bestI);
    }

    if (idx.size() == 3)
    {
        tris.push_back({ring[idx[0]], color, labelColor});
        tris.push_back({ring[idx[1]], color, labelColor});
        tris.push_back({ring[idx[2]], color, labelColor});
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

vsg::ref_ptr<vsg::Group> SceneBuilder::build(const SceneData& scene, const std::string& shaderDir,
                                              const std::vector<LoadedImage>& images)
{
    // The one per-frame uniform every marks/lines/fill batch below reads its
    // pixel-to-world scale from - see giv::ViewParams. All six of those
    // pipelines (three batches x display/label variant) share one pipeline
    // layout, so one vkCmdBindDescriptorSets at the root of each scene graph
    // covers every draw under it instead of one per dataset - which matters
    // for files with hundreds of thousands of tiny datasets, where per-draw
    // recording cost is what limits the frame rate.
    viewParams_ = ViewParams::create();
    viewParams_->value = vsg::vec4Value::create(vsg::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    viewParams_->value->properties.dataVariance = vsg::DYNAMIC_DATA;
    auto viewParamsLayout = vsg::DescriptorSetLayout::create(
        vsg::DescriptorSetLayoutBindings{{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
    auto viewParamsSet = vsg::DescriptorSet::create(
        viewParamsLayout, vsg::Descriptors{vsg::DescriptorBuffer::create(viewParams_->value, 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)});
    auto vectorPipelineLayout = vsg::PipelineLayout::create(
        vsg::DescriptorSetLayouts{viewParamsLayout},
        vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT, 0, 128}}); // projection + modelview, auto-supplied by RecordTraversal
    auto bindViewParams = vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, vectorPipelineLayout, 0, viewParamsSet);

    // Both scene graphs are StateGroups purely to carry that one bind; the
    // image/sprite and text draws below bind descriptor sets of their own,
    // and vsg::StateGroup's state stack re-binds this one after each such
    // subtree.
    auto root = vsg::StateGroup::create();
    root->add(bindViewParams);

    // -------------------------------------------------------------
    // Image batch ($image references) - added first so vector data
    // (marks/lines/fill/text, appended to root below) always draws on
    // top of any background image.
    // -------------------------------------------------------------
    if (!images.empty())
    {
        auto [imageStateGroup, imagePipelineLayout] = makeImagePipeline(shaderDir);

        auto nearestSampler = vsg::Sampler::create();
        nearestSampler->minFilter = VK_FILTER_NEAREST;
        nearestSampler->magFilter = VK_FILTER_NEAREST;
        nearestSampler->addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        nearestSampler->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        auto linearSampler = vsg::Sampler::create();
        linearSampler->minFilter = VK_FILTER_LINEAR;
        linearSampler->magFilter = VK_FILTER_LINEAR;
        linearSampler->addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        linearSampler->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        auto imageQuadIndices = vsg::ushortArray::create({0, 1, 2, 2, 3, 0});

        imageSwitch_ = vsg::Switch::create();
        imageFilterAnimator_ = ImageFilterAnimator::create();

        for (const LoadedImage& li : images)
        {
            const float w = static_cast<float>(li.width);
            const float h = static_cast<float>(li.height);
            // Top-left at world (0,0), extending to (w,-h): giv's $image has
            // no placement/calibration args (one image pixel == one world
            // unit, anchored at the origin), and -h applies the same
            // Y-negation convention as every other primitive (see the main
            // dataset loop below).
            auto positions = vsg::vec2Array::create({{0.0f, 0.0f}, {w, 0.0f}, {w, -h}, {0.0f, -h}});
            auto uvs = vsg::vec2Array::create({{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}});

            auto textureData =
                vsg::ubvec4Array2D::create(static_cast<uint32_t>(li.width), static_cast<uint32_t>(li.height),
                                            vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
            std::memcpy(textureData->dataPointer(), li.rgba.data(), li.rgba.size());

            auto innerSwitch = vsg::Switch::create(); // child 0 = nearest, child 1 = linear
            for (auto& sampler : {nearestSampler, linearSampler})
            {
                auto descriptorImage = vsg::DescriptorImage::create(sampler, textureData, 0, 0,
                                                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                auto descriptorSet =
                    vsg::DescriptorSet::create(imagePipelineLayout->setLayouts[0], vsg::Descriptors{descriptorImage});
                auto bindDescriptorSet = vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                                         imagePipelineLayout, 0, descriptorSet);

                auto variantGroup = vsg::StateGroup::create();
                variantGroup->add(bindDescriptorSet);

                auto drawCommands = vsg::Commands::create();
                drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{positions, uvs}));
                drawCommands->addChild(vsg::BindIndexBuffer::create(imageQuadIndices));
                drawCommands->addChild(vsg::DrawIndexed::create(6, 1, 0, 0, 0));
                variantGroup->addChild(drawCommands);

                innerSwitch->addChild(true, variantGroup);
            }
            innerSwitch->setSingleChildOn(0); // default: nearest (worldPerPixel unknown until first update())
            imageFilterAnimator_->filterSwitches.push_back(innerSwitch);

            imageSwitch_->addChild(true, innerSwitch);
        }
        imageSwitch_->setSingleChildOn(0); // default: first image

        imageStateGroup->addChild(imageSwitch_);
        root->addChild(imageStateGroup);
    }

    // -------------------------------------------------------------
    // Raster-sprite pipeline (see Dataset::isSprite / SvgLoader) - built
    // lazily, only if some dataset actually needs it, reusing the exact
    // same textured-quad shaders as the $image batch above. Unlike that
    // batch (one shared state group, drawn once, always behind the
    // vector data), each sprite needs its own position/size/texture *and*
    // must interleave with its neighboring vector datasets in document
    // order - see the per-dataset loop below - so only the pipeline
    // itself is shared here; each sprite gets its own descriptor set/draw.
    // -------------------------------------------------------------
    vsg::ref_ptr<vsg::BindGraphicsPipeline> spriteBind;
    vsg::ref_ptr<vsg::PipelineLayout> spritePipelineLayout;
    vsg::ref_ptr<vsg::Sampler> spriteSampler;
    bool hasSprites = std::any_of(scene.datasets.begin(), scene.datasets.end(), [](const Dataset& ds) { return ds.isSprite; });
    if (hasSprites)
    {
        auto [spriteStateGroup, layout] = makeImagePipeline(shaderDir);
        spriteBind = spriteStateGroup->stateCommands.front().cast<vsg::BindGraphicsPipeline>();
        spritePipelineLayout = layout;

        spriteSampler = vsg::Sampler::create();
        spriteSampler->minFilter = VK_FILTER_LINEAR;
        spriteSampler->magFilter = VK_FILTER_LINEAR;
        spriteSampler->addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        spriteSampler->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }

    std::vector<MarkInstance> marks;
    std::vector<bool> markScalesWithZoom; // parallel to `marks`; see giv::ViewParams
    std::vector<LineInstance> lines;
    std::vector<FillVertex> fillTris;

    // giv semantics: a later dataset's marks/fill/lines must visually
    // overwrite anything an earlier dataset painted where they overlap
    // (pure painter's algorithm, in dataset order), and within one
    // dataset a polygon's outline stroke must paint on top of its own
    // fill. The scene is flat 2D with no depth test (see makePipeline's
    // doc comment), so both orderings only come from scene-graph child
    // order at draw time. marks/fillTris/lines above are still one
    // global buffer per primitive type (for batching), but the actual
    // Draw commands below are split into one sub-range per dataset and
    // interleaved marks-then-fill-then-lines, dataset by dataset, so the
    // submission order matches giv's - see the "interleave" loop after
    // the marks/fill/lines batch setup below.
    struct Range
    {
        uint32_t start = 0;
        uint32_t count = 0;
    };
    std::vector<Range> markRanges(scene.datasets.size());
    std::vector<Range> fillRanges(scene.datasets.size());
    std::vector<Range> lineRanges(scene.datasets.size());
    // Indices into `fillTris` of just the arrowhead vertices (polygon-fill
    // vertices are not pixel-constant and have no entry here), with
    // `arrowTip`/`arrowOffsetPixels` parallel to `arrowIndex`; see
    // addArrowHead's doc comment and giv::ViewParams.
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
        options_->shaderSets["text"] = makeFlatTextShaderSet(options_);

    for (size_t dsIdx = 0; dsIdx < scene.datasets.size(); ++dsIdx)
    {
        const Dataset& ds = scene.datasets[dsIdx];
        if (!ds.isVisible) continue;

        markRanges[dsIdx].start = static_cast<uint32_t>(marks.size());
        fillRanges[dsIdx].start = static_cast<uint32_t>(fillTris.size());
        lineRanges[dsIdx].start = static_cast<uint32_t>(lines.size());

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

                if (ds.doDrawPolygon) triangulatePolygon(fillTris, sp, fillColor, labelColor);
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

        markRanges[dsIdx].count = static_cast<uint32_t>(marks.size()) - markRanges[dsIdx].start;
        fillRanges[dsIdx].count = static_cast<uint32_t>(fillTris.size()) - fillRanges[dsIdx].start;
        lineRanges[dsIdx].count = static_cast<uint32_t>(lines.size()) - lineRanges[dsIdx].start;
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
    auto labelRoot = vsg::StateGroup::create();
    labelRoot->add(bindViewParams);

    // -------------------------------------------------------------
    // Marks batch - build the pipeline binds and full vertex/instance
    // buffers, but don't draw yet; the "interleave" loop below issues one
    // sub-range draw per dataset so marks/fill/lines can be interleaved
    // in dataset order (see the Range comment above).
    // -------------------------------------------------------------
    vsg::ref_ptr<vsg::BindGraphicsPipeline> marksBind, marksLabelBind;
    vsg::ref_ptr<vsg::vec4Array> markPosSizeModeArray, markColorArray, markLabelColorArray;
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

        marksBind = makePipeline(shaderDir, "marks.vert.spv", "marks.frag.spv", bindings, attributes, vectorPipelineLayout);

        markPosSizeModeArray = vsg::vec4Array::create(static_cast<uint32_t>(marks.size()));
        markColorArray = vsg::vec4Array::create(static_cast<uint32_t>(marks.size()));
        markLabelColorArray = vsg::vec4Array::create(static_cast<uint32_t>(marks.size()));
        for (size_t i = 0; i < marks.size(); ++i)
        {
            vsg::vec4 posSizeMode = marks[i].posSizeMode;
            // marks[i].posSizeMode.z holds ds.markSize * 0.5, which is a
            // *pixel* half-size unless the dataset asked for $scale_marks 1
            // (giv's non-default do_scale_marks == TRUE). marks.vert reads a
            // negative half-size as "this many screen pixels" and converts
            // it with the per-frame ViewParams scale, so the whole buffer
            // stays static no matter how the view zooms - see ViewParams.
            if (!markScalesWithZoom[i]) posSizeMode.z = -posSizeMode.z;
            markPosSizeModeArray->set(i, posSizeMode);
            markColorArray->set(i, marks[i].color);
            markLabelColorArray->set(i, marks[i].labelColor);
        }

        // Label variant: same quad/position/mode buffers (marks_label.frag
        // reads fragMode/fragCorner exactly like marks.frag), labelColor in
        // place of color, hard edges, no blending.
        marksLabelBind = makePipeline(shaderDir, "marks.vert.spv", "marks_label.frag.spv", bindings, attributes, vectorPipelineLayout, /*blend=*/false);
    }

    // -------------------------------------------------------------
    // Fill (polygons + arrow/quiver heads) batch - same two-phase
    // build/draw split as the marks batch above. Each dataset's fill
    // sub-range is drawn before that dataset's lines sub-range (see the
    // interleave loop) so a polygon's outline stroke (part of the lines
    // batch, see ds.doDrawPolygonOutline above) paints on top of its own
    // fill instead of being covered by it.
    // -------------------------------------------------------------
    vsg::ref_ptr<vsg::BindGraphicsPipeline> fillBind, fillLabelBind;
    vsg::ref_ptr<vsg::vec2Array> fillPosArray, fillOffsetPixelsArray;
    vsg::ref_ptr<vsg::vec4Array> fillColorArray, fillLabelColorArray;
    if (!fillTris.empty())
    {
        vsg::VertexInputState::Bindings bindings{
            VkVertexInputBindingDescription{0, sizeof(vsg::vec2), VK_VERTEX_INPUT_RATE_VERTEX},
            VkVertexInputBindingDescription{1, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_VERTEX},
            VkVertexInputBindingDescription{2, sizeof(vsg::vec2), VK_VERTEX_INPUT_RATE_VERTEX}};
        // pos/color/offset are separate tightly-packed buffers; see note above.
        vsg::VertexInputState::Attributes attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
            VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            VkVertexInputAttributeDescription{2, 2, VK_FORMAT_R32G32_SFLOAT, 0}};

        fillBind = makePipeline(shaderDir, "fill.vert.spv", "fill.frag.spv", bindings, attributes, vectorPipelineLayout);

        fillPosArray = vsg::vec2Array::create(static_cast<uint32_t>(fillTris.size()));
        fillColorArray = vsg::vec4Array::create(static_cast<uint32_t>(fillTris.size()));
        fillLabelColorArray = vsg::vec4Array::create(static_cast<uint32_t>(fillTris.size()));
        fillOffsetPixelsArray = vsg::vec2Array::create(static_cast<uint32_t>(fillTris.size()));
        for (size_t i = 0; i < fillTris.size(); ++i)
        {
            fillPosArray->set(i, fillTris[i].pos);
            fillColorArray->set(i, fillTris[i].color);
            fillLabelColorArray->set(i, fillTris[i].labelColor);
            fillOffsetPixelsArray->set(i, vsg::vec2(0.0f, 0.0f));
        }
        // Arrowhead vertices must stay pixel-constant in size as the view
        // zooms (see addArrowHead's doc comment), so instead of a world-space
        // position each one stores its arrow's tip plus a screen-pixel
        // offset, which fill.vert resolves against the current ViewParams
        // scale - leaving this buffer static across zooms too.
        for (size_t k = 0; k < arrowIndex.size(); ++k)
        {
            fillPosArray->set(arrowIndex[k], arrowTip[k]);
            fillOffsetPixelsArray->set(arrowIndex[k], arrowOffsetPixels[k]);
        }

        // Label variant: fill.frag has no AA already; swap in labelColor and
        // disable blending.
        fillLabelBind = makePipeline(shaderDir, "fill.vert.spv", "fill.frag.spv", bindings, attributes, vectorPipelineLayout, /*blend=*/false);
    }

    // -------------------------------------------------------------
    // Lines batch - same two-phase build/draw split as above.
    // -------------------------------------------------------------
    vsg::ref_ptr<vsg::BindGraphicsPipeline> linesBind, linesLabelBind;
    vsg::ref_ptr<vsg::vec4Array> linesP0p1Array, linesWidthDashArray, linesColorArray, linesLabelColorArray, linesLabelWidthDashArray;
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

        linesBind = makePipeline(shaderDir, "lines.vert.spv", "lines.frag.spv", bindings, attributes, vectorPipelineLayout);

        linesP0p1Array = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        linesWidthDashArray = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        // giv always draws line/outline/quiver-shaft width in constant
        // device pixels regardless of zoom (GivPainterCairo::set_line_width
        // sets cairo's line width directly with no CTM scale applied, and
        // there's no $scale_marks-equivalent toggle for line width - see
        // giv-data.cc default_line_width), so widthDash.x stays in pixels
        // here and lines.vert scales it by the per-frame ViewParams value.
        linesColorArray = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        linesLabelColorArray = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        for (size_t i = 0; i < lines.size(); ++i)
        {
            linesP0p1Array->set(i, lines[i].p0p1);
            linesWidthDashArray->set(i, lines[i].widthDash);
            linesColorArray->set(i, lines[i].color);
            linesLabelColorArray->set(i, lines[i].labelColor);
        }

        // Label-pass copy of widthDashArray, but with the half-width
        // widened to a 1.5px floor (3px total) - matches giv's own
        // do_paint_by_index picking pass (GivPainterAgg::set_line_width /
        // GivPainterCairo::set_line_width: `if (do_paint_by_index &&
        // line_width < 3) line_width = 3;`), which gives thin lines a
        // bigger hit area to hover without changing how thick they actually
        // look on screen. giv applies no equivalent widening to marks.
        linesLabelWidthDashArray = vsg::vec4Array::create(static_cast<uint32_t>(lines.size()));
        for (size_t i = 0; i < lines.size(); ++i)
        {
            vsg::vec4 widthDash = lines[i].widthDash;
            widthDash.x = std::max(widthDash.x, 1.5f);
            linesLabelWidthDashArray->set(i, widthDash);
        }

        // Label variant: lines.frag already has no AA (only a hard dash
        // discard), so it's reused as-is; just swap in labelColor, the
        // widened linesLabelWidthDashArray (see above), and disable blending.
        linesLabelBind = makePipeline(shaderDir, "lines.vert.spv", "lines.frag.spv", bindings, attributes, vectorPipelineLayout, /*blend=*/false);
    }

    // -------------------------------------------------------------
    // Interleave: one sub-range draw per dataset, in dataset order and
    // marks-then-fill-then-lines within a dataset, so overlapping
    // datasets composite with giv's painter's-algorithm semantics (a
    // later dataset overwrites an earlier one) instead of every mark
    // batch, then every fill, then every line across the whole scene.
    // -------------------------------------------------------------
    auto spriteQuadIndices = vsg::ushortArray::create({0, 1, 2, 2, 3, 0});

    // Bind nodes for the shared, whole-scene marks/fill/lines arrays are
    // built ONCE here and the same node objects are reused in every
    // dataset's draw commands below (not re-created per dataset). This
    // matters more than it looks: vsg::BindVertexBuffers::assignArrays()
    // wraps each vsg::Data in a brand-new vsg::BufferInfo, and
    // BindVertexBuffers::compile() re-uploads to a freshly allocated GPU
    // buffer whenever it sees a BufferInfo it hasn't compiled before -
    // there's no dedup by the underlying vsg::Data pointer across
    // different BindVertexBuffers nodes. So calling
    // vsg::BindVertexBuffers::create() again inside the per-dataset loop
    // (as this used to do) re-uploaded the *entire* scene's marks/fill/
    // lines arrays to a brand-new buffer once per dataset instead of once
    // total. For a file with a handful of large datasets that's wasteful;
    // for one with hundreds of thousands of tiny datasets (e.g.
    // GeomSimPrintedPattern.giv: 477,838 datasets for only 482,647 total
    // points) it re-uploads the whole-scene arrays 477,838 times and
    // exhausts GPU memory during Viewer::compile(), throwing
    // vsg::Exception(VK_ERROR_OUT_OF_DEVICE_MEMORY) before a single frame
    // is drawn. Sharing one already-compiled node across every dataset's
    // draw commands avoids the re-upload: compile() checks each
    // BufferInfo's own copied-modified-count, so calling compile() again
    // on the *same* node is a cheap no-op once it has been compiled once.
    vsg::ref_ptr<vsg::BindVertexBuffers> marksVertexBind, marksLabelVertexBind;
    vsg::ref_ptr<vsg::BindVertexBuffers> fillVertexBind, fillLabelVertexBind;
    vsg::ref_ptr<vsg::BindVertexBuffers> linesVertexBind, linesLabelVertexBind;
    vsg::ref_ptr<vsg::BindIndexBuffer> quadIndexBind;

    if (marksBind)
    {
        marksVertexBind = vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, markPosSizeModeArray, markColorArray});
        marksLabelVertexBind = vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, markPosSizeModeArray, markLabelColorArray});
        quadIndexBind = vsg::BindIndexBuffer::create(quadIndices);
    }
    if (fillBind)
    {
        fillVertexBind = vsg::BindVertexBuffers::create(0, vsg::DataList{fillPosArray, fillColorArray, fillOffsetPixelsArray});
        fillLabelVertexBind = vsg::BindVertexBuffers::create(0, vsg::DataList{fillPosArray, fillLabelColorArray, fillOffsetPixelsArray});
    }
    if (linesBind)
    {
        linesVertexBind = vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, linesP0p1Array, linesWidthDashArray, linesColorArray});
        linesLabelVertexBind =
            vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, linesP0p1Array, linesLabelWidthDashArray, linesLabelColorArray});
        if (!quadIndexBind) quadIndexBind = vsg::BindIndexBuffer::create(quadIndices);
    }

    // Consecutive datasets very often contribute to the same batch and
    // nothing else (a run of mark-only datasets, say), and their sub-ranges
    // are contiguous by construction - so instead of one draw each they are
    // grown into a single wider draw, which is what the `lastKind`/`lastEnd`
    // bookkeeping below is for. Files built out of hundreds of thousands of
    // one- or two-point datasets are recording-bound (one pipeline bind,
    // vertex/index binds and draw per dataset, every frame), and this cuts
    // that per-frame work by the length of each run.
    enum class BatchKind
    {
        None,
        Marks,
        Fill,
        Lines
    };
    BatchKind lastKind = BatchKind::None;
    uint32_t lastEnd = 0;
    vsg::ref_ptr<vsg::DrawIndexed> lastDrawIndexed, lastLabelDrawIndexed;
    vsg::ref_ptr<vsg::Draw> lastDraw, lastLabelDraw;

    // A sprite draw has to keep its exact place in submission order, so it
    // ends any run that a following dataset could otherwise have joined.
    auto breakRun = [&]() { lastKind = BatchKind::None; };

    auto continuesRun = [&](BatchKind kind, const Range& r) { return kind == lastKind && r.start == lastEnd; };

    auto emitIndexed = [&](BatchKind kind, const Range& r, vsg::ref_ptr<vsg::BindGraphicsPipeline> bind,
                            vsg::ref_ptr<vsg::BindGraphicsPipeline> labelBind, vsg::ref_ptr<vsg::BindVertexBuffers> vertexBind,
                            vsg::ref_ptr<vsg::BindVertexBuffers> labelVertexBind) {
        if (!bind || r.count == 0) return;
        if (continuesRun(kind, r))
        {
            lastDrawIndexed->instanceCount += r.count;
            lastLabelDrawIndexed->instanceCount += r.count;
        }
        else
        {
            lastDrawIndexed = vsg::DrawIndexed::create(6, r.count, 0, 0, r.start);
            lastLabelDrawIndexed = vsg::DrawIndexed::create(6, r.count, 0, 0, r.start);

            auto drawCommands = vsg::Commands::create();
            drawCommands->addChild(vertexBind);
            drawCommands->addChild(quadIndexBind);
            drawCommands->addChild(lastDrawIndexed);
            root->addChild(wrapDraw(bind, drawCommands));

            auto labelDrawCommands = vsg::Commands::create();
            labelDrawCommands->addChild(labelVertexBind);
            labelDrawCommands->addChild(quadIndexBind);
            labelDrawCommands->addChild(lastLabelDrawIndexed);
            labelRoot->addChild(wrapDraw(labelBind, labelDrawCommands));
            lastKind = kind;
        }
        lastEnd = r.start + r.count;
    };

    auto emitMarks = [&](const Range& r) { emitIndexed(BatchKind::Marks, r, marksBind, marksLabelBind, marksVertexBind, marksLabelVertexBind); };
    auto emitLines = [&](const Range& r) { emitIndexed(BatchKind::Lines, r, linesBind, linesLabelBind, linesVertexBind, linesLabelVertexBind); };

    auto emitFill = [&](const Range& r) {
        if (!fillBind || r.count == 0) return;
        if (continuesRun(BatchKind::Fill, r))
        {
            lastDraw->vertexCount += r.count;
            lastLabelDraw->vertexCount += r.count;
        }
        else
        {
            lastDraw = vsg::Draw::create(r.count, 1, r.start, 0);
            lastLabelDraw = vsg::Draw::create(r.count, 1, r.start, 0);

            auto drawCommands = vsg::Commands::create();
            drawCommands->addChild(fillVertexBind);
            drawCommands->addChild(lastDraw);
            root->addChild(wrapDraw(fillBind, drawCommands));

            auto labelDrawCommands = vsg::Commands::create();
            labelDrawCommands->addChild(fillLabelVertexBind);
            labelDrawCommands->addChild(lastLabelDraw);
            labelRoot->addChild(wrapDraw(fillLabelBind, labelDrawCommands));
            lastKind = BatchKind::Fill;
        }
        lastEnd = r.start + r.count;
    };

    for (size_t dsIdx = 0; dsIdx < scene.datasets.size(); ++dsIdx)
    {
        const Dataset& spriteDs = scene.datasets[dsIdx];
        if (spriteDs.isSprite && spriteDs.isVisible && spriteBind && !spriteDs.spriteRGBA.empty())
        {
            const float x0 = static_cast<float>(spriteDs.spriteX);
            const float y0 = static_cast<float>(spriteDs.spriteY);
            const float w = static_cast<float>(spriteDs.spriteW);
            const float h = static_cast<float>(spriteDs.spriteH);
            // Same Y-negation convention as every other primitive (see the
            // Op::Move case above): giv/SVG are Y-down, this renderer Y-up.
            auto positions = vsg::vec2Array::create({{x0, -y0}, {x0 + w, -y0}, {x0 + w, -(y0 + h)}, {x0, -(y0 + h)}});
            auto uvs = vsg::vec2Array::create({{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}});

            auto textureData = vsg::ubvec4Array2D::create(static_cast<uint32_t>(spriteDs.spriteWidth),
                                                             static_cast<uint32_t>(spriteDs.spriteHeight),
                                                             vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
            std::memcpy(textureData->dataPointer(), spriteDs.spriteRGBA.data(), spriteDs.spriteRGBA.size());

            auto descriptorImage =
                vsg::DescriptorImage::create(spriteSampler, textureData, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            auto descriptorSet =
                vsg::DescriptorSet::create(spritePipelineLayout->setLayouts[0], vsg::Descriptors{descriptorImage});
            auto bindDescriptorSet =
                vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, spritePipelineLayout, 0, descriptorSet);

            auto drawCommands = vsg::Commands::create();
            drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{positions, uvs}));
            drawCommands->addChild(vsg::BindIndexBuffer::create(spriteQuadIndices));
            drawCommands->addChild(vsg::DrawIndexed::create(6, 1, 0, 0, 0));
            root->addChild(wrapImageDraw(spriteBind, bindDescriptorSet, drawCommands));
            breakRun();
        }

        emitMarks(markRanges[dsIdx]);
        emitFill(fillRanges[dsIdx]);
        emitLines(lineRanges[dsIdx]);
    }

    if (textGroup->children.size() > 0) root->addChild(textGroup);

    labelGraph_ = labelRoot;

    return root;
}

} // namespace giv
