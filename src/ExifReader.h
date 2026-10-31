#pragma once
//
// ExifReader.h - a small, dependency-free EXIF reader for the Image Info
// panel. Reads the metadata straight from the file (independent of the image
// plugins, which only extract the Orientation tag) and returns a curated,
// human-formatted list: camera/image, exposure, and GPS.
//
// Supported containers (detected by content, not extension): JPEG (APP1
// "Exif"), TIFF/DNG-style TIFF (IFD0 + Exif + GPS IFDs), PNG (eXIf chunk) and
// WebP (EXIF chunk). Anything else, or a file with no EXIF, yields an empty
// list.
//
#include <string>
#include <vector>

namespace giv
{

struct ExifEntry
{
    std::string name;  // e.g. "Camera model", "Exposure time"
    std::string value; // already formatted, e.g. "1/250 s", "f/2.8"
};

std::vector<ExifEntry> readExif(const std::string& path);

} // namespace giv
