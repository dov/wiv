#pragma once
//
// Colormaps.h - predefined 256x3 RGB color-table LUTs, ported verbatim from
// giv's ~/hd/github/giv/src/colormaps.cc, plus None/Invert generated
// procedurally. Used by the Color Table tool to recolor a contrast-stretched
// grayscale image (see DisplayImage.h).
//
#include <cstdint>

namespace giv::colormaps
{

enum class Id
{
    None = 0,
    Invert,
    LowContrast,
    Rainbow,
    RedTemperature,
    BlueGreenRedYellow,
    BlueWhite,
    GreenRedBlueWhite,
    Count
};

// Display name for a menu entry, matching giv's own labels.
const char* name(Id id);

// Returns a 256*3-byte RGB LUT (R,G,B interleaved per index) for `id`.
// None/Invert are generated on first use and cached; the rest are static
// data tables. The returned pointer is valid for the lifetime of the
// program.
const uint8_t* lut(Id id);

} // namespace giv::colormaps
