// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// RawImageWriter.h - File > Save Raw Data: writes an image's original sample values
// (8/16-bit integer or 32-bit float, before any contrast stretch or colour table) as a
// TIFF or NumPy .npy file. Pure C++, no Qt, so it can be unit tested.
//
#include <string>

namespace giv
{

struct LoadedImage;

enum class RawFormat
{
    Tiff, // uncompressed classic TIFF, one page per slice
    Npy,  // NumPy .npy v1.0, shape (H, W) or (slices, H, W)
};

struct RawSaveOptions
{
    RawFormat format = RawFormat::Tiff;
    bool allSlices = false; // false: only `slice`
    int slice = 0;
};

// True when `image` has raw samples that can be saved (not a colour image, not a preview).
bool hasRawSamples(const LoadedImage& image);

// Format chosen by a file name's suffix (.tif/.tiff, .npy); false if it is neither.
bool rawFormatFromPath(const std::string& path, RawFormat& format);

// Writes the samples to `path` (UTF-8). Returns false and sets *error on failure; a
// failed write removes the partial file.
bool writeRawImage(const std::string& path, const LoadedImage& image, const RawSaveOptions& options,
                   std::string* error = nullptr);

} // namespace giv
