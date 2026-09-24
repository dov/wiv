#include "SceneBuilder.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace giv
{

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
                                            const vsg::VertexInputState::Attributes& attributes)
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

    auto colorBlend = vsg::ColorBlendState::create();
    colorBlend->configureAttachments(true);

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
};

struct LineInstance
{
    vsg::vec4 p0p1;      // xy = p0, zw = p1
    vsg::vec4 widthDash; // x = half width, y = dashOn, z = dashOff, w = cumulative length at p0
    vsg::vec4 color;
};

struct FillVertex
{
    vsg::vec2 pos;
    vsg::vec4 color;
};

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
                     float halfWidth, float dashOn, float dashOff, float& cumLen, const vsg::vec4& color)
{
    LineInstance li;
    li.p0p1 = vsg::vec4(p0.x, p0.y, p1.x, p1.y);
    li.widthDash = vsg::vec4(halfWidth, dashOn, dashOff, cumLen);
    li.color = color;
    lines.push_back(li);
    cumLen += vsg::length(p1 - p0);
}

// Appends a small filled triangle arrowhead at `tip`, pointing along
// direction `dir` (unit vector), sized relative to `lineHalfWidth`.
void addArrowHead(std::vector<FillVertex>& tris, const vsg::vec2& tip, const vsg::vec2& dir,
                   float size, const vsg::vec4& color)
{
    vsg::vec2 back = vsg::vec2(-dir.x, -dir.y);
    vsg::vec2 normal(-dir.y, dir.x);
    vsg::vec2 base = tip + back * size;
    vsg::vec2 left = base + normal * (size * 0.5f);
    vsg::vec2 right = base - normal * (size * 0.5f);
    tris.push_back({tip, color});
    tris.push_back({left, color});
    tris.push_back({right, color});
}

// Fan-triangulates a (assumed simple-ish) closed polygon from vertex 0.
// Not a full earcut - concave polygons may triangulate incorrectly, noted
// as a Phase 1 simplification.
void addPolygonFill(std::vector<FillVertex>& tris, const std::vector<vsg::vec2>& poly, const vsg::vec4& color)
{
    if (poly.size() < 3) return;
    for (size_t i = 1; i + 1 < poly.size(); ++i)
    {
        tris.push_back({poly[0], color});
        tris.push_back({poly[i], color});
        tris.push_back({poly[i + 1], color});
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

    auto cached = fontCache_.find(family);
    if (cached != fontCache_.end()) return cached->second;

    // Resolve an actual font file via fontconfig (Linux). Falls back to
    // whatever vsgXchange's default search turns up if fc-match is absent.
    std::string path;
    std::string cmd = "fc-match -f '%{file}' \"" + family + "\" 2>/dev/null";
    if (FILE* p = popen(cmd.c_str(), "r"))
    {
        char buf[1024] = {0};
        if (fgets(buf, sizeof(buf), p)) path = buf;
        pclose(p);
    }

    vsg::ref_ptr<vsg::Font> font;
    if (!path.empty())
        font = vsg::read_cast<vsg::Font>(path, options_);

    fontCache_[family] = font; // cache even nullptr, so we don't keep retrying
    return font;
}

vsg::ref_ptr<vsg::Group> SceneBuilder::build(const SceneData& scene, const std::string& shaderDir)
{
    auto root = vsg::Group::create();

    std::vector<MarkInstance> marks;
    std::vector<bool> markScalesWithZoom; // parallel to `marks`; see PixelSizeAnimator
    std::vector<LineInstance> lines;
    std::vector<FillVertex> fillTris;

    // Text nodes are created directly (not batched); typical giv scenes have
    // at most thousands of labels, not millions.
    auto textGroup = vsg::Group::create();

    for (const Dataset& ds : scene.datasets)
    {
        if (!ds.isVisible) continue;

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
                    marks.push_back({vsg::vec4(p.x, p.y, markHalfSize, markMode), markColor});
                    markScalesWithZoom.push_back(ds.doScaleMarks);
                }
                if (needSubpaths)
                {
                    if (!cur.empty()) subpaths.push_back(cur);
                    cur.clear();
                    cur.push_back(p);
                }
                currentPoint = p;
                break;

            case Op::Draw:
                if (ds.doDrawMarks)
                {
                    marks.push_back({vsg::vec4(p.x, p.y, markHalfSize, markMode), markColor});
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
                        cur.clear();
                    }
                    subpaths.push_back(tessellateEllipse(center, wh.x, wh.y, angle, 48));
                }
                currentPoint = center;
                i += 2;
                break;
            }

            case Op::Quiver:
            {
                vsg::vec2 tip = currentPoint + p * static_cast<float>(ds.quiverScale);
                float cum = 0.0f;
                addLineSegment(lines, currentPoint, tip, lineHalfWidth, 1e9f, 0.0f, cum, quiverColor);
                if (ds.quiverHead)
                {
                    vsg::vec2 dir = tip - currentPoint;
                    float len = vsg::length(dir);
                    if (len > 1e-6f) addArrowHead(fillTris, tip, dir / len, std::max(2.0f, lineHalfWidth * 4.0f), quiverColor);
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

        if (needSubpaths && !cur.empty()) subpaths.push_back(cur);

        if (needSubpaths)
        {
            for (const auto& sp : subpaths)
            {
                if (ds.doDrawPolygon) addPolygonFill(fillTris, sp, fillColor);
                if (ds.doDrawPolygonOutline && ds.doDrawPolygon)
                {
                    float cum = 0.0f;
                    for (size_t i = 0; i + 1 < sp.size(); ++i)
                        addLineSegment(lines, sp[i], sp[i + 1], lineHalfWidth, dashOn, dashOff, cum, outlineColor);
                }
                else if (ds.doDrawLines)
                {
                    float cum = 0.0f;
                    for (size_t i = 0; i + 1 < sp.size(); ++i)
                        addLineSegment(lines, sp[i], sp[i + 1], lineHalfWidth, dashOn, dashOff, cum, lineColor);
                }

                if (ds.doDrawLines && ds.arrowType != ArrowType::None && sp.size() >= 2)
                {
                    float headSize = std::max(3.0f, lineHalfWidth * 4.0f);
                    if (ds.arrowType == ArrowType::End || ds.arrowType == ArrowType::Both)
                    {
                        vsg::vec2 dir = sp.back() - sp[sp.size() - 2];
                        float len = vsg::length(dir);
                        if (len > 1e-6f) addArrowHead(fillTris, sp.back(), dir / len, headSize, lineColor);
                    }
                    if (ds.arrowType == ArrowType::Start || ds.arrowType == ArrowType::Both)
                    {
                        vsg::vec2 dir = sp.front() - sp[1];
                        float len = vsg::length(dir);
                        if (len > 1e-6f) addArrowHead(fillTris, sp.front(), dir / len, headSize, lineColor);
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
        for (size_t i = 0; i < marks.size(); ++i)
        {
            posSizeModeArray->set(i, marks[i].posSizeMode);
            colorArray->set(i, marks[i].color);
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
        for (size_t i = 0; i < lines.size(); ++i)
        {
            p0p1Array->set(i, lines[i].p0p1);
            widthDashArray->set(i, lines[i].widthDash);
            colorArray->set(i, lines[i].color);
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

        auto drawCommands = vsg::Commands::create();
        drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{quadCorners, p0p1Array, widthDashArray, colorArray}));
        drawCommands->addChild(vsg::BindIndexBuffer::create(quadIndices));
        drawCommands->addChild(vsg::DrawIndexed::create(6, static_cast<uint32_t>(lines.size()), 0, 0, 0));

        stateGroup->addChild(drawCommands);
        root->addChild(stateGroup);
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
        for (size_t i = 0; i < fillTris.size(); ++i)
        {
            posArray->set(i, fillTris[i].pos);
            colorArray->set(i, fillTris[i].color);
        }

        auto drawCommands = vsg::Commands::create();
        drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{posArray, colorArray}));
        drawCommands->addChild(vsg::Draw::create(static_cast<uint32_t>(fillTris.size()), 1, 0, 0));

        stateGroup->addChild(drawCommands);
        root->addChild(stateGroup);
    }

    if (textGroup->children.size() > 0) root->addChild(textGroup);

    return root;
}

} // namespace giv
