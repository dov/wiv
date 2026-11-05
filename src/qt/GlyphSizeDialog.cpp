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
#include <QWidget>

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
    const char* tooltip;
    const char* gesture; // shown in the help table
    const char* gestureTip; // spelled-out version, tooltip of the gesture
};
constexpr RowInfo kRowInfo[] = {
    {"Mark size", VulkanViewport::GlyphScale::Marks,
     "Multiplies the size of all marks (circles, squares, pixels).\nCtrl+left-drag up/down.",
     "Ctrl + LMB drag up/down",
     "Hold Ctrl, press the left mouse button (LMB) and drag up or down."},
    {"Line width", VulkanViewport::GlyphScale::Lines,
     "Multiplies the thickness of all lines, polygon outlines and quiver shafts.\nCtrl+right-drag left/right.",
     "Ctrl + RMB drag left/right",
     "Hold Ctrl, press the right mouse button (RMB) and drag left or right."},
    {"Quiver length", VulkanViewport::GlyphScale::Quivers,
     "Multiplies the length of all quiver arrows.\nCtrl+left-drag left/right, or Alt+mouse wheel.",
     "Ctrl + LMB drag left/right, or Alt + wheel",
     "Hold Ctrl, press the left mouse button (LMB) and drag left or right. Alternatively hold Alt and turn the mouse wheel."},
    {"Opacity", VulkanViewport::GlyphScale::Alpha,
     "Multiplies the opacity of all marks, lines and fills. Values above 1 make translucent glyphs more solid.\n"
     "Ctrl+right-drag up/down. Force Opaque overrides it.",
     "Ctrl + RMB drag up/down",
     "Hold Ctrl, press the right mouse button (RMB) and drag up or down."},
};

bool rowUsed(int i, const VulkanViewport::GlyphUsage& u)
{
    switch (kRowInfo[i].which)
    {
    case VulkanViewport::GlyphScale::Marks: return u.marks;
    case VulkanViewport::GlyphScale::Lines: return u.lines || u.quivers;
    case VulkanViewport::GlyphScale::Quivers: return u.quivers;
    case VulkanViewport::GlyphScale::Alpha: return true;
    }
    return true;
}
} // namespace

GlyphSizeDialog::GlyphSizeDialog(VulkanViewport* viewport, QWidget* parent) : QDialog(parent), viewport_(viewport)
{
    setWindowTitle("Glyph Appearance");

    auto* title = new QLabel("Adjust the size and opacity of the marks, lines and arrows drawn in this view.", this);
    title->setWordWrap(true);

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

        labels_[i] = new QLabel(kRowInfo[i].name, this);
        resets_[i] = reset;
        for (QWidget* w : {static_cast<QWidget*>(labels_[i]), static_cast<QWidget*>(sliders_[i]),
                           static_cast<QWidget*>(spins_[i]), static_cast<QWidget*>(reset)})
            w->setToolTip(kRowInfo[i].tooltip);

        grid->addWidget(labels_[i], i, 0);
        grid->addWidget(sliders_[i], i, 1);
        grid->addWidget(spins_[i], i, 2);
        grid->addWidget(reset, i, 3);

        connect(sliders_[i], &QSlider::valueChanged, this,
                [this, which](int pos) { viewport_->setGlyphScale(which, static_cast<float>(sliderToScale(pos))); });
        connect(spins_[i], qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, which](double v) { viewport_->setGlyphScale(which, static_cast<float>(v)); });
        connect(reset, &QPushButton::clicked, this, [this, which] { viewport_->setGlyphScale(which, 1.0f); });
    }

    help_ = new QWidget(this);
    auto* helpGrid = new QGridLayout(help_);
    helpGrid->setContentsMargins(0, 0, 0, 0);
    helpGrid->setHorizontalSpacing(14);
    helpGrid->setColumnStretch(2, 1); // empty glue column: keeps the table left-aligned, not stretched
    auto* helpTitle = new QLabel("<b>Mouse shortcuts</b> (hold Shift to drag faster)", help_);
    helpGrid->addWidget(helpTitle, 0, 0, 1, 2);
    const char* headers[] = {"Setting", "Gesture"};
    for (int c = 0; c < 2; ++c) helpGrid->addWidget(new QLabel(QString("<i>%1</i>").arg(headers[c]), help_), 1, c);
    for (int i = 0; i < kRows; ++i)
    {
        helpRows_[i][0] = new QLabel(QString("<b>%1</b>").arg(kRowInfo[i].name), help_);
        helpRows_[i][1] = new QLabel(kRowInfo[i].gesture, help_);
        helpRows_[i][1]->setToolTip(kRowInfo[i].gestureTip);
        for (int c = 0; c < 2; ++c) helpGrid->addWidget(helpRows_[i][c], 2 + i, c);
    }

    emptyNote_ = new QLabel("No marks, lines or arrows in the current view.", this);
    emptyNote_->setWordWrap(true);

    auto* resetAll = new QPushButton("Reset All", this);
    connect(resetAll, &QPushButton::clicked, viewport_, &VulkanViewport::resetGlyphScales);
    auto* close = new QPushButton("Close", this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(resetAll);
    buttons->addStretch();
    buttons->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addLayout(grid);
    layout->addWidget(emptyNote_);
    layout->addWidget(help_);
    layout->addLayout(buttons);

    connect(viewport_, &VulkanViewport::glyphScalesChanged, this, &GlyphSizeDialog::refresh);
    connect(viewport_, &VulkanViewport::glyphUsageChanged, this, &GlyphSizeDialog::updateVisibleRows);
    refresh();
    updateVisibleRows();
}

void GlyphSizeDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    updateVisibleRows();
}

void GlyphSizeDialog::updateVisibleRows()
{
    const auto usage = viewport_->glyphUsage();
    bool anyGlyph = false;
    for (int i = 0; i < kRows; ++i)
    {
        const bool used = rowUsed(i, usage);
        const bool isOpacity = kRowInfo[i].which == VulkanViewport::GlyphScale::Alpha;
        labels_[i]->setVisible(used);
        sliders_[i]->setVisible(used);
        spins_[i]->setVisible(used);
        resets_[i]->setVisible(used);
        if (used && !isOpacity) anyGlyph = true;
        for (QLabel* l : helpRows_[i]) l->setVisible(used);
    }
    // With nothing drawn, even opacity has nothing to act on.
    if (!anyGlyph)
        for (int i = 0; i < kRows; ++i)
        {
            labels_[i]->hide();
            sliders_[i]->hide();
            spins_[i]->hide();
            resets_[i]->hide();
            for (QLabel* l : helpRows_[i]) l->hide();
        }
    emptyNote_->setVisible(!anyGlyph);
    help_->setVisible(anyGlyph);
    adjustSize();
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
