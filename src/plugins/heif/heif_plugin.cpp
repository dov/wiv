//
// heif_plugin.cpp - wiv image plugin for HEIC/HEIF and AVIF stills, via
// libheif (which pulls in the actual codecs - libde265/x265 for HEVC,
// aom/dav1d for AV1 - as its own plugins). The primary image is decoded to
// 8-bit interleaved RGBA with libheif's default option of applying the
// file's rotation/mirror transforms, so the plugin leaves `orientation` at 1.
//
#include "../wiv_plugin_common.h"

#include <libheif/heif.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

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

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    static const char* kExtensions[] = {".heic", ".heif", ".hif", ".avif"};
    for (const char* ext : kExtensions)
        if (hasExtension(f, ext))
            return true;
    return false;
}

extern "C" bool wiv_plugin_sniff_file(const char* filename)
{
    FILE* fh = std::fopen(filename, "rb");
    if (!fh)
        return false;
    unsigned char head[12];
    size_t n = std::fread(head, 1, sizeof(head), fh);
    std::fclose(fh);
    if (n < sizeof(head))
        return false;
    return heif_check_filetype(head, static_cast<int>(n)) == heif_filetype_yes_supported;
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    auto fail = [&](const char* what, const char* detail) -> WivPluginImage*
    {
        if (error_msg)
        {
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "heif: %s %s: %s", what, filename, detail);
            *error_msg = strdup(buf);
        }
        return nullptr;
    };

    heif_context* ctx = heif_context_alloc();
    heif_error err = heif_context_read_from_file(ctx, filename, nullptr);
    if (err.code != heif_error_Ok)
    {
        heif_context_free(ctx);
        return fail("failed to read", err.message);
    }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok)
    {
        heif_context_free(ctx);
        return fail("no primary image in", err.message);
    }

    heif_image* image = nullptr;
    err = heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    if (err.code != heif_error_Ok)
    {
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return fail("failed to decode", err.message);
    }

    const int width = heif_image_get_width(image, heif_channel_interleaved);
    const int height = heif_image_get_height(image, heif_channel_interleaved);
    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);

    WivPluginImage* img = nullptr;
    if (plane && width > 0 && height > 0)
    {
        img = wiv_plugin::allocImage(width, height);
        for (int y = 0; y < height; ++y)
            std::memcpy(img->rgba + static_cast<size_t>(y) * width * 4, plane + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(width) * 4);
    }

    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return img ? img : fail("empty image in", "no pixel data");
}
