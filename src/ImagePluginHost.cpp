#include "ImagePluginHost.h"

#include "plugins/wiv_plugin.h"
#include "plugins/wiv_plugin_common.h"
#include "AppPaths.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define dlopen(p, f) static_cast<void*>(LoadLibraryExA(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
#define dlsym(h, s) reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), s))
#define dlclose(h) FreeLibrary(static_cast<HMODULE>(h))
#define dlerror() ("LoadLibrary failed, error " + std::to_string(GetLastError())).c_str()
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "PathUtils.h"
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>

namespace giv
{

namespace
{
#ifdef _WIN32
constexpr const char* kPluginExt = ".dll";
// Dependency DLLs (tiff.dll, ...) are copied next to the plugins; only load ours.
bool isPluginName(const std::string& stem) { return stem.rfind("wiv_", 0) == 0; }
#else
constexpr const char* kPluginExt = ".so";
bool isPluginName(const std::string&) { return true; }
#endif


namespace fs = std::filesystem;

using SupportsFileFn = bool (*)(const char*);
using SniffFileFn = bool (*)(const char*);
using LoadImageFn = WivPluginImage* (*)(const char*, char**);
using PageCountFn = int (*)(const char*);
using LoadPageFn = WivPluginImage* (*)(const char*, int, char**);
using LoadPreviewFn = WivPluginImage* (*)(const char*, int, int*, int*, char**);
using LoadCancellableFn = WivPluginImage* (*)(const char*, int, const volatile int*, char**);
using AnimOpenFn = void* (*)(const char*, WivAnimInfo*);
using AnimNextFn = bool (*)(void*, unsigned char*, int*);
using AnimCloseFn = void (*)(void*);
using FreeImageFn = void (*)(WivPluginImage*);
using FreeErrorFn = void (*)(char*);

struct Plugin
{
    std::string path;
    std::string format; // short name of the file format: the library's name ("dicom", "tiff", ...)
    void* handle = nullptr;
    SupportsFileFn supportsFile = nullptr;
    SniffFileFn sniffFile = nullptr; // optional
    LoadImageFn loadImage = nullptr;
    PageCountFn pageCount = nullptr; // optional, with loadPage
    LoadPageFn loadPage = nullptr;
    LoadCancellableFn loadCancellable = nullptr; // optional
    LoadPreviewFn loadPreview = nullptr;         // optional
    AnimOpenFn animOpen = nullptr;               // optional, all three or none
    AnimNextFn animNext = nullptr;
    AnimCloseFn animClose = nullptr;
    FreeImageFn freeImage = nullptr;
    FreeErrorFn freeError = nullptr;
};

// Reorders a row-major, top-to-bottom buffer of `elemSize`-byte elements
// (w*h elements total) per the rotation/flip implied by an EXIF/TIFF
// Orientation tag (1-8; see the Exif spec's table). Shared by both the
// RGBA8 preview (elemSize=4) and the optional raw sample buffer
// (elemSize=1/2/4, see WivSampleType) so every plugin can hand back raw
// sensor-order pixels plus an orientation value instead of each
// reimplementing this. Returns the buffer unchanged (by value-move) if
// `orientation` is 1 ("normal") or out of range.
std::vector<uint8_t> reorderBuffer(const std::vector<uint8_t>& in, int w, int h, size_t elemSize,
                                    int orientation)
{
    if (orientation <= 1 || orientation > 8 || in.empty())
        return in;

    const bool swapDims = orientation >= 5;
    const int newW = swapDims ? h : w;
    const int newH = swapDims ? w : h;

    std::vector<uint8_t> out(static_cast<size_t>(newW) * newH * elemSize);
    auto srcElem = [&](int x, int y) { return &in[(static_cast<size_t>(y) * w + x) * elemSize]; };

    for (int y = 0; y < newH; ++y)
    {
        for (int x = 0; x < newW; ++x)
        {
            int sx, sy;
            switch (orientation)
            {
                case 2: sx = w - 1 - x; sy = y; break;                 // mirror horizontal
                case 3: sx = w - 1 - x; sy = h - 1 - y; break;         // rotate 180
                case 4: sx = x; sy = h - 1 - y; break;                 // mirror vertical
                case 5: sx = y; sy = x; break;                        // transpose
                case 6: sx = y; sy = h - 1 - x; break;                 // rotate 90 CW
                case 7: sx = w - 1 - y; sy = h - 1 - x; break;         // transverse
                default: sx = w - 1 - y; sy = x; break;                // 8: rotate 270 CW
            }
            std::memcpy(&out[(static_cast<size_t>(y) * newW + x) * elemSize], srcElem(sx, sy), elemSize);
        }
    }

    return out;
}

void applyOrientation(LoadedImage& image, int orientation)
{
    if (orientation <= 1 || orientation > 8)
        return;

    // Multi-slice volumes (npy/FITS/DICOM stacks) have no EXIF/TIFF
    // orientation concept - none of those plugins set anything but 1, so
    // this is just a safety net against reorderBuffer() misinterpreting a
    // multi-slice sample buffer as a single width*height*elemSize one.
    if (image.depth > 1)
        return;

    const int w = image.width;
    const int h = image.height;
    if (!image.samples.empty())
        image.samples = reorderBuffer(image.samples.vec(), w, h, wiv_plugin::sampleTypeSize(image.sampleType), orientation);
    image.rgba = reorderBuffer(image.rgba.vec(), w, h, 4, orientation);

    const bool swapDims = orientation >= 5;
    if (swapDims)
        std::swap(image.width, image.height);
}

// Reads flat sample `idx` (0..width*height*depth) as a double, regardless
// of sampleType - mirrors giv's giv_image_get_value().
double sampleValue(const LoadedImage& image, size_t idx)
{
    switch (image.sampleType)
    {
        case WIV_SAMPLE_U8:
            return image.samples[idx];
        case WIV_SAMPLE_U16:
            return reinterpret_cast<const uint16_t*>(image.samples.data())[idx];
        case WIV_SAMPLE_FLOAT:
            return reinterpret_cast<const float*>(image.samples.data())[idx];
        default:
            return 0.0;
    }
}

// Full linear scan (over every slice) for the image's native-value min/max -
// mirrors giv's giv_image_get_min_max() (no shortcuts/caching there either).
void computeSampleMinMax(LoadedImage& image)
{
    if (image.samples.empty())
        return;

    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    const size_t count = static_cast<size_t>(image.width) * image.height * image.depth;
    for (size_t idx = 0; idx < count; ++idx)
    {
        double v = sampleValue(image, idx);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }

    image.sampleMin = static_cast<float>(lo);
    image.sampleMax = static_cast<float>(hi);
}

// "dicom" from ".../dicom.so", "wiv_dicom.dll" or a debug build's "dicomd.so".
std::string formatNameOf(const std::string& libraryPath)
{
    std::string name = giv::pathToUtf8(fs::path(libraryPath).stem());
    if (name.rfind("wiv_", 0) == 0)
        name.erase(0, 5);
#ifndef NDEBUG
    if (name.size() > 1 && name.back() == 'd') // debug postfix
        name.pop_back();
#endif
    return name;
}

// The format each file was last recognised as, remembered from loads (which
// run on prefetch threads too) so that asking later costs no file access.
std::mutex& formatCacheMutex()
{
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, std::string>& formatCache()
{
    static std::unordered_map<std::string, std::string> cache;
    return cache;
}
void rememberFormat(const std::string& filename, const std::string& format)
{
    std::lock_guard<std::mutex> lock(formatCacheMutex());
    formatCache()[filename] = format;
}

std::vector<Plugin> scanPlugins()
{
    std::vector<Plugin> plugins;

    const char* dirOverride = std::getenv("WIV_PLUGIN_DIR");
    std::string dir = dirOverride ? std::string(dirOverride) : resourceDir("plugins", "lib/wiv/plugins", WIV_PLUGIN_DIR);

    std::error_code ec;
    if (!fs::is_directory(dir, ec))
    {
        spdlog::error("Plugin directory not found: {}", dir);
        return plugins;
    }

    std::vector<std::string> candidates;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (entry.path().extension() == kPluginExt && isPluginName(giv::pathToUtf8(entry.path().stem())))
            candidates.push_back(giv::pathToUtf8(entry.path()));
    }
    std::sort(candidates.begin(), candidates.end());

#ifdef _WIN32
    // Plugins live in lib/wiv/plugins but their dependency DLLs (cfitsio,
    // libtiff, ...) sit next to wiv.exe in bin/; LOAD_WITH_ALTERED_SEARCH_PATH
    // only searches the plugin's own directory, so add the exe directory.
    {
        wchar_t exePath[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            const fs::path exeDir = fs::path(exePath).parent_path();
            SetDllDirectoryW(exeDir.c_str());
        }
    }
#endif

    for (const auto& path : candidates)
    {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            spdlog::error("Failed to load plugin {}: {}", path, dlerror());
            continue;
        }

        Plugin plugin;
        plugin.path = path;
        plugin.format = formatNameOf(path);
        plugin.handle = handle;
        plugin.supportsFile = reinterpret_cast<SupportsFileFn>(dlsym(handle, "wiv_plugin_supports_file"));
        plugin.sniffFile = reinterpret_cast<SniffFileFn>(dlsym(handle, "wiv_plugin_sniff_file"));
        plugin.loadImage = reinterpret_cast<LoadImageFn>(dlsym(handle, "wiv_plugin_load_image"));
        plugin.pageCount = reinterpret_cast<PageCountFn>(dlsym(handle, "wiv_plugin_page_count"));
        plugin.loadPage = reinterpret_cast<LoadPageFn>(dlsym(handle, "wiv_plugin_load_page"));
        if (!plugin.pageCount || !plugin.loadPage) plugin.pageCount = nullptr, plugin.loadPage = nullptr;
        plugin.loadCancellable = reinterpret_cast<LoadCancellableFn>(dlsym(handle, "wiv_plugin_load_page_cancellable"));
        plugin.loadPreview = reinterpret_cast<LoadPreviewFn>(dlsym(handle, "wiv_plugin_load_preview"));
        plugin.animOpen = reinterpret_cast<AnimOpenFn>(dlsym(handle, "wiv_plugin_anim_open"));
        plugin.animNext = reinterpret_cast<AnimNextFn>(dlsym(handle, "wiv_plugin_anim_next"));
        plugin.animClose = reinterpret_cast<AnimCloseFn>(dlsym(handle, "wiv_plugin_anim_close"));
        if (!plugin.animOpen || !plugin.animNext || !plugin.animClose)
            plugin.animOpen = nullptr, plugin.animNext = nullptr, plugin.animClose = nullptr;
        plugin.freeImage = reinterpret_cast<FreeImageFn>(dlsym(handle, "wiv_plugin_free_image"));
        plugin.freeError = reinterpret_cast<FreeErrorFn>(dlsym(handle, "wiv_plugin_free_error"));

        if (!plugin.supportsFile || !plugin.loadImage || !plugin.freeImage || !plugin.freeError)
        {
            spdlog::error("Plugin {} is missing required symbols, skipping", path);
            dlclose(handle);
            continue;
        }

        spdlog::info("Loaded image plugin: {}", path);
        plugins.push_back(plugin);
    }

    return plugins;
}

// The scan runs once, inside a function-local static's initializer, so the
// main thread and the prefetch workers can race to the first call: C++
// guarantees the losers block until the plugin list is complete, instead of
// seeing it half-built (or empty) and reporting "unsupported file format".
std::vector<Plugin>& loadedPlugins()
{
    static std::vector<Plugin> plugins = scanPlugins();
    return plugins;
}

} // namespace

std::optional<LoadedImage> ImagePluginHost::load(const std::string& filename, int page, const volatile int* cancel)
{
    const auto cancelled = [cancel]() { return cancel && *cancel != 0; };
    // Sniffing opens the file, so only fall back to it once no plugin
    // claims the name; a suffix match is still preferred.
    const bool claimedBySuffix = isSupported(filename);
    for (const Plugin& plugin : loadedPlugins())
    {
        if (claimedBySuffix ? !plugin.supportsFile(filename.c_str())
                            : !(plugin.sniffFile && plugin.sniffFile(filename.c_str())))
            continue;

        char* errorMsg = nullptr;
        const int pageCount = plugin.pageCount ? std::max(1, plugin.pageCount(filename.c_str())) : 1;
        if (cancelled())
            return std::nullopt;
        const int wantedPage = (plugin.loadPage && page > 0 && page < pageCount) ? page : 0;
        WivPluginImage* img = plugin.loadCancellable ? plugin.loadCancellable(filename.c_str(), wantedPage, cancel, &errorMsg)
                               : wantedPage > 0      ? plugin.loadPage(filename.c_str(), wantedPage, &errorMsg)
                                                     : plugin.loadImage(filename.c_str(), &errorMsg);
        if (img && cancelled())
        {
            plugin.freeImage(img);
            img = nullptr;
        }
        if (!img)
        {
            if (cancelled())
            {
                if (errorMsg)
                    plugin.freeError(errorMsg);
                return std::nullopt;
            }
            spdlog::error("{} ({})", errorMsg ? errorMsg : "failed to load image", filename);
            if (errorMsg)
                plugin.freeError(errorMsg);
            return std::nullopt;
        }

        LoadedImage result;
        result.width = img->width;
        result.height = img->height;
        result.rgba.assign(img->rgba, img->rgba + static_cast<size_t>(img->width) * img->height * 4);
        result.sampleType = img->sampleType;
        result.depth = std::max(1, img->depth);
        result.pageCount = pageCount;
        if (img->sampleType != WIV_SAMPLE_NONE && img->samples)
        {
            const size_t bytes = static_cast<size_t>(img->width) * img->height * result.depth *
                                  wiv_plugin::sampleTypeSize(img->sampleType);
            const auto* samplesBytes = static_cast<const uint8_t*>(img->samples);
            result.samples.assign(samplesBytes, samplesBytes + bytes);
        }
        rememberFormat(filename, plugin.format);
        const int orientation = img->orientation;
        plugin.freeImage(img);
        applyOrientation(result, orientation);
        computeSampleMinMax(result);
        return result;
    }

    spdlog::error("No plugin supports image file {}", filename);
    return std::nullopt;
}

bool ImagePluginHost::mayBeAnimated(const std::string& filename)
{
    for (const Plugin& plugin : loadedPlugins())
        if (plugin.animOpen && plugin.supportsFile(filename.c_str()))
            return true;
    return false;
}

std::shared_ptr<const Animation> ImagePluginHost::loadAnimation(const std::string& filename, size_t maxBytes,
                                                                const volatile int* cancel)
{
    const auto cancelled = [cancel]() { return cancel && *cancel != 0; };
    for (const Plugin& plugin : loadedPlugins())
    {
        if (!plugin.animOpen || !plugin.supportsFile(filename.c_str()))
            continue;
        WivAnimInfo info{};
        void* decoder = plugin.animOpen(filename.c_str(), &info);
        if (!decoder)
            continue; // not an animation as far as this plugin can tell
        auto anim = std::make_shared<Animation>();
        anim->width = info.width;
        anim->height = info.height;
        anim->loopCount = std::max(0, info.loop_count);
        const size_t frameBytes = static_cast<size_t>(info.width) * info.height * 4;
        bool ok = info.width > 0 && info.height > 0 && frameBytes > 0 &&
                  (info.frame_count <= 0 || frameBytes * static_cast<size_t>(info.frame_count) <= maxBytes);
        if (!ok)
            spdlog::warn("Animation {} is too large to hold in memory ({}x{}, {} frames); showing the first frame only",
                         filename, info.width, info.height, info.frame_count);
        std::vector<uint8_t> buffer(ok ? frameBytes : 0);
        while (ok && !cancelled())
        {
            int delay = 0;
            if (!plugin.animNext(decoder, buffer.data(), &delay))
                break;
            if (anim->frames.size() * frameBytes + frameBytes > maxBytes)
            {
                spdlog::warn("Animation {} exceeds the {} MiB frame budget; showing the first frame only", filename,
                             maxBytes >> 20);
                ok = false;
                break;
            }
            // Zero/tiny delays mean "as fast as possible" in the files that
            // use them; every viewer plays those at 10 fps instead.
            anim->frames.emplace_back();
            anim->frames.back().assign(buffer.data(), buffer.data() + frameBytes);
            anim->delaysMs.push_back(delay <= 10 ? 100 : delay);
        }
        plugin.animClose(decoder);
        if (!ok || cancelled() || anim->frames.size() < 2)
            return nullptr;
        return anim;
    }
    return nullptr;
}

std::optional<LoadedImage> ImagePluginHost::loadPreview(const std::string& filename, int minLongSide)
{
    // Only suffix-claimed files: the preview is an optimization, never worth a content sniff.
    for (const Plugin& plugin : loadedPlugins())
    {
        if (!plugin.supportsFile(filename.c_str()))
            continue;
        if (!plugin.loadPreview)
            return std::nullopt;

        char* errorMsg = nullptr;
        int fullWidth = 0, fullHeight = 0;
        WivPluginImage* img = plugin.loadPreview(filename.c_str(), minLongSide, &fullWidth, &fullHeight, &errorMsg);
        if (errorMsg)
            plugin.freeError(errorMsg);
        if (!img)
            return std::nullopt;

        LoadedImage result;
        result.width = img->width;
        result.height = img->height;
        result.rgba.assign(img->rgba, img->rgba + static_cast<size_t>(img->width) * img->height * 4);
        const int orientation = img->orientation;
        plugin.freeImage(img);
        applyOrientation(result, orientation);
        const bool swapDims = orientation >= 5 && orientation <= 8;
        result.fullWidth = swapDims ? fullHeight : fullWidth;
        result.fullHeight = swapDims ? fullWidth : fullHeight;
        return result;
    }
    return std::nullopt;
}

bool ImagePluginHost::isSupported(const std::string& filename)
{
    for (const Plugin& plugin : loadedPlugins())
    {
        if (plugin.supportsFile(filename.c_str()))
            return true;
    }
    return false;
}

std::string ImagePluginHost::formatOf(const std::string& filename)
{
    {
        std::lock_guard<std::mutex> lock(formatCacheMutex());
        auto it = formatCache().find(filename);
        if (it != formatCache().end())
            return it->second;
    }
    // Not loaded yet: ask the plugins the way load() would - by suffix, else by content.
    std::string format;
    for (const Plugin& plugin : loadedPlugins())
        if (plugin.supportsFile(filename.c_str()))
        {
            format = plugin.format;
            break;
        }
    if (format.empty())
        for (const Plugin& plugin : loadedPlugins())
            if (plugin.sniffFile && plugin.sniffFile(filename.c_str()))
            {
                format = plugin.format;
                break;
            }
    if (!format.empty())
        rememberFormat(filename, format);
    return format;
}

std::vector<std::string> ImagePluginHost::formatNames()
{
    std::vector<std::string> names;
    for (const Plugin& plugin : loadedPlugins())
        names.push_back(plugin.format);
    return names;
}

bool ImagePluginHost::isSupportedExplicit(const std::string& filename)
{
    if (isSupported(filename))
        return true;
    for (const Plugin& plugin : loadedPlugins())
    {
        if (plugin.sniffFile && plugin.sniffFile(filename.c_str()))
            return true;
    }
    return false;
}

const LoadedImage* ImageCache::get(const std::string& path, int page)
{
    const std::string key = keyFor(path, page);
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        lru_.splice(lru_.begin(), lru_, it->second.lruIt); // move to front (most-recently-used)
        return &it->second.image;
    }

    auto loaded = ImagePluginHost::load(path, page);
    if (!loaded)
        return nullptr;

    if (entries_.size() >= capacity_ && !lru_.empty())
    {
        entries_.erase(lru_.back());
        lru_.pop_back();
    }

    lru_.push_front(key);
    Entry entry{std::move(*loaded), lru_.begin()};
    return &entries_.emplace(key, std::move(entry)).first->second.image;
}

const LoadedImage* ImageCache::peek(const std::string& path, int page)
{
    auto it = entries_.find(keyFor(path, page));
    if (it == entries_.end())
        return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lruIt);
    return &it->second.image;
}

bool ImageCache::contains(const std::string& path, int page) const
{
    return entries_.count(keyFor(path, page)) > 0;
}

void ImageCache::put(const std::string& path, int page, LoadedImage image)
{
    const std::string key = keyFor(path, page);
    if (entries_.count(key)) return;
    if (entries_.size() >= capacity_ && !lru_.empty())
    {
        entries_.erase(lru_.back());
        lru_.pop_back();
    }
    lru_.push_back(key);
    Entry entry{std::move(image), std::prev(lru_.end())};
    entries_.emplace(key, std::move(entry));
}

void ImageCache::retain(const std::unordered_set<std::string>& keep)
{
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        const std::string path = it->first.substr(0, it->first.find('\n'));
        if (keep.count(path)) { ++it; continue; }
        lru_.erase(it->second.lruIt);
        it = entries_.erase(it);
    }
}

} // namespace giv
