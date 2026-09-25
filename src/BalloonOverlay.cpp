#include "BalloonOverlay.h"

#include <cstdio>
#include <future>
#include <sstream>

namespace giv
{

namespace
{

// giv's tooltip window background (giv_widget.gob:
// gdk_color_parse("yellow", &color)).
constexpr vsg::vec4 kBoxColor(1.0f, 1.0f, 0.0f, 0.9f);
constexpr vsg::vec4 kTextColor(0.0f, 0.0f, 0.0f, 1.0f);
constexpr float kPad = 6.0f;

vsg::ref_ptr<vsg::ShaderStage> loadShader(VkShaderStageFlagBits stage, const std::string& dir, const std::string& file)
{
    auto shader = vsg::ShaderStage::read(stage, "main", dir + "/" + file);
    if (!shader) throw std::runtime_error("failed to load shader: " + dir + "/" + file);
    return shader;
}

// Minimal flat-colored-triangle pipeline for the balloon's background box -
// same vertex layout/shaders as SceneBuilder's fill batch (posArray +
// colorArray, both per-vertex), just built standalone here since
// SceneBuilder's makePipeline() is private to that translation unit.
vsg::ref_ptr<vsg::StateGroup> makeBoxPipeline(const std::string& shaderDir)
{
    auto vertexShader = loadShader(VK_SHADER_STAGE_VERTEX_BIT, shaderDir, "fill.vert.spv");
    auto fragmentShader = loadShader(VK_SHADER_STAGE_FRAGMENT_BIT, shaderDir, "fill.frag.spv");

    vsg::PushConstantRanges pushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT, 0, 128}};

    auto rasterization = vsg::RasterizationState::create();
    rasterization->cullMode = VK_CULL_MODE_NONE;

    auto depthStencil = vsg::DepthStencilState::create();
    depthStencil->depthTestEnable = VK_FALSE;
    depthStencil->depthWriteEnable = VK_FALSE;

    auto colorBlend = vsg::ColorBlendState::create();
    colorBlend->configureAttachments(true);

    vsg::VertexInputState::Bindings bindings{
        VkVertexInputBindingDescription{0, sizeof(vsg::vec2), VK_VERTEX_INPUT_RATE_VERTEX},
        VkVertexInputBindingDescription{1, sizeof(vsg::vec4), VK_VERTEX_INPUT_RATE_VERTEX}};
    vsg::VertexInputState::Attributes attributes{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};

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

// Resolves a plain sans-serif font via fontconfig, independent of
// SceneBuilder::resolveFont() (private/per-dataset there) - the balloon
// always uses one fixed font.
// Building a glyph atlas for an entire system "Sans" font (typically a
// large, many-thousand-glyph file like NotoSans-Regular.ttf) via freetype
// takes on the order of ~2 seconds. This used to be paid synchronously on
// the render thread the first time BalloonOverlay::show() ran (i.e. as a
// ~1-2s stall the first time a user pressed 'b' and hovered over data) -
// noticeably laggy in practice. Instead, kick the resolution off on a
// background thread as soon as the first BalloonOverlay exists (well
// before the balloon feature could plausibly be used), cached for the
// lifetime of the process (keyed on nothing - the balloon always resolves
// the same fixed "Sans" query) via a shared_future so every BalloonOverlay
// instance (one per VulkanViewport::loadFiles() call) reuses the same
// in-flight or completed resolution. show() only blocks on it if the
// background resolution genuinely hasn't finished yet by the time it's
// needed.
std::shared_future<vsg::ref_ptr<vsg::Font>>& balloonFontFuture(vsg::ref_ptr<vsg::Options> options)
{
    static std::shared_future<vsg::ref_ptr<vsg::Font>> future = std::async(std::launch::async, [options]() -> vsg::ref_ptr<vsg::Font> {
                                                                     std::string path;
                                                                     if (FILE* p = popen("fc-match -f '%{file}' Sans 2>/dev/null", "r"))
                                                                     {
                                                                         char buf[1024] = {0};
                                                                         if (fgets(buf, sizeof(buf), p)) path = buf;
                                                                         pclose(p);
                                                                     }
                                                                     if (path.empty()) return {};
                                                                     return vsg::read_cast<vsg::Font>(path, options);
                                                                 }).share();
    return future;
}

} // namespace

BalloonOverlay::BalloonOverlay(vsg::ref_ptr<vsg::Options> options, const std::string& shaderDir) :
    options_(options), shaderDir_(shaderDir)
{
    // Kick off font resolution on a background thread now (see
    // balloonFontFuture's doc comment) - font_ itself is only assigned
    // lazily on first show(), once the (by then likely-already-finished)
    // background resolution is available.
    balloonFontFuture(options_);

    // Screen-space HUD camera: window-pixel coordinates directly (origin
    // top-left, y-down), matching vsg::MoveEvent's x/y and giv's own pixel
    // math for balloon placement 1:1 - see updateExtent()/show().
    auto projection = vsg::Orthographic::create(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);
    auto lookAt = vsg::LookAt::create(vsg::dvec3(0.0, 0.0, 1.0), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 1.0, 0.0));
    camera_ = vsg::Camera::create(projection, lookAt, vsg::ViewportState::create(VkExtent2D{1, 1}));

    switch_ = vsg::Switch::create();

    auto boxPipeline = makeBoxPipeline(shaderDir_);
    boxPos_ = vsg::vec2Array::create(6);
    boxPos_->properties.dataVariance = vsg::DYNAMIC_DATA;
    auto boxColor = vsg::vec4Array::create(6);
    for (int i = 0; i < 6; ++i) boxColor->set(i, kBoxColor);

    auto boxDraw = vsg::Commands::create();
    boxDraw->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{boxPos_, boxColor}));
    boxDraw->addChild(vsg::Draw::create(6, 1, 0, 0));
    boxPipeline->addChild(boxDraw);

    textTransform_ = vsg::MatrixTransform::create();
    textTransform_->addChild(vsg::Group::create()); // placeholder, replaced by rebuildContent()'s Text node

    content_ = vsg::Group::create();
    content_->addChild(boxPipeline);   // children[0]
    content_->addChild(textTransform_); // children[1]

    // Added enabled, not disabled: the single viewer->compile() call in
    // main.cpp only visits/compiles a Switch child whose mask is on at that
    // moment (see LabelPicker::rebuild()'s doc comment for the same issue
    // there) - main.cpp calls hide() right after compiling to start hidden.
    switch_->addChild(true, content_);

    view_ = vsg::View::create(camera_);
    view_->addChild(switch_);
}

void BalloonOverlay::updateExtent(const VkExtent2D& extent)
{
    if (extent.width == 0 || extent.height == 0) return;

    // Unconditionally re-assert the exact pixel-space bounds every frame
    // (cheap) rather than only on change: the shared RenderGraph's default
    // WindowResizeHandler also rescales every View's camera - including
    // this HUD one - on a detected window resize (via
    // Orthographic::changeExtent(), which multiplies left/right by the new/
    // old aspect ratio), which would otherwise silently desync this
    // camera's exact left=0/right=width pixel mapping from the real window
    // size after a resize.
    auto ortho = camera_->projectionMatrix.cast<vsg::Orthographic>();
    ortho->left = 0.0;
    ortho->right = static_cast<double>(extent.width);
    ortho->bottom = static_cast<double>(extent.height);
    ortho->top = 0.0;

    if (extent.width != extent_.width || extent.height != extent_.height)
    {
        extent_ = extent;
        camera_->viewportState = vsg::ViewportState::create(extent);
    }
}

void BalloonOverlay::hide()
{
    visible_ = false;
    switch_->setAllChildren(false);
}

void BalloonOverlay::show(vsg::Viewer* viewer, const std::string& text, int32_t x, int32_t y)
{
    if (!font_) font_ = balloonFontFuture(options_).get();

    if (!visible_ || text != currentText_)
    {
        rebuildContent(viewer, text, x, y);
        currentText_ = text;
    }
    else
    {
        // Same text, just follow the cursor: reposition the box's vertices
        // and the text's MatrixTransform (see textTransform_'s doc comment
        // in the header for why the Text node itself is never touched here).
        float bx = static_cast<float>(x) + 20.0f;
        float by = static_cast<float>(y) - 20.0f;
        // Recompute box extent from the existing quad's size.
        float w = boxPos_->at(2).x - boxPos_->at(0).x;
        float h = boxPos_->at(2).y - boxPos_->at(0).y;
        boxPos_->set(0, {bx, by});
        boxPos_->set(1, {bx + w, by});
        boxPos_->set(2, {bx + w, by + h});
        boxPos_->set(3, {bx + w, by + h});
        boxPos_->set(4, {bx, by + h});
        boxPos_->set(5, {bx, by});
        boxPos_->dirty();

        textTransform_->matrix = vsg::translate(static_cast<double>(bx), static_cast<double>(by), 0.0);
    }

    visible_ = true;
    switch_->setAllChildren(true);
}

void BalloonOverlay::rebuildContent(vsg::Viewer* viewer, const std::string& text, int32_t x, int32_t y)
{
    // Rough size estimate (giv relies on GTK's own label auto-sizing here;
    // wiv has no text-measurement pass, so approximate from character/line
    // counts instead - good enough for a hover tooltip).
    std::vector<std::string> lines;
    {
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) lines.push_back(line);
        if (lines.empty()) lines.push_back(text);
    }
    size_t maxLen = 1;
    for (const auto& l : lines) maxLen = std::max(maxLen, l.size());

    float charW = static_cast<float>(fontSize_) * 0.6f;
    float lineH = static_cast<float>(fontSize_) * 1.3f;
    float textW = static_cast<float>(maxLen) * charW;
    float textH = static_cast<float>(lines.size()) * lineH;

    float bx = static_cast<float>(x) + 20.0f;
    float by = static_cast<float>(y) - 20.0f;
    float w = textW + 2.0f * kPad;
    float h = textH + 2.0f * kPad;

    boxPos_->set(0, {bx, by});
    boxPos_->set(1, {bx + w, by});
    boxPos_->set(2, {bx + w, by + h});
    boxPos_->set(3, {bx + w, by + h});
    boxPos_->set(4, {bx, by + h});
    boxPos_->set(5, {bx, by});
    boxPos_->dirty();

    textTransform_->matrix = vsg::translate(static_cast<double>(bx), static_cast<double>(by), 0.0);

    if (font_)
    {
        auto layout = vsg::StandardLayout::create();
        layout->horizontalAlignment = vsg::StandardLayout::LEFT_ALIGNMENT;
        layout->verticalAlignment = vsg::StandardLayout::TOP_ALIGNMENT;
        // Local to textTransform_, which carries (bx,by) - see this
        // function's first line and textTransform_'s doc comment in the
        // header: vsg::Text's default CpuLayoutTechnique bakes
        // layout->position into vertex data at setup() time and has no
        // supported way to reposition it afterward, so the *only* place
        // this text can be cheaply moved from is the wrapping transform.
        layout->position = vsg::vec3(kPad, kPad, 0.0f);
        float fsize = static_cast<float>(fontSize_);
        layout->horizontal = vsg::vec3(fsize, 0.0f, 0.0f);
        // Negated: the HUD camera's world space is y-down (see the
        // constructor), so "up" (the glyph ascender direction) is -Y here.
        layout->vertical = vsg::vec3(0.0f, -fsize, 0.0f);
        layout->color = kTextColor;

        auto textNode = vsg::Text::create();
        textNode->text = vsg::stringValue::create(text);
        textNode->font = font_;
        textNode->layout = layout;
        textNode->setup(0, options_);

        // This node is spliced into the already-compiled live scene graph
        // below, after the viewer's one-time startup compile() - it needs
        // its own compile pass (font-atlas descriptor set, vertex buffers)
        // before RecordTraversal touches it next frame, same issue as
        // LabelPicker::rebuild()'s doc comment.
        if (viewer && viewer->compileManager)
        {
            auto result = viewer->compileManager->compile(textNode);
            vsg::updateViewer(*viewer, result);
        }

        textTransform_->children[0] = textNode;
    }
}

} // namespace giv
