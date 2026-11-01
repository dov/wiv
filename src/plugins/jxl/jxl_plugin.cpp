//
// jxl_plugin.cpp - wiv image plugin for JPEG XL (.jxl), via libjxl.
// Decodes the first frame to sRGB RGBA8 (an animated file shows its first
// frame). libjxl applies the file's orientation itself, so `orientation`
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

} // namespace

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

    const JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
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
