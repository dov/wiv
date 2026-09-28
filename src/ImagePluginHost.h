#pragma once
//
// ImagePluginHost.h - scans a directory for wiv image-format plugins
// (.so files conforming to src/plugins/wiv_plugin.h) and dispatches
// image loads to the first plugin that claims a given filename. Mirrors
// giv's own plugin loader (~/hd/github/giv/src/givplugin.cc), but with a
// deterministic (sorted-filename) load/dispatch order - giv's own docs
// (doc/plugins.txt) admit its order is arbitrary.
//
#include "plugins/wiv_plugin.h"

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace giv
{

struct LoadedImage
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width*height*4, top-to-bottom row order

    // Optional raw single-channel sample buffer (see wiv_plugin.h) used by
    // the Contrast/Color Table tools. sampleType == WIV_SAMPLE_NONE for
    // images with no raw dynamic range beyond the RGBA8 preview above.
    WivSampleType sampleType = WIV_SAMPLE_NONE;
    std::vector<uint8_t> samples;
    float sampleMin = 0.0f; // full-image scan, computed once at load
    float sampleMax = 0.0f;
};

class ImagePluginHost
{
public:
    // Loads plugins (lazily, once) from WIV_PLUGIN_DIR (env var override)
    // or the compile-time default plugin directory, then attempts to load
    // `filename` with the first plugin that claims it. Returns nullopt and
    // prints a "wiv: ..." diagnostic to stderr on failure.
    static std::optional<LoadedImage> load(const std::string& filename);

    // Cheap check ("does some plugin claim this filename") that never
    // decodes the file - used to build the $image cycling list without
    // paying for a full decode of every candidate up front.
    static bool isSupported(const std::string& filename);
};

// Bounded decode cache for $image cycling (giv's shift-Up/shift-Down):
// wiv only ever displays one image at a time, but a user paging back and
// forth through a folder shouldn't re-decode the same handful of images
// over and over, so the last `capacity` decodes are kept around. Evicts
// least-recently-used on overflow. Not thread-safe - only ever touched from
// the Qt GUI thread.
class ImageCache
{
public:
    explicit ImageCache(size_t capacity) : capacity_(capacity) {}

    // Returns the decoded image for `path`, decoding (and caching) it on a
    // miss. Returns nullptr if no plugin can decode it. The returned
    // pointer is only valid until the next get() call (a subsequent miss
    // may evict it).
    const LoadedImage* get(const std::string& path);

private:
    size_t capacity_;
    std::list<std::string> lru_; // front = most recently used
    struct Entry
    {
        LoadedImage image;
        std::list<std::string>::iterator lruIt;
    };
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace giv
