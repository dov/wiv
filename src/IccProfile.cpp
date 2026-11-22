// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "IccProfile.h"

#include <QByteArray>
#include <QtCore/qbytearray.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#ifdef WIV_HAVE_LCMS
#include <lcms2.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace giv
{

namespace
{

FILE* openUtf8(const std::string& path)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    return _wfopen(w.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

// Small random-access reader over a FILE*.
struct Reader
{
    FILE* f;
    long size = 0;
    explicit Reader(FILE* file) : f(file)
    {
        std::fseek(f, 0, SEEK_END);
        size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
    }
    bool read(long offset, void* dst, size_t n) const
    {
        if (offset < 0 || offset + static_cast<long>(n) > size) return false;
        return std::fseek(f, offset, SEEK_SET) == 0 && std::fread(dst, 1, n, f) == n;
    }
    bool bytes(long offset, size_t n, std::vector<uint8_t>& out) const
    {
        out.resize(n);
        return read(offset, out.data(), n);
    }
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

// JPEG: the profile is split over APP2 "ICC_PROFILE\0" markers (sequence number, count, data).
std::vector<uint8_t> jpegProfile(const Reader& r)
{
    std::map<int, std::vector<uint8_t>> parts;
    long pos = 2;
    uint8_t hdr[4];
    while (r.read(pos, hdr, 4))
    {
        if (hdr[0] != 0xFF) break;
        const int marker = hdr[1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { pos += 2; continue; }
        if (marker == 0xDA || marker == 0xD9) break; // start of scan / end of image
        const int len = hdr[2] << 8 | hdr[3];
        if (len < 2) break;
        if (marker == 0xE2 && len >= 16)
        {
            std::vector<uint8_t> seg;
            if (r.bytes(pos + 4, len - 2, seg) && std::memcmp(seg.data(), "ICC_PROFILE\0", 12) == 0)
                parts[seg[12]].assign(seg.begin() + 14, seg.end());
        }
        pos += 2 + len;
    }
    std::vector<uint8_t> out;
    for (auto& [seq, data] : parts) out.insert(out.end(), data.begin(), data.end());
    return out;
}

// PNG: an iCCP chunk holds "name\0", a compression byte, then a zlib stream.
std::vector<uint8_t> pngProfile(const Reader& r)
{
    long pos = 8;
    uint8_t hdr[8];
    while (r.read(pos, hdr, 8))
    {
        const uint32_t len = be32(hdr);
        if (std::memcmp(hdr + 4, "iCCP", 4) == 0)
        {
            std::vector<uint8_t> chunk;
            if (len > (64u << 20) || !r.bytes(pos + 8, len, chunk)) return {};
            const auto nul = std::find(chunk.begin(), chunk.end(), uint8_t(0));
            if (nul == chunk.end() || nul + 2 > chunk.end()) return {};
            // qUncompress wants a 4-byte big-endian expected size in front; it is only a hint.
            QByteArray z;
            const uint32_t hint = 1u << 20;
            z.append(char(hint >> 24)).append(char(hint >> 16)).append(char(hint >> 8)).append(char(hint));
            z.append(reinterpret_cast<const char*>(&*(nul + 2)), static_cast<qsizetype>(chunk.end() - (nul + 2)));
            const QByteArray raw = qUncompress(z);
            return std::vector<uint8_t>(raw.begin(), raw.end());
        }
        if (std::memcmp(hdr + 4, "IDAT", 4) == 0 || std::memcmp(hdr + 4, "IEND", 4) == 0) break; // iCCP precedes IDAT
        pos += 12 + static_cast<long>(len);
    }
    return {};
}

// TIFF: tag 34675 (InterColorProfile) of the first IFD.
std::vector<uint8_t> tiffProfile(const Reader& r)
{
    uint8_t h[8];
    if (!r.read(0, h, 8)) return {};
    const bool little = h[0] == 'I';
    auto u16 = [&](const uint8_t* p) { return little ? uint32_t(p[0] | p[1] << 8) : uint32_t(p[0] << 8 | p[1]); };
    auto u32 = [&](const uint8_t* p) { return little ? le32(p) : be32(p); };
    const uint32_t ifd = u32(h + 4);
    uint8_t cnt[2];
    if (!r.read(ifd, cnt, 2)) return {};
    const uint32_t n = u16(cnt);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint8_t e[12];
        if (!r.read(long(ifd) + 2 + long(i) * 12, e, 12)) return {};
        if (u16(e) != 34675) continue;
        const uint32_t count = u32(e + 4), offset = u32(e + 8);
        std::vector<uint8_t> out;
        if (count > (64u << 20) || !r.bytes(offset, count, out)) return {};
        return out;
    }
    return {};
}

// WebP: an "ICCP" chunk in the RIFF container.
std::vector<uint8_t> webpProfile(const Reader& r)
{
    long pos = 12;
    uint8_t hdr[8];
    while (r.read(pos, hdr, 8))
    {
        const uint32_t len = le32(hdr + 4);
        if (std::memcmp(hdr, "ICCP", 4) == 0)
        {
            std::vector<uint8_t> out;
            if (len > (64u << 20) || !r.bytes(pos + 8, len, out)) return {};
            return out;
        }
        pos += 8 + long(len) + long(len & 1);
    }
    return {};
}

} // namespace

std::vector<uint8_t> readIccProfile(const std::string& path)
{
    FILE* f = openUtf8(path);
    if (!f) return {};
    Reader r(f);
    uint8_t magic[12] = {};
    std::vector<uint8_t> profile;
    if (r.read(0, magic, 12))
    {
        if (magic[0] == 0xFF && magic[1] == 0xD8)
            profile = jpegProfile(r);
        else if (std::memcmp(magic, "\x89PNG\r\n\x1a\n", 8) == 0)
            profile = pngProfile(r);
        else if ((std::memcmp(magic, "II*\0", 4) == 0) || (std::memcmp(magic, "MM\0*", 4) == 0))
            profile = tiffProfile(r);
        else if (std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "WEBP", 4) == 0)
            profile = webpProfile(r);
    }
    std::fclose(f);
    return profile;
}

#ifdef WIV_HAVE_LCMS

bool iccConversionAvailable() { return true; }

std::string iccDescription(const std::vector<uint8_t>& profile)
{
    cmsHPROFILE p = cmsOpenProfileFromMem(profile.data(), static_cast<cmsUInt32Number>(profile.size()));
    if (!p) return {};
    char buf[256] = {};
    cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", buf, sizeof(buf));
    cmsCloseProfile(p);
    return buf;
}

namespace
{
// A transform from `profile` to sRGB, or null when the profile is not an RGB profile, is unusable, or is
// (to within rounding) sRGB already.
cmsHTRANSFORM makeSrgbTransform(const std::vector<uint8_t>& profile)
{
    if (profile.size() < 128) return nullptr;
    cmsHPROFILE in = cmsOpenProfileFromMem(profile.data(), static_cast<cmsUInt32Number>(profile.size()));
    if (!in) return nullptr;
    if (cmsGetColorSpace(in) != cmsSigRgbData)
    {
        cmsCloseProfile(in);
        return nullptr;
    }
    cmsHPROFILE out = cmsCreate_sRGBProfile();
    cmsHTRANSFORM xf = cmsCreateTransform(in, TYPE_RGBA_8, out, TYPE_RGBA_8, INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA);
    cmsCloseProfile(in);
    cmsCloseProfile(out);
    if (!xf) return nullptr;

    uint8_t probe[8][4], probeOut[8][4];
    const uint8_t samples[8][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
                                   {0, 255, 255}, {128, 128, 128}, {200, 100, 50}, {20, 60, 220}};
    for (int i = 0; i < 8; ++i) probe[i][0] = samples[i][0], probe[i][1] = samples[i][1], probe[i][2] = samples[i][2], probe[i][3] = 255;
    cmsDoTransform(xf, probe, probeOut, 8);
    int maxDiff = 0;
    for (int i = 0; i < 8; ++i)
        for (int c = 0; c < 3; ++c) maxDiff = std::max(maxDiff, std::abs(int(probe[i][c]) - int(probeOut[i][c])));
    if (maxDiff <= 1)
    {
        cmsDeleteTransform(xf);
        return nullptr;
    }
    return xf;
}
} // namespace

bool iccConversionNeeded(const std::vector<uint8_t>& profile)
{
    cmsHTRANSFORM xf = makeSrgbTransform(profile);
    if (!xf) return false;
    cmsDeleteTransform(xf);
    return true;
}

bool convertRgba8ToSrgb(const std::vector<uint8_t>& profile, uint8_t* rgba, size_t pixelCount)
{
    if (!rgba || pixelCount == 0) return false;
    cmsHTRANSFORM xf = makeSrgbTransform(profile);
    if (!xf) return false;

    const size_t threads = pixelCount < (1u << 18) ? 1 : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    const size_t chunk = (pixelCount + threads - 1) / threads;
    for (size_t t = 0; t < threads; ++t)
    {
        const size_t begin = t * chunk, end = std::min(pixelCount, begin + chunk);
        if (begin >= end) break;
        auto work = [=] { cmsDoTransform(xf, rgba + begin * 4, rgba + begin * 4, static_cast<cmsUInt32Number>(end - begin)); };
        if (threads == 1) work();
        else pool.emplace_back(work);
    }
    for (auto& th : pool) th.join();
    cmsDeleteTransform(xf);
    return true;
}

#else

bool iccConversionAvailable() { return false; }
std::string iccDescription(const std::vector<uint8_t>&) { return {}; }
bool iccConversionNeeded(const std::vector<uint8_t>&) { return false; }
bool convertRgba8ToSrgb(const std::vector<uint8_t>&, uint8_t*, size_t) { return false; }

#endif

} // namespace giv
