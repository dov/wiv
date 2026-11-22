// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// IccProfile.h - embedded ICC colour profiles: reading them out of JPEG, PNG, TIFF and WebP files,
// and converting RGBA8 pixels from such a profile to sRGB with Little CMS (when built with it).
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace giv
{

// The raw ICC profile embedded in `path` (UTF-8), found by content, not extension. Empty when the
// file has none or is of another container.
std::vector<uint8_t> readIccProfile(const std::string& path);

// True when this build can convert (Little CMS was found).
bool iccConversionAvailable();

// The profile's description tag ("sRGB IEC61966-2.1", "Display P3", ...), or "" if unreadable.
std::string iccDescription(const std::vector<uint8_t>& profile);

// True when converting from `profile` would change pixels: an RGB profile that is not sRGB.
bool iccConversionNeeded(const std::vector<uint8_t>& profile);

// Converts `pixelCount` RGBA8 pixels (alpha untouched) from `profile` to sRGB, in place, using the
// perceptual intent. Does nothing and returns false when the profile is not an RGB profile, is
// unusable, is (to within rounding) sRGB already, or conversion is not built in.
bool convertRgba8ToSrgb(const std::vector<uint8_t>& profile, uint8_t* rgba, size_t pixelCount);

} // namespace giv
