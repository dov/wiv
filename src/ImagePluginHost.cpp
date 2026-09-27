#include "ImagePluginHost.h"

#include "plugins/wiv_plugin.h"

#include <spdlog/spdlog.h>

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace giv
{

namespace
{

namespace fs = std::filesystem;

using SupportsFileFn = bool (*)(const char*);
using LoadImageFn = WivPluginImage* (*)(const char*, char**);
using FreeImageFn = void (*)(WivPluginImage*);
using FreeErrorFn = void (*)(char*);

struct Plugin
{
    std::string path;
    void* handle = nullptr;
    SupportsFileFn supportsFile = nullptr;
    LoadImageFn loadImage = nullptr;
    FreeImageFn freeImage = nullptr;
    FreeErrorFn freeError = nullptr;
};

// Applies the rotation/flip implied by an EXIF/TIFF Orientation tag (1-8;
// see the Exif spec's table) to an already-decoded RGBA8 image, so that
// every plugin can hand back raw sensor-order pixels plus an orientation
// value and share one transform instead of each reimplementing it. 1
// ("normal") and anything out of range are a no-op.
void applyOrientation(LoadedImage& image, int orientation)
{
    if (orientation <= 1 || orientation > 8)
        return;

    const int w = image.width;
    const int h = image.height;
    const bool swapDims = orientation >= 5;
    const int newW = swapDims ? h : w;
    const int newH = swapDims ? w : h;

    std::vector<uint8_t> out(static_cast<size_t>(newW) * newH * 4);
    auto srcPixel = [&](int x, int y) { return &image.rgba[(static_cast<size_t>(y) * w + x) * 4]; };

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
            std::memcpy(&out[(static_cast<size_t>(y) * newW + x) * 4], srcPixel(sx, sy), 4);
        }
    }

    image.width = newW;
    image.height = newH;
    image.rgba = std::move(out);
}

std::vector<Plugin>& loadedPlugins()
{
    static std::vector<Plugin> plugins;
    static bool scanned = false;
    if (scanned)
        return plugins;
    scanned = true;

    const char* dirOverride = std::getenv("WIV_PLUGIN_DIR");
    std::string dir = dirOverride ? dirOverride : WIV_PLUGIN_DIR;

    std::error_code ec;
    if (!fs::is_directory(dir, ec))
    {
        spdlog::error("Plugin directory not found: {}", dir);
        return plugins;
    }

    std::vector<std::string> candidates;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (entry.path().extension() == ".so")
            candidates.push_back(entry.path().string());
    }
    std::sort(candidates.begin(), candidates.end());

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
    for (const Plugin& plugin : loadedPlugins())
    {
        if (!plugin.supportsFile(filename.c_str()))
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
        const int orientation = img->orientation;
        plugin.freeImage(img);
        applyOrientation(result, orientation);
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
