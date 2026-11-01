//
// heif_plugin.cpp - wiv image plugin for HEIC/HEIF and AVIF stills, via
// libheif (which pulls in the actual codecs - libde265/x265 for HEVC,
// aom/dav1d for AV1 - as its own plugins). The primary image is decoded to
// 8-bit interleaved RGBA with libheif's default option of applying the
// file's rotation/mirror transforms, so the plugin leaves `orientation` at 1.
//
#include "../wiv_plugin_common.h"

#include <libheif/heif.h>
#include <libheif/heif_version.h>
#if LIBHEIF_NUMERIC_VERSION >= ((1 << 24) | (20 << 16))
#include <libheif/heif_sequences.h>
#define WIV_HEIF_SEQUENCES 1
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
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
    static const char* kExtensions[] = {".heic", ".heif", ".hif", ".avif", ".avifs"};
    for (const char* ext : kExtensions)
        if (hasExtension(f, ext))
            return true;
    return false;
}

extern "C" bool wiv_plugin_sniff_file(const char* filename)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
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
#ifdef WIV_HEIF_SEQUENCES
        // A pure image sequence has no primary image; its first frame stands in.
        if (heif_context_has_sequence(ctx))
        {
            WivPluginImage* first = nullptr;
            if (heif_track* track = heif_context_get_track(ctx, 0))
            {
                heif_image* frame = nullptr;
                if (heif_track_decode_next_image(track, &frame, heif_colorspace_RGB, heif_chroma_interleaved_RGBA,
                                                  nullptr).code == heif_error_Ok)
                {
                    const int w = heif_image_get_width(frame, heif_channel_interleaved);
                    const int h = heif_image_get_height(frame, heif_channel_interleaved);
                    int stride = 0;
                    const uint8_t* plane = heif_image_get_plane_readonly(frame, heif_channel_interleaved, &stride);
                    if (plane && w > 0 && h > 0)
                    {
                        first = wiv_plugin::allocImage(w, h);
                        for (int y = 0; y < h; ++y)
                            std::memcpy(first->rgba + static_cast<size_t>(y) * w * 4,
                                        plane + static_cast<size_t>(y) * stride, static_cast<size_t>(w) * 4);
                    }
                    heif_image_release(frame);
                }
                heif_track_release(track);
            }
            if (first)
            {
                heif_context_free(ctx);
                return first;
            }
        }
#endif
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

#ifdef WIV_HEIF_SEQUENCES
namespace
{

struct Anim
{
    heif_context* ctx = nullptr;
    heif_track* track = nullptr;
    heif_decoding_options* options = nullptr;
    int width = 0, height = 0;
    ~Anim()
    {
        heif_decoding_options_free(options);
        heif_track_release(track);
        heif_context_free(ctx);
    }
};

} // namespace

extern "C" void* wiv_plugin_anim_open(const char* filename, WivAnimInfo* info)
{
    auto a = std::make_unique<Anim>();
    a->ctx = heif_context_alloc();
    if (heif_context_read_from_file(a->ctx, filename, nullptr).code != heif_error_Ok ||
        !heif_context_has_sequence(a->ctx))
        return nullptr;
    a->track = heif_context_get_track(a->ctx, 0);
    uint16_t w = 0, h = 0;
    if (!a->track || heif_track_get_image_resolution(a->track, &w, &h).code != heif_error_Ok || !w || !h)
        return nullptr;
    a->width = w;
    a->height = h;
    // Play the media once per pass; looping is the host's job.
    a->options = heif_decoding_options_alloc();
    a->options->ignore_sequence_editlist = 1;
#if LIBHEIF_NUMERIC_VERSION > ((1 << 24) | (21 << 16) | (2 << 8)) // getter absent in libheif <= 1.21.2
    const uint32_t reps = heif_track_get_number_of_repetitions(a->track);
    const bool infinite = reps == heif_sequence_track_number_of_repetitions_infinite;
#else
    const uint32_t reps = 1;
    const bool infinite = false;
#endif
    // Sequences carry no frame count up front.
    *info = {a->width, a->height, 0, infinite ? 0 : static_cast<int>(std::max<uint32_t>(1, reps))};
    return a.release();
}

extern "C" bool wiv_plugin_anim_next(void* decoder, unsigned char* rgba, int* delay_ms)
{
    auto* a = static_cast<Anim*>(decoder);
    heif_image* frame = nullptr;
    if (heif_track_decode_next_image(a->track, &frame, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, a->options)
            .code != heif_error_Ok)
        return false; // includes end-of-sequence
    bool ok = false;
    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(frame, heif_channel_interleaved, &stride);
    if (plane && heif_image_get_width(frame, heif_channel_interleaved) == a->width &&
        heif_image_get_height(frame, heif_channel_interleaved) == a->height)
    {
        for (int y = 0; y < a->height; ++y)
            std::memcpy(rgba + static_cast<size_t>(y) * a->width * 4, plane + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(a->width) * 4);
        const uint32_t scale = std::max<uint32_t>(1, heif_track_get_timescale(a->track));
        *delay_ms = static_cast<int>(static_cast<uint64_t>(heif_image_get_duration(frame)) * 1000 / scale);
        ok = true;
    }
    heif_image_release(frame);
    return ok;
}

extern "C" void wiv_plugin_anim_close(void* decoder) { delete static_cast<Anim*>(decoder); }
#endif
