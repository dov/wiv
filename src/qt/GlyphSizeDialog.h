#pragma once
//
// GlyphSizeDialog.h - View > Glyph Appearance: a small non-modal panel showing and
// editing the viewport's global glyph factors (mark size, line width,
// quiver length, opacity - VulkanViewport::GlyphScale). It makes the Ctrl+left-drag
// (vertical: marks, horizontal: quiver), Ctrl+right-drag (horizontal: line
// width, vertical: opacity) and Alt+wheel (quiver) gestures discoverable and undoable. Live like
// ContrastDialog: the factors are shader uniforms, so every edit applies at
// once, and the panel follows gesture changes via glyphScalesChanged().
//
#include <QDialog>

class QDoubleSpinBox;
class QSlider;

namespace givqt
{

class VulkanViewport;

class GlyphSizeDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit GlyphSizeDialog(VulkanViewport* viewport, QWidget* parent = nullptr);

private:
    static constexpr int kRows = 4;
    VulkanViewport* viewport_ = nullptr;
    QSlider* sliders_[kRows] = {};
    QDoubleSpinBox* spins_[kRows] = {};

    void refresh();
};

} // namespace givqt
