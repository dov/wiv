// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// DisplayImage.h - renders the RGBA8 buffer actually uploaded to the GPU
// from an image's raw sample data, applying a linear contrast stretch and
// (optionally) a colormap LUT. Pure CPU, no GPU involvement - mirrors
// giv's giv_image_get_pixbuf()+apply_color_map() two-stage pipeline
// (~/hd/github/giv/src/givimage.cc, giv-win.gob). Used by VulkanViewport to
// recompute the displayed texture whenever the Contrast/Color Table tools
// change their settings; SceneBuilder itself is untouched and keeps
// uploading whatever rgba bytes it's handed.
//
#include "Colormaps.h"

#include <cstdint>
#include <vector>

namespace giv
{

struct LoadedImage;
class PixelBuffer;

// Renders `outRgba` (src.width*src.height*4 bytes) from src's raw
// samples: each native sample value is linearly stretched to [0,255] by
// (contrastMin, contrastMax) and clamped, then either replicated to gray
// (colormapEnabled == false) or looked up in colormaps::lut(colormapId).
// If src has no raw sample buffer (sampleType == WIV_SAMPLE_NONE, e.g. a
// color image), src.rgba is shared through unchanged (no copy) - contrast/colormap
// only apply to genuine single-channel sample data. `slice` (0..src.depth-1)
// selects which of src's stacked slices to render - see WivPluginImage::depth.
void renderDisplayRgba(const LoadedImage& src, float contrastMin, float contrastMax,
                        colormaps::Id colormapId, bool colormapEnabled, PixelBuffer& outRgba,
                        int slice = 0, float gamma = 1.0f);

// The 256*3 RGB table applied after the contrast stretch, or empty for a plain
// gray ramp. Equalize/Normalize have no fixed table: like giv, theirs comes from
// the contrast-stretched 8-bit image of `slice`, so it depends on the samples.
// `gamma` (>0, 1 = linear; >1 brightens midtones) is applied to the stretched level
// before the table lookup, so a non-unit gamma always yields a table (a gray ramp
// when there is no colormap).
std::vector<uint8_t> displayLut(const LoadedImage& src, float contrastMin, float contrastMax, colormaps::Id colormapId,
                                bool colormapEnabled, int slice, float gamma = 1.0f);

// The colour the display shows for sample `sampleIndex` of src.samples (an
// index into the whole slice-major buffer): the same stretch as
// renderDisplayRgba() and the GPU shader, for one sample. Packed so the low
// byte is R, then G, B, A (255).
uint32_t displayRgbaAt(const LoadedImage& src, float contrastMin, float contrastMax, colormaps::Id colormapId,
                       bool colormapEnabled, size_t sampleIndex, float gamma = 1.0f);

// $low_contrast: compresses RGBA bytes toward mid-gray (alpha untouched) so overlaid
// vectors stand out. One byte value -> the matching dimmed byte.
inline uint8_t dimByte(uint8_t v) { return static_cast<uint8_t>(64 + (v >> 1)); }
inline void dimRgba(const uint8_t* src, uint8_t* dst, size_t bytes)
{
    for (size_t i = 0; i + 3 < bytes; i += 4)
    {
        dst[i] = dimByte(src[i]);
        dst[i + 1] = dimByte(src[i + 1]);
        dst[i + 2] = dimByte(src[i + 2]);
        dst[i + 3] = src[i + 3];
    }
}

} // namespace giv
