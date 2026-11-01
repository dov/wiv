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
    std::vector<uint8_t> samples; // width*height*depth samples, slice-major
    float sampleMin = 0.0f; // full-volume scan, computed once at load
    float sampleMax = 0.0f;

    // Number of stacked 2D slices in `samples` (1 for an ordinary 2D
    // image) - see WivPluginImage::depth. VulkanViewport's currentSlice_
    // selects which slice of `samples` is rendered/reported on.
    int depth = 1;

    // Number of pages in the file this image was loaded from (1 unless the
    // plugin exports wiv_plugin_page_count, e.g. multi-page TIFF).
    int pageCount = 1;
};

class ImagePluginHost
{
public:
    // Loads plugins (lazily, once) from WIV_PLUGIN_DIR (env var override)
    // or the compile-time default plugin directory, then attempts to load
    // `filename` with the first plugin that claims it (by suffix, else by
    // content sniffing - see isSupportedExplicit()). Returns nullopt and
    // prints a "wiv: ..." diagnostic to stderr on failure.
    // `page` (0-based) selects a page of a multi-page file; ignored by
    // plugins that have a single page.
    static std::optional<LoadedImage> load(const std::string& filename, int page = 0);

    // Cheap check ("does some plugin claim this filename") that never
    // decodes the file - used to build the $image cycling list without
    // paying for a full decode of every candidate up front.
    static bool isSupported(const std::string& filename);

    // Like isSupported(), but falls back to content sniffing (opens the
    // file) for a name no plugin claims by suffix. Only for a file the user
    // named explicitly - never call from a directory scan.
    static bool isSupportedExplicit(const std::string& filename);
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
    const LoadedImage* get(const std::string& path, int page = 0);

    // Inserts an image decoded elsewhere (e.g. by a background prefetch) as
    // the least-recently-used entry, so it never evicts what is on screen.
    // No-op if `path` is already cached.
    void put(const std::string& path, int page, LoadedImage image);
    bool contains(const std::string& path, int page = 0) const;

private:
    static std::string keyFor(const std::string& path, int page)
    {
        return page > 0 ? path + '\n' + std::to_string(page) : path;
    }
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
