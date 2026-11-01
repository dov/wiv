#pragma once
//
// ImagePluginHost.h - scans a directory for wiv image-format plugins
// (.so files conforming to src/plugins/wiv_plugin.h) and dispatches
// image loads to the first plugin that claims a given filename. Mirrors
// giv's own plugin loader (~/hd/github/giv/src/givplugin.cc), but with a
// deterministic (sorted-filename) load/dispatch order - giv's own docs
// (doc/plugins.txt) admit its order is arbitrary.
//
#include "Colormaps.h"
#include "plugins/wiv_plugin.h"

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giv
{

// An immutable, reference-counted byte buffer. Copying a LoadedImage - which
// happens every time one moves from the decode cache to the viewport and on
// to the texture - then costs a refcount instead of a copy of tens of
// megabytes. Never modified once built: write a new buffer and assign it.
class PixelBuffer
{
public:
    PixelBuffer() = default;
    PixelBuffer(std::vector<uint8_t>&& bytes) { *this = std::move(bytes); }
    PixelBuffer& operator=(std::vector<uint8_t>&& bytes)
    {
        buf_ = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        return *this;
    }
    void assign(const uint8_t* begin, const uint8_t* end) { buf_ = std::make_shared<const std::vector<uint8_t>>(begin, end); }

    const uint8_t* data() const { return buf_ ? buf_->data() : nullptr; }
    size_t size() const { return buf_ ? buf_->size() : 0; }
    bool empty() const { return size() == 0; }
    const uint8_t& operator[](size_t i) const { return (*buf_)[i]; }
    const std::vector<uint8_t>& vec() const
    {
        static const std::vector<uint8_t> kEmpty;
        return buf_ ? *buf_ : kEmpty;
    }
    // For handing the bytes to something that must keep them alive (a GPU texture's CPU-side data).
    const std::shared_ptr<const std::vector<uint8_t>>& owner() const { return buf_; }

private:
    std::shared_ptr<const std::vector<uint8_t>> buf_;
};

struct LoadedImage
{
    int width = 0;
    int height = 0;
    PixelBuffer rgba; // width*height*4, top-to-bottom row order

    // Optional raw single-channel sample buffer (see wiv_plugin.h) used by
    // the Contrast/Color Table tools. sampleType == WIV_SAMPLE_NONE for
    // images with no raw dynamic range beyond the RGBA8 preview above.
    WivSampleType sampleType = WIV_SAMPLE_NONE;
    PixelBuffer samples; // width*height*depth samples, slice-major
    float sampleMin = 0.0f; // full-volume scan, computed once at load
    float sampleMax = 0.0f;

    // Number of stacked 2D slices in `samples` (1 for an ordinary 2D
    // image) - see WivPluginImage::depth. VulkanViewport's currentSlice_
    // selects which slice of `samples` is rendered/reported on.
    int depth = 1;

    // Number of pages in the file this image was loaded from (1 unless the
    // plugin exports wiv_plugin_page_count, e.g. multi-page TIFF).
    int pageCount = 1;

    // Non-zero only for a reduced-resolution preview (see
    // ImagePluginHost::loadPreview()): the dimensions, after orientation, of
    // the full image this one stands in for. width/height are then those of
    // the preview itself; it is drawn stretched to fullWidth x fullHeight.
    int fullWidth = 0;
    int fullHeight = 0;
    bool isPreview() const { return fullWidth > 0; }

    // How a raw-sample image is shown (see SceneBuilder): the window, colormap
    // and slice the GPU shader applies to `samples`. Only the viewport sets it
    // (on its display copy); meaningless when sampleType == WIV_SAMPLE_NONE.
    struct DisplayParams
    {
        float min = 0.0f;
        float max = 0.0f;
        colormaps::Id colormap = colormaps::Id::None;
        bool colormapEnabled = false;
        int slice = 0;
    } display;

    // Set by the viewport while this image is being played as an animation:
    // the scene builder then gives it a texture the CPU may rewrite per frame
    // (rgba is the frame currently shown).
    bool animated = false;
};

// Every composited frame of an animation (GIF, APNG, animated WebP, AVIF
// sequence), decoded up front. Frames cover the whole canvas and are
// immutable, so a handle can be shared freely.
struct Animation
{
    int width = 0;
    int height = 0;
    int loopCount = 0; // plays through the frames; 0 = forever
    std::vector<PixelBuffer> frames;
    std::vector<int> delaysMs; // per frame, already clamped to something displayable
    size_t frameCount() const { return frames.size(); }
};

class ImagePluginHost
{
public:
    // True if `filename` has a suffix some plugin offering animation claims -
    // a cheap gate (no file access) before the real check, loadAnimation().
    static bool mayBeAnimated(const std::string& filename);

    // Decodes all frames of an animated file. nullopt-equivalent (nullptr) if
    // the file is not an animation (a still, or a single frame), could not be
    // decoded, was cancelled (`cancel`, as in load()), or would need more than
    // `maxBytes` of frame data - an animation is held in memory in full, so
    // that bounds it.
    static std::shared_ptr<const Animation> loadAnimation(const std::string& filename, size_t maxBytes,
                                                          const volatile int* cancel = nullptr);

    // Loads plugins (lazily, once) from WIV_PLUGIN_DIR (env var override)
    // or the compile-time default plugin directory, then attempts to load
    // `filename` with the first plugin that claims it (by suffix, else by
    // content sniffing - see isSupportedExplicit()). Returns nullopt and
    // prints a "wiv: ..." diagnostic to stderr on failure.
    // `page` (0-based) selects a page of a multi-page file; ignored by
    // plugins that have a single page.
    // `cancel`, if non-null, is polled (by plugins exporting
    // wiv_plugin_load_page_cancellable, and between host stages) and
    // abandons the load - returning nullopt without logging an error - once
    // it reads non-zero. May be set from any thread.
    static std::optional<LoadedImage> load(const std::string& filename, int page = 0,
                                           const volatile int* cancel = nullptr);

    // Cheap check ("does some plugin claim this filename") that never
    // decodes the file - used to build the $image cycling list without
    // paying for a full decode of every candidate up front.
    static bool isSupported(const std::string& filename);

    // A quick reduced-resolution stand-in for load(filename) - only for
    // plugins exporting wiv_plugin_load_preview, and only when the image is
    // big enough for that to pay off. Returns nullopt otherwise (the caller
    // should then do the full load). The result has isPreview() set.
    static std::optional<LoadedImage> loadPreview(const std::string& filename, int minLongSide = 800);

    // Like isSupported(), but falls back to content sniffing (opens the
    // file) for a name no plugin claims by suffix. Only for a file the user
    // named explicitly - never call from a directory scan.
    static bool isSupportedExplicit(const std::string& filename);

    // The format of `filename` as the plugins see it - the name of the plugin
    // that decodes it ("dicom", "tiff", "fits", ...), or empty if none does.
    // Remembered from every successful load(), so for a file that is or was on
    // screen this is a lookup; otherwise it asks the plugins (suffix, then a
    // content sniff that opens the file), so it also names files without an
    // extension. Thread-safe.
    static std::string formatOf(const std::string& filename);

    // The formats of all loaded plugins.
    static std::vector<std::string> formatNames();
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

    // Like get() but never decodes: nullptr on a miss. Counts as a use.
    const LoadedImage* peek(const std::string& path, int page = 0);

    // Drops every entry (any page) whose path is not in `keep`: the
    // sliding-window eviction of the prefetcher, so neighbours of the
    // current file never compete with stale, already-visited ones.
    void retain(const std::unordered_set<std::string>& keep);

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
