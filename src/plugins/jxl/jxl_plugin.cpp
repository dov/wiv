// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// jxl_plugin.cpp - wiv image plugin for JPEG XL (.jxl), via libjxl.
// Decodes to sRGB RGBA8. An animated file shows its first frame from
// wiv_plugin_load_image and plays through the anim_* entry points (frames
// come out coalesced onto the full canvas). libjxl applies the file's orientation itself, so `orientation`
// stays 1 and the host must not rotate the result again.
//
#include "../wiv_plugin_common.h"

#include <jxl/encode.h>
#include <jxl/decode.h>
#include <jxl/resizable_parallel_runner.h>

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

struct DecoderDeleter
{
    void operator()(JxlDecoder* d) const { JxlDecoderDestroy(d); }
};
struct RunnerDeleter
{
    void operator()(void* r) const { JxlResizableParallelRunnerDestroy(r); }
};

const JxlPixelFormat kRgba8 = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

// A streaming decoder over a file that stays in memory for the decoder's life.
struct Anim
{
    std::vector<unsigned char> data;
    std::unique_ptr<JxlDecoder, DecoderDeleter> dec;
    std::unique_ptr<void, RunnerDeleter> runner;
    int width = 0, height = 0;
    double msPerTick = 0;
};

} // namespace

extern "C" void* wiv_plugin_anim_open(const char* filename, WivAnimInfo* out)
{
    // Stills are the common case: bail out cheaply unless the header says animation.
    auto a = std::make_unique<Anim>();
    if (!readFile(filename, a->data))
        return nullptr;
    a->dec.reset(JxlDecoderCreate(nullptr));
    a->runner.reset(JxlResizableParallelRunnerCreate(nullptr));
    if (!a->dec || !a->runner ||
        JxlDecoderSetParallelRunner(a->dec.get(), JxlResizableParallelRunner, a->runner.get()) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(a->dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME |
                                                    JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS)
        return nullptr;
    JxlDecoderSetInput(a->dec.get(), a->data.data(), a->data.size());
    JxlDecoderCloseInput(a->dec.get());
    if (JxlDecoderProcessInput(a->dec.get()) != JXL_DEC_BASIC_INFO)
        return nullptr;
    JxlBasicInfo info;
    // A rotated file would need width/height swapped; leave those to the still path.
    if (JxlDecoderGetBasicInfo(a->dec.get(), &info) != JXL_DEC_SUCCESS || !info.have_animation ||
        info.xsize == 0 || info.ysize == 0 || info.orientation != JXL_ORIENT_IDENTITY ||
        info.animation.tps_numerator == 0)
        return nullptr;
    JxlResizableParallelRunnerSetThreads(a->runner.get(),
                                         JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
    a->width = static_cast<int>(info.xsize);
    a->height = static_cast<int>(info.ysize);
    a->msPerTick = 1000.0 * info.animation.tps_denominator / info.animation.tps_numerator;
    *out = {a->width, a->height, 0, static_cast<int>(info.animation.num_loops)};
    return a.release();
}

extern "C" bool wiv_plugin_anim_next(void* decoder, unsigned char* rgba, int* delay_ms)
{
    auto* a = static_cast<Anim*>(decoder);
    uint32_t ticks = 0;
    for (;;)
    {
        switch (JxlDecoderProcessInput(a->dec.get()))
        {
        case JXL_DEC_COLOR_ENCODING:
        {
            JxlColorEncoding srgb;
            JxlColorEncodingSetToSRGB(&srgb, /*is_gray=*/JXL_FALSE);
            JxlDecoderSetPreferredColorProfile(a->dec.get(), &srgb);
            break;
        }
        case JXL_DEC_FRAME:
        {
            JxlFrameHeader fh;
            if (JxlDecoderGetFrameHeader(a->dec.get(), &fh) != JXL_DEC_SUCCESS)
                return false;
            ticks = fh.duration;
            break;
        }
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
        {
            size_t size = 0;
            if (JxlDecoderImageOutBufferSize(a->dec.get(), &kRgba8, &size) != JXL_DEC_SUCCESS ||
                size != static_cast<size_t>(a->width) * a->height * 4 ||
                JxlDecoderSetImageOutBuffer(a->dec.get(), &kRgba8, rgba, size) != JXL_DEC_SUCCESS)
                return false;
            break;
        }
        case JXL_DEC_FULL_IMAGE:
            *delay_ms = static_cast<int>(ticks * a->msPerTick + 0.5);
            return true;
        default:
            return false; // JXL_DEC_SUCCESS (last frame done) or an error
        }
    }
}

extern "C" void wiv_plugin_anim_close(void* decoder) { delete static_cast<Anim*>(decoder); }

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasExtension(std::string(filename), ".jxl");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    auto fail = [&](const char* fmt) -> WivPluginImage*
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError(fmt, filename);
        return nullptr;
    };

    std::vector<unsigned char> data;
    if (!readFile(filename, data))
        return fail("jxl: failed to read %s");

    std::unique_ptr<JxlDecoder, DecoderDeleter> dec(JxlDecoderCreate(nullptr));
    std::unique_ptr<void, RunnerDeleter> runner(JxlResizableParallelRunnerCreate(nullptr));
    if (!dec || !runner)
        return fail("jxl: out of memory decoding %s");
    if (JxlDecoderSetParallelRunner(dec.get(), JxlResizableParallelRunner, runner.get()) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) !=
            JXL_DEC_SUCCESS)
        return fail("jxl: decoder setup failed for %s");
    JxlDecoderSetInput(dec.get(), data.data(), data.size());
    JxlDecoderCloseInput(dec.get());

    const JxlPixelFormat& format = kRgba8;
    WivPluginImage* img = nullptr;
    for (;;)
    {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
        if (status == JXL_DEC_BASIC_INFO)
        {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS || info.xsize == 0 || info.ysize == 0)
                break;
            JxlResizableParallelRunnerSetThreads(
                runner.get(), JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
            img = wiv_plugin::allocImage(static_cast<int>(info.xsize), static_cast<int>(info.ysize));
        }
        else if (status == JXL_DEC_COLOR_ENCODING)
        {
            // Ask for sRGB; if libjxl can't convert (no CMS for this profile)
            // the pixels stay in the file's own space, which is still viewable.
            JxlColorEncoding srgb;
            JxlColorEncodingSetToSRGB(&srgb, /*is_gray=*/JXL_FALSE);
            JxlDecoderSetPreferredColorProfile(dec.get(), &srgb);
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
        {
            size_t size = 0;
            if (!img || JxlDecoderImageOutBufferSize(dec.get(), &format, &size) != JXL_DEC_SUCCESS ||
                size != static_cast<size_t>(img->width) * img->height * 4 ||
                JxlDecoderSetImageOutBuffer(dec.get(), &format, img->rgba, size) != JXL_DEC_SUCCESS)
                break;
        }
        else if (status == JXL_DEC_FULL_IMAGE)
        {
            return img; // first frame only
        }
        else
        {
            break; // JXL_DEC_ERROR, JXL_DEC_NEED_MORE_INPUT (truncated), or end without an image
        }
    }
    if (img)
        wiv_plugin_free_image(img);
    return fail("jxl: failed to decode %s");
}
