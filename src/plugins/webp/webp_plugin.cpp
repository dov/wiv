//
// webp_plugin.cpp - wiv image plugin for WebP, via libwebp. Modeled on
// giv's ~/hd/github/giv/src/plugins/webp.cc: read the whole file, decode
// straight into the pre-allocated destination buffer with
// WebPDecodeRGBAInto.
//
#include "../wiv_plugin_common.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

bool hasExtension(const std::string& filename, const char* ext)
{
    size_t n = std::strlen(ext);
    if (filename.size() < n)
        return false;
    return std::equal(filename.end() - n, filename.end(), ext, [](char a, char b)
                       { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

bool readFile(const char* filename, std::vector<unsigned char>& data)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
        return false;
    std::fseek(fh, 0, SEEK_END);
    long size = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    bool ok = size > 0;
    if (ok)
    {
        data.resize(static_cast<size_t>(size));
        ok = std::fread(data.data(), 1, data.size(), fh) == data.size();
    }
    std::fclose(fh);
    return ok;
}

// A WebPAnimDecoder over a file that stays in memory for the decoder's life
// (WebPData is only a view). Composites every frame onto the canvas itself.
struct Anim
{
    std::vector<unsigned char> data;
    WebPAnimDecoder* dec = nullptr;
    WebPAnimInfo info{};
    int lastTimestamp = 0;
    ~Anim()
    {
        if (dec)
            WebPAnimDecoderDelete(dec);
    }
};

std::unique_ptr<Anim> openAnim(const char* filename)
{
    // Stills are the common case: the header alone says whether this is an animation.
    {
        unsigned char head[64];
        FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
        if (!fh)
            return nullptr;
        const size_t n = std::fread(head, 1, sizeof(head), fh);
        std::fclose(fh);
        WebPBitstreamFeatures features;
        if (WebPGetFeatures(head, n, &features) != VP8_STATUS_OK || !features.has_animation)
            return nullptr;
    }
    auto a = std::make_unique<Anim>();
    if (!readFile(filename, a->data))
        return nullptr;
    WebPAnimDecoderOptions opts;
    if (!WebPAnimDecoderOptionsInit(&opts))
        return nullptr;
    opts.color_mode = MODE_RGBA;
    opts.use_threads = 1;
    WebPData wd{a->data.data(), a->data.size()};
    a->dec = WebPAnimDecoderNew(&wd, &opts);
    if (!a->dec || !WebPAnimDecoderGetInfo(a->dec, &a->info))
        return nullptr;
    return a;
}

} // namespace

extern "C" void* wiv_plugin_anim_open(const char* filename, WivAnimInfo* info)
{
    auto a = openAnim(filename);
    if (!a || a->info.frame_count < 2)
        return nullptr;
    *info = {static_cast<int>(a->info.canvas_width), static_cast<int>(a->info.canvas_height),
             static_cast<int>(a->info.frame_count), static_cast<int>(a->info.loop_count)};
    return a.release();
}

extern "C" bool wiv_plugin_anim_next(void* decoder, unsigned char* rgba, int* delay_ms)
{
    auto* a = static_cast<Anim*>(decoder);
    if (!WebPAnimDecoderHasMoreFrames(a->dec))
        return false;
    uint8_t* frame = nullptr;
    int timestamp = 0;
    if (!WebPAnimDecoderGetNext(a->dec, &frame, &timestamp))
        return false;
    std::memcpy(rgba, frame, static_cast<size_t>(a->info.canvas_width) * a->info.canvas_height * 4);
    *delay_ms = timestamp - a->lastTimestamp;
    a->lastTimestamp = timestamp;
    return true;
}

extern "C" void wiv_plugin_anim_close(void* decoder) { delete static_cast<Anim*>(decoder); }

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasExtension(std::string(filename), ".webp");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    std::vector<unsigned char> data;
    if (!readFile(filename, data))
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("webp: failed to read %s", filename);
        return nullptr;
    }

    int width = 0, height = 0;
    if (!WebPGetInfo(data.data(), data.size(), &width, &height))
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("webp: not a valid WebP file: %s", filename);
        return nullptr;
    }

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    size_t stride = static_cast<size_t>(width) * 4;
    unsigned char* decoded =
        WebPDecodeRGBAInto(data.data(), data.size(), img->rgba, stride * height, static_cast<int>(stride));
    if (!decoded)
    {
        // An animated file is refused by the still decoder: show its first frame.
        bool ok = false;
        if (auto a = openAnim(filename))
        {
            uint8_t* frame = nullptr;
            int ts = 0;
            if (static_cast<int>(a->info.canvas_width) == width && static_cast<int>(a->info.canvas_height) == height &&
                WebPAnimDecoderGetNext(a->dec, &frame, &ts))
            {
                std::memcpy(img->rgba, frame, stride * height);
                ok = true;
            }
        }
        if (!ok)
        {
            wiv_plugin_free_image(img);
            if (error_msg)
                *error_msg = wiv_plugin::makeError("webp: failed to decode %s", filename);
            return nullptr;
        }
    }

    return img;
}
