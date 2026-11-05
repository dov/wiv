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
    Equalize,  // histogram-equalized gray; the table depends on the image, see isAdaptive()
    Normalize, // gray stretched to the image's own min..max; likewise adaptive
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

// Equalize and Normalize have no fixed table: giv derives theirs from the
// contrast-stretched 8-bit image being displayed (histogram / min-max), so
// DisplayImage builds it per render. lut() returns the identity ramp for them
// (what a color bar or swatch shows).
bool isAdaptive(Id id);

// Returns a 256*3-byte RGB LUT (R,G,B interleaved per index) for `id`.
// None/Invert are generated on first use and cached; the rest are static
// data tables. The returned pointer is valid for the lifetime of the
// program.
const uint8_t* lut(Id id);

} // namespace giv::colormaps
