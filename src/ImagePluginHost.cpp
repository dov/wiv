#include "ImagePluginHost.h"

#include "plugins/wiv_plugin.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

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
        std::cerr << "wiv: plugin directory not found: " << dir << "\n";
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
            std::cerr << "wiv: failed to load plugin " << path << ": " << dlerror() << "\n";
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
            std::cerr << "wiv: plugin " << path << " is missing required symbols, skipping\n";
            dlclose(handle);
            continue;
        }

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
            std::cerr << "wiv: " << (errorMsg ? errorMsg : "failed to load image") << " (" << filename << ")\n";
            if (errorMsg)
                plugin.freeError(errorMsg);
            return std::nullopt;
        }

        LoadedImage result;
        result.width = img->width;
        result.height = img->height;
        result.rgba.assign(img->rgba, img->rgba + static_cast<size_t>(img->width) * img->height * 4);
        plugin.freeImage(img);
        return result;
    }

    std::cerr << "wiv: no plugin supports image file " << filename << "\n";
    return std::nullopt;
}

} // namespace giv
