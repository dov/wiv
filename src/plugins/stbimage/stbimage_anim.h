// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// stbimage_anim.h - animation support for the stbimage plugin (included by
// stbimage_plugin.cpp after stb_image.h): GIF through stb's own multi-frame
// GIF loader, and APNG by splitting the file into one ordinary PNG per
// frame, decoding those with stb, and compositing them onto the canvas by
// hand (stb itself only ever reads an APNG's default image).
//
#include "../wiv_plugin_common.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace stbanim
{

bool readFile(const char* filename, std::vector<uint8_t>& out)
{
    FILE* fh = std::fopen(filename, "rb");
    if (!fh)
        return false;
    std::fseek(fh, 0, SEEK_END);
    const long size = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    bool ok = size > 0;
    if (ok)
    {
        out.resize(static_cast<size_t>(size));
        ok = std::fread(out.data(), 1, out.size(), fh) == out.size();
    }
    std::fclose(fh);
    return ok;
}

struct Decoder
{
    virtual ~Decoder() = default;
    WivAnimInfo info{};
    virtual bool next(unsigned char* rgba, int* delayMs) = 0;
};

// ---------------------------------------------------------------- GIF ----

// Streams stb's GIF decoder one frame per next() instead of using its
// load-everything entry point, which would hold the whole animation (twice
// over, once the host has copied it) before the host's memory budget or a
// cancel could act. stbi__gif_load_next() composites onto an internal canvas;
// disposal "restore previous" (3) needs the frame from two steps back, so
// the last two outputs are kept.
struct GifDecoder : Decoder
{
    std::vector<uint8_t> data;
    stbi__context ctx;
    stbi__gif gif;
    std::vector<uint8_t> prev, prevPrev; // outputs of frames n-1 and n-2
    std::vector<uint8_t> first;          // frame 0, decoded by open() to learn the size
    bool haveFirst = false;
    int frameIndex = 0;
    bool done = false;

    GifDecoder() { std::memset(&gif, 0, sizeof(gif)); }
    ~GifDecoder() override
    {
        STBI_FREE(gif.out);
        STBI_FREE(gif.history);
        STBI_FREE(gif.background);
    }

    // The next composited frame (gif.w*gif.h*4 bytes at *pixels), or false at the end.
    bool decodeNext(const uint8_t** pixels)
    {
        if (done)
            return false;
        int comp = 0;
        stbi_uc* u = stbi__gif_load_next(&ctx, &gif, &comp, 4, frameIndex >= 2 ? prevPrev.data() : nullptr);
        if (u == reinterpret_cast<stbi_uc*>(&ctx) || !u)
        {
            done = true;
            return false;
        }
        const size_t bytes = static_cast<size_t>(gif.w) * gif.h * 4;
        std::swap(prevPrev, prev);
        prev.assign(u, u + bytes);
        ++frameIndex;
        *pixels = prev.data();
        return true;
    }

    bool next(unsigned char* rgba, int* delayMs) override
    {
        const size_t bytes = static_cast<size_t>(info.width) * info.height * 4;
        if (haveFirst)
        {
            haveFirst = false;
            std::memcpy(rgba, first.data(), bytes);
            std::vector<uint8_t>().swap(first);
            *delayMs = firstDelay;
            return true;
        }
        const uint8_t* px = nullptr;
        if (!decodeNext(&px))
            return false;
        std::memcpy(rgba, px, bytes);
        *delayMs = gif.delay;
        return true;
    }
    int firstDelay = 100;
};

// A GIF's loop count lives in the NETSCAPE2.0 application extension; stb
// skips it, so find it ourselves. The stored value is the number of *repeats*
// (0 = forever), as browsers play it; no extension means play once.
int gifPlays(const std::vector<uint8_t>& data)
{
    static const char kTag[] = "NETSCAPE2.0";
    for (size_t i = 0; i + 11 + 5 < data.size(); ++i)
        if (std::memcmp(&data[i], kTag, 11) == 0 && data[i + 11] == 3 && data[i + 12] == 1)
        {
            const int repeats = data[i + 13] | (data[i + 14] << 8);
            return repeats == 0 ? 0 : repeats + 1;
        }
    return 1;
}

std::unique_ptr<Decoder> openGif(std::vector<uint8_t>&& data)
{
    auto dec = std::make_unique<GifDecoder>();
    dec->data = std::move(data);
    stbi__start_mem(&dec->ctx, dec->data.data(), static_cast<int>(dec->data.size()));
    if (!stbi__gif_test(&dec->ctx))
        return nullptr;
    const uint8_t* px = nullptr;
    if (!dec->decodeNext(&px))
        return nullptr;
    dec->first.assign(px, px + static_cast<size_t>(dec->gif.w) * dec->gif.h * 4);
    dec->firstDelay = dec->gif.delay;
    dec->haveFirst = true;
    dec->info = {dec->gif.w, dec->gif.h, 0, gifPlays(dec->data)};
    return dec;
}

// --------------------------------------------------------------- APNG ----

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
unsigned be16(const uint8_t* p) { return (unsigned(p[0]) << 8) | p[1]; }

void putChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len)
{
    const auto put32 = [&out](uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            out.push_back(static_cast<uint8_t>(v >> s));
    };
    put32(static_cast<uint32_t>(len));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len)
        out.insert(out.end(), data, data + len);
    put32(crc32(out.data() + start, out.size() - start));
}

struct ApngFrame
{
    uint32_t w = 0, h = 0, x = 0, y = 0;
    int delayMs = 100;
    int dispose = 0; // 0 none, 1 background, 2 previous
    int blend = 0;   // 0 source, 1 over
    std::vector<uint8_t> idat;
};

struct ApngDecoder : Decoder
{
    uint8_t ihdr[13]{};
    std::vector<uint8_t> plte, trns;
    std::vector<ApngFrame> apngFrames;
    std::vector<uint8_t> canvas, saved;
    size_t index = 0;

    // Frame `f` as a stand-alone PNG, decoded to RGBA8.
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> decodeFrame(const ApngFrame& f)
    {
        static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::vector<uint8_t> png(kSig, kSig + 8);
        uint8_t hdr[13];
        std::memcpy(hdr, ihdr, 13);
        for (int i = 0; i < 4; ++i)
        {
            hdr[i] = static_cast<uint8_t>(f.w >> (24 - 8 * i));
            hdr[4 + i] = static_cast<uint8_t>(f.h >> (24 - 8 * i));
        }
        putChunk(png, "IHDR", hdr, 13);
        if (!plte.empty())
            putChunk(png, "PLTE", plte.data(), plte.size());
        if (!trns.empty())
            putChunk(png, "tRNS", trns.data(), trns.size());
        putChunk(png, "IDAT", f.idat.data(), f.idat.size());
        putChunk(png, "IEND", nullptr, 0);
        int w = 0, h = 0, comp = 0;
        std::unique_ptr<unsigned char, decltype(&stbi_image_free)> px{
            stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &comp, 4), stbi_image_free};
        if (px && (uint32_t(w) != f.w || uint32_t(h) != f.h))
            px.reset();
        return px;
    }

    bool next(unsigned char* rgba, int* delayMs) override
    {
        if (index >= apngFrames.size())
            return false;
        const ApngFrame& f = apngFrames[index];
        const size_t stride = static_cast<size_t>(info.width) * 4;
        // The previous frame's disposal happens before this one is drawn;
        // a first frame that asks to restore "previous" just clears.
        if (index > 0)
        {
            const ApngFrame& p = apngFrames[index - 1];
            if (p.dispose == 1 || (p.dispose == 2 && saved.empty()))
                for (uint32_t row = 0; row < p.h; ++row)
                    std::memset(&canvas[(p.y + row) * stride + p.x * 4], 0, p.w * 4);
            else if (p.dispose == 2)
                canvas = saved;
        }
        saved.clear();
        if (f.dispose == 2)
            saved = canvas;

        auto px = decodeFrame(f);
        if (px)
        {
            for (uint32_t row = 0; row < f.h; ++row)
            {
                const unsigned char* src = px.get() + static_cast<size_t>(row) * f.w * 4;
                unsigned char* dst = &canvas[(f.y + row) * stride + f.x * 4];
                if (f.blend == 0)
                {
                    std::memcpy(dst, src, f.w * 4);
                    continue;
                }
                for (uint32_t col = 0; col < f.w; ++col, src += 4, dst += 4)
                {
                    const unsigned sa = src[3];
                    if (sa == 255 || dst[3] == 0)
                    {
                        std::memcpy(dst, src, 4);
                        continue;
                    }
                    if (sa == 0)
                        continue;
                    // Non-premultiplied "over".
                    const unsigned da = dst[3];
                    const unsigned outA = sa * 255 + da * (255 - sa); // scaled by 255
                    for (int c = 0; c < 3; ++c)
                        dst[c] = static_cast<uint8_t>((src[c] * sa * 255 + dst[c] * da * (255 - sa)) / outA);
                    dst[3] = static_cast<uint8_t>(outA / 255);
                }
            }
        }
        std::memcpy(rgba, canvas.data(), canvas.size());
        *delayMs = f.delayMs;
        ++index;
        return true;
    }
};

// Is there an acTL chunk before the first IDAT (where the spec requires it)?
// Walks chunk headers with seeks, so a still PNG costs a few small reads.
bool pngHasActl(FILE* fh)
{
    std::fseek(fh, 8, SEEK_SET);
    unsigned char hdr[8];
    while (std::fread(hdr, 1, 8, fh) == 8)
    {
        if (!std::memcmp(hdr + 4, "acTL", 4))
            return true;
        if (!std::memcmp(hdr + 4, "IDAT", 4) || !std::memcmp(hdr + 4, "IEND", 4))
            return false;
        if (std::fseek(fh, static_cast<long>(be32(hdr)) + 4, SEEK_CUR) != 0)
            return false;
    }
    return false;
}

std::unique_ptr<Decoder> openApng(const std::vector<uint8_t>& data)
{
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() < 8 || std::memcmp(data.data(), kSig, 8) != 0)
        return nullptr;

    auto dec = std::make_unique<ApngDecoder>();
    bool haveHdr = false, haveActl = false, sawIdat = false;
    uint32_t numFrames = 0, plays = 1;
    ApngFrame pending;
    bool havePending = false;
    std::vector<ApngFrame>& frames = dec->apngFrames;
    const auto flush = [&] {
        if (havePending)
            frames.push_back(std::move(pending));
        pending = ApngFrame{};
        havePending = false;
    };

    for (size_t pos = 8; pos + 12 <= data.size();)
    {
        const uint32_t len = be32(&data[pos]);
        if (len > data.size() - pos - 12)
            break;
        const char* type = reinterpret_cast<const char*>(&data[pos + 4]);
        const uint8_t* body = &data[pos + 8];
        pos += 12 + static_cast<size_t>(len);

        if (!std::memcmp(type, "IHDR", 4) && len == 13)
        {
            std::memcpy(dec->ihdr, body, 13);
            haveHdr = true;
        }
        else if (!std::memcmp(type, "PLTE", 4))
            dec->plte.assign(body, body + len);
        else if (!std::memcmp(type, "tRNS", 4))
            dec->trns.assign(body, body + len);
        else if (!std::memcmp(type, "acTL", 4) && len >= 8)
        {
            haveActl = true;
            numFrames = be32(body);
            plays = be32(body + 4);
        }
        else if (!std::memcmp(type, "fcTL", 4) && len >= 26)
        {
            flush();
            pending.w = be32(body + 4);
            pending.h = be32(body + 8);
            pending.x = be32(body + 12);
            pending.y = be32(body + 16);
            const unsigned num = be16(body + 20), den = be16(body + 22);
            pending.delayMs = static_cast<int>(num * 1000u / (den ? den : 100u));
            pending.dispose = body[24];
            pending.blend = body[25];
            havePending = true;
        }
        else if (!std::memcmp(type, "IDAT", 4))
        {
            // Only part of the animation if an fcTL came first; otherwise
            // it is a default image that the animation skips.
            if (havePending)
                pending.idat.insert(pending.idat.end(), body, body + len);
            sawIdat = true;
        }
        else if (!std::memcmp(type, "fdAT", 4) && len >= 4 && havePending)
            pending.idat.insert(pending.idat.end(), body + 4, body + len);
        else if (!std::memcmp(type, "IEND", 4))
            break;
    }
    flush();
    if (!haveHdr || !haveActl || !sawIdat || frames.size() < 2 || frames.size() != numFrames)
        return nullptr;

    const uint32_t cw = be32(dec->ihdr), ch = be32(dec->ihdr + 4);
    if (cw == 0 || ch == 0 || cw > 65535 || ch > 65535)
        return nullptr;
    for (const ApngFrame& f : frames)
        if (f.idat.empty() || f.w == 0 || f.h == 0 || f.x + f.w > cw || f.y + f.h > ch)
            return nullptr;
    dec->info = {static_cast<int>(cw), static_cast<int>(ch), static_cast<int>(frames.size()), static_cast<int>(plays)};
    dec->canvas.assign(static_cast<size_t>(cw) * ch * 4, 0);
    return dec;
}

} // namespace stbanim

extern "C" void* wiv_plugin_anim_open(const char* filename, WivAnimInfo* info)
{
    // Most files this is asked about are stills (every JPEG, BMP, ...): decide
    // from the first bytes, and for a PNG from its chunk headers, before
    // reading the whole file.
    FILE* fh = std::fopen(filename, "rb");
    if (!fh)
        return nullptr;
    unsigned char head[8];
    const bool gif = std::fread(head, 1, 8, fh) == 8 && !std::memcmp(head, "GIF8", 4);
    const bool png = !gif && head[0] == 0x89 && !std::memcmp(&head[1], "PNG", 3) && stbanim::pngHasActl(fh);
    std::fclose(fh);
    if (!gif && !png)
        return nullptr;

    std::vector<uint8_t> data;
    if (!stbanim::readFile(filename, data) || data.size() < 12)
        return nullptr;
    std::unique_ptr<stbanim::Decoder> dec = gif ? stbanim::openGif(std::move(data)) : stbanim::openApng(data);
    if (!dec)
        return nullptr;
    *info = dec->info;
    return dec.release();
}

extern "C" bool wiv_plugin_anim_next(void* decoder, unsigned char* rgba, int* delay_ms)
{
    return static_cast<stbanim::Decoder*>(decoder)->next(rgba, delay_ms);
}

extern "C" void wiv_plugin_anim_close(void* decoder) { delete static_cast<stbanim::Decoder*>(decoder); }
