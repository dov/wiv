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
using FreeImageFn = void (*)(WivPluginImage*);
using FreeErrorFn = void (*)(char*);

struct Plugin
{
    std::string path;
    void* handle = nullptr;
    SupportsFileFn supportsFile = nullptr;
    SniffFileFn sniffFile = nullptr; // optional
    LoadImageFn loadImage = nullptr;
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
        image.samples = reorderBuffer(image.samples, w, h, wiv_plugin::sampleTypeSize(image.sampleType), orientation);
    image.rgba = reorderBuffer(image.rgba, w, h, 4, orientation);

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

std::vector<Plugin>& loadedPlugins()
{
    static std::vector<Plugin> plugins;
    static bool scanned = false;
    if (scanned)
        return plugins;
    scanned = true;

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
        plugin.handle = handle;
        plugin.supportsFile = reinterpret_cast<SupportsFileFn>(dlsym(handle, "wiv_plugin_supports_file"));
        plugin.sniffFile = reinterpret_cast<SniffFileFn>(dlsym(handle, "wiv_plugin_sniff_file"));
        plugin.loadImage = reinterpret_cast<LoadImageFn>(dlsym(handle, "wiv_plugin_load_image"));
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

} // namespace

std::optional<LoadedImage> ImagePluginHost::load(const std::string& filename)
{
    // Sniffing opens the file, so only fall back to it once no plugin
    // claims the name; a suffix match is still preferred.
    const bool claimedBySuffix = isSupported(filename);
    for (const Plugin& plugin : loadedPlugins())
    {
        if (claimedBySuffix ? !plugin.supportsFile(filename.c_str())
                            : !(plugin.sniffFile && plugin.sniffFile(filename.c_str())))
            continue;

        char* errorMsg = nullptr;
        WivPluginImage* img = plugin.loadImage(filename.c_str(), &errorMsg);
        if (!img)
        {
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
        if (img->sampleType != WIV_SAMPLE_NONE && img->samples)
        {
            const size_t bytes = static_cast<size_t>(img->width) * img->height * result.depth *
                                  wiv_plugin::sampleTypeSize(img->sampleType);
            const auto* samplesBytes = static_cast<const uint8_t*>(img->samples);
            result.samples.assign(samplesBytes, samplesBytes + bytes);
        }
        const int orientation = img->orientation;
        plugin.freeImage(img);
        applyOrientation(result, orientation);
        computeSampleMinMax(result);
        return result;
    }

    spdlog::error("No plugin supports image file {}", filename);
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

const LoadedImage* ImageCache::get(const std::string& path)
{
    auto it = entries_.find(path);
    if (it != entries_.end())
    {
        lru_.splice(lru_.begin(), lru_, it->second.lruIt); // move to front (most-recently-used)
        return &it->second.image;
    }

    auto loaded = ImagePluginHost::load(path);
    if (!loaded)
        return nullptr;

    if (entries_.size() >= capacity_ && !lru_.empty())
    {
        entries_.erase(lru_.back());
        lru_.pop_back();
    }

    lru_.push_front(path);
    Entry entry{std::move(*loaded), lru_.begin()};
    return &entries_.emplace(path, std::move(entry)).first->second.image;
}

} // namespace giv
