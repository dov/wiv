#include "GlyphSizeDialog.h"

#include "VulkanViewport.h"

#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace givqt
{

namespace
{
// Sliders are logarithmic: position = 100 * log10(scale), so equal drags give
// equal ratios and 1.0 sits in the middle-ish of the range.
int scaleToSlider(double scale) { return static_cast<int>(std::lround(100.0 * std::log10(scale))); }
double sliderToScale(int pos) { return std::pow(10.0, pos / 100.0); }

struct RowInfo
{
    const char* name;
    VulkanViewport::GlyphScale which;
};
constexpr RowInfo kRowInfo[] = {
    {"Mark size", VulkanViewport::GlyphScale::Marks},
    {"Line width", VulkanViewport::GlyphScale::Lines},
    {"Quiver length", VulkanViewport::GlyphScale::Quivers},
    {"Opacity", VulkanViewport::GlyphScale::Alpha},
};
} // namespace

GlyphSizeDialog::GlyphSizeDialog(VulkanViewport* viewport, QWidget* parent) : QDialog(parent), viewport_(viewport)
{
    setWindowTitle("Glyph Appearance");

    auto* grid = new QGridLayout();
    for (int i = 0; i < kRows; ++i)
    {
        const auto which = kRowInfo[i].which;
        sliders_[i] = new QSlider(Qt::Horizontal, this);
        sliders_[i]->setRange(scaleToSlider(VulkanViewport::glyphScaleMin(which)), scaleToSlider(VulkanViewport::glyphScaleMax(which)));
        sliders_[i]->setMinimumWidth(220);
        spins_[i] = new QDoubleSpinBox(this);
        spins_[i]->setRange(VulkanViewport::glyphScaleMin(which), VulkanViewport::glyphScaleMax(which));
        spins_[i]->setDecimals(2);
        spins_[i]->setSingleStep(0.1);
        spins_[i]->setPrefix(QString::fromUtf8("×"));
        auto* reset = new QPushButton("Reset", this);

        grid->addWidget(new QLabel(kRowInfo[i].name, this), i, 0);
        grid->addWidget(sliders_[i], i, 1);
        grid->addWidget(spins_[i], i, 2);
        grid->addWidget(reset, i, 3);

        connect(sliders_[i], &QSlider::valueChanged, this,
                [this, which](int pos) { viewport_->setGlyphScale(which, static_cast<float>(sliderToScale(pos))); });
        connect(spins_[i], qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, which](double v) { viewport_->setGlyphScale(which, static_cast<float>(v)); });
        connect(reset, &QPushButton::clicked, this, [this, which] { viewport_->setGlyphScale(which, 1.0f); });
    }

    auto* hint = new QLabel(
        "Mouse: Ctrl+left-drag up/down = mark size, left/right = quiver length;\n"
        "Ctrl+right-drag left/right = line width, up/down = opacity; Alt+wheel = quiver length.\n"
        "Opacity multiplies marks, lines and fills (Force Opaque overrides it).\n"
        "Shift speeds the drags up. Settings apply to the whole view and are not saved in the file.",
        this);
    hint->setWordWrap(true);

    auto* resetAll = new QPushButton("Reset All", this);
    connect(resetAll, &QPushButton::clicked, viewport_, &VulkanViewport::resetGlyphScales);
    auto* close = new QPushButton("Close", this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(resetAll);
    buttons->addStretch();
    buttons->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(grid);
    layout->addWidget(hint);
    layout->addLayout(buttons);

    connect(viewport_, &VulkanViewport::glyphScalesChanged, this, &GlyphSizeDialog::refresh);
    refresh();
}

void GlyphSizeDialog::refresh()
{
    for (int i = 0; i < kRows; ++i)
    {
        const double scale = viewport_->glyphScale(kRowInfo[i].which);
        // Blocked so that echoing a value back doesn't re-enter setGlyphScale
        // (and a spin box's 2-decimal rounding can't fight a finer drag value).
        QSignalBlocker b1(sliders_[i]);
        QSignalBlocker b2(spins_[i]);
        sliders_[i]->setValue(scaleToSlider(scale));
        spins_[i]->setValue(scale);
    }
}

} // namespace givqt
