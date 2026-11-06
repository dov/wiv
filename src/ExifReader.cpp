// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ExifReader.h"
#include "PathUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace giv
{

namespace
{

// Random-access byte source: either the whole file (TIFF, whose IFDs can sit
// anywhere) or an extracted EXIF blob. All offsets are relative to `base`.
struct Source
{
    std::function<bool(uint64_t, void*, size_t)> read;
    uint64_t size = 0;
};

Source fileSource(std::ifstream& f, uint64_t size)
{
    Source s;
    s.size = size;
    s.read = [&f, size](uint64_t off, void* dst, size_t n) {
        if (off + n > size) return false;
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<size_t>(f.gcount()) == n;
    };
    return s;
}

Source blobSource(const std::vector<uint8_t>& blob)
{
    Source s;
    s.size = blob.size();
    s.read = [&blob](uint64_t off, void* dst, size_t n) {
        if (off + n > blob.size()) return false;
        std::memcpy(dst, blob.data() + off, n);
        return true;
    };
    return s;
}

struct Reader
{
    bool be; // big-endian ("MM")

    uint16_t u16(const uint8_t* p) const { return be ? uint16_t(p[0] << 8 | p[1]) : uint16_t(p[1] << 8 | p[0]); }
    uint32_t u32(const uint8_t* p) const
    {
        return be ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
                  : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
    }
};

enum Type { BYTE = 1, ASCII = 2, SHORT = 3, LONG = 4, RATIONAL = 5, SBYTE = 6, UNDEF = 7, SSHORT = 8, SLONG = 9, SRATIONAL = 10, FLOAT = 11, DOUBLE = 12 };

size_t typeSize(int t)
{
    switch (t)
    {
    case BYTE: case ASCII: case SBYTE: case UNDEF: return 1;
    case SHORT: case SSHORT: return 2;
    case LONG: case SLONG: case FLOAT: return 4;
    case RATIONAL: case SRATIONAL: case DOUBLE: return 8;
    }
    return 0;
}

// One decoded tag: raw bytes plus enough to pull numbers out of it.
struct Tag
{
    int type = 0;
    uint32_t count = 0;
    std::vector<uint8_t> data;
    bool be = false;

    double num(uint32_t i) const
    {
        const size_t sz = typeSize(type);
        if (i >= count || (i + 1) * sz > data.size()) return 0;
        Reader r{be};
        const uint8_t* p = data.data() + i * sz;
        auto u32 = [&](const uint8_t* q) { return r.u32(q); };
        switch (type)
        {
        case BYTE: case UNDEF: return p[0];
        case SBYTE: return int8_t(p[0]);
        case SHORT: return r.u16(p);
        case SSHORT: return int16_t(r.u16(p));
        case LONG: return u32(p);
        case SLONG: return int32_t(u32(p));
        case RATIONAL: { uint32_t d = u32(p + 4); return d ? double(u32(p)) / d : 0; }
        case SRATIONAL: { int32_t d = int32_t(u32(p + 4)); return d ? double(int32_t(u32(p))) / d : 0; }
        case FLOAT: { uint32_t v = u32(p); float f; std::memcpy(&f, &v, 4); return f; }
        }
        return 0;
    }
    // Numerator/denominator of rational i (for "1/250"-style exposure times).
    bool rational(uint32_t i, uint32_t& n, uint32_t& d) const
    {
        if ((type != RATIONAL && type != SRATIONAL) || (i + 1) * 8 > data.size()) return false;
        Reader r{be};
        n = r.u32(data.data() + i * 8);
        d = r.u32(data.data() + i * 8 + 4);
        return d != 0;
    }
    std::string str() const
    {
        std::string s(data.begin(), data.end());
        const size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
        return s;
    }
};

using TagMap = std::map<uint16_t, Tag>;

// Reads the IFD at `offset` into `out`; remembers the Exif/GPS sub-IFD pointers.
void readIfd(const Source& src, const Reader& r, uint64_t offset, TagMap& out, std::set<uint64_t>& visited)
{
    if (!visited.insert(offset).second) return;
    uint8_t cnt[2];
    if (!src.read(offset, cnt, 2)) return;
    const uint32_t n = r.u16(cnt);
    if (n > 4096) return;
    std::vector<uint8_t> raw(size_t(n) * 12);
    if (!src.read(offset + 2, raw.data(), raw.size())) return;
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint8_t* e = raw.data() + i * 12;
        Tag t;
        t.be = r.be;
        const uint16_t id = r.u16(e);
        t.type = r.u16(e + 2);
        t.count = r.u32(e + 4);
        const size_t sz = typeSize(t.type);
        if (sz == 0) continue;
        const uint64_t total = uint64_t(sz) * t.count;
        if (total > 65536) continue; // maker notes, thumbnails, ...
        t.data.resize(size_t(total));
        if (total <= 4)
            std::memcpy(t.data.data(), e + 8, size_t(total));
        else if (!src.read(r.u32(e + 8), t.data.data(), size_t(total)))
            continue;
        out[id] = std::move(t);
    }
}

std::string fmt(const char* f, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string enumName(int v, const std::map<int, const char*>& names)
{
    auto it = names.find(v);
    return it != names.end() ? it->second : "(" + std::to_string(v) + ")";
}

std::string gpsCoord(const Tag& t, const std::string& ref)
{
    if (t.count < 3) return {};
    const double v = t.num(0) + t.num(1) / 60.0 + t.num(2) / 3600.0;
    return fmt("%.6f", v) + "° " + ref + " (" + std::to_string(int(t.num(0))) + "° " +
           std::to_string(int(t.num(1))) + "' " + fmt("%.2f", t.num(2)) + "\" " + ref + ")";
}

std::vector<ExifEntry> decode(const Source& src, uint64_t tiffBase)
{
    // `src` is positioned so tiffBase is the TIFF header ("II"/"MM").
    std::vector<ExifEntry> out;
    uint8_t hdr[8];
    if (!src.read(tiffBase, hdr, 8)) return out;
    const bool be = hdr[0] == 'M' && hdr[1] == 'M';
    if (!be && !(hdr[0] == 'I' && hdr[1] == 'I')) return out;
    Reader r{be};
    if (r.u16(hdr + 2) != 42) return out;

    // Shift every offset by tiffBase so the IFD readers can use header-relative ones.
    Source rel;
    rel.size = src.size - tiffBase;
    rel.read = [&src, tiffBase](uint64_t off, void* dst, size_t n) { return src.read(tiffBase + off, dst, n); };

    TagMap ifd0, exif, gps;
    std::set<uint64_t> visited;
    readIfd(rel, r, r.u32(hdr + 4), ifd0, visited);
    if (auto it = ifd0.find(0x8769); it != ifd0.end()) readIfd(rel, r, uint64_t(it->second.num(0)), exif, visited);
    if (auto it = ifd0.find(0x8825); it != ifd0.end()) readIfd(rel, r, uint64_t(it->second.num(0)), gps, visited);

    auto add = [&](const std::string& name, const std::string& value) {
        if (!value.empty()) out.push_back({name, value});
    };
    auto str = [](const TagMap& m, uint16_t id) {
        auto it = m.find(id);
        return it != m.end() && it->second.type == ASCII ? it->second.str() : std::string();
    };
    auto numOf = [](const TagMap& m, uint16_t id, double& v) {
        auto it = m.find(id);
        if (it == m.end() || it->second.count == 0) return false;
        v = it->second.num(0);
        return true;
    };

    // ---- Camera / image ----
    add("Description", str(ifd0, 0x010E));
    add("Camera make", str(ifd0, 0x010F));
    add("Camera model", str(ifd0, 0x0110));
    add("Lens make", str(exif, 0xA433));
    add("Lens model", str(exif, 0xA434));
    add("Software", str(ifd0, 0x0131));
    add("Artist", str(ifd0, 0x013B));
    add("Copyright", str(ifd0, 0x8298));
    double v;
    if (numOf(ifd0, 0x0112, v))
        add("Orientation", enumName(int(v), {{1, "Normal"}, {2, "Mirrored"}, {3, "Rotated 180°"}, {4, "Mirrored vertically"},
                                              {5, "Mirrored, rotated 270° CW"}, {6, "Rotated 90° CW"},
                                              {7, "Mirrored, rotated 90° CW"}, {8, "Rotated 270° CW"}}));
    add("Date taken", str(exif, 0x9003));
    add("Date digitized", str(exif, 0x9004));
    add("Date modified", str(ifd0, 0x0132));

    // ---- Exposure ----
    {
        auto it = exif.find(0x829A);
        uint32_t n, d;
        if (it != exif.end() && it->second.rational(0, n, d))
            add("Exposure time", (n && d > n ? "1/" + std::to_string(uint32_t(std::lround(double(d) / n)))
                                              : fmt("%.4g", double(n) / d)) + " s");
    }
    if (numOf(exif, 0x829D, v)) add("Aperture", "f/" + fmt("%.1f", v));
    if (numOf(exif, 0x8827, v)) add("ISO", std::to_string(int(v)));
    if (numOf(exif, 0x920A, v))
    {
        std::string s = fmt("%.1f", v) + " mm";
        double v35;
        if (numOf(exif, 0xA405, v35) && v35 > 0) s += " (" + std::to_string(int(v35)) + " mm equiv.)";
        add("Focal length", s);
    }
    if (numOf(exif, 0x9204, v)) add("Exposure bias", (v > 0 ? "+" : "") + fmt("%.2g", v) + " EV");
    if (numOf(exif, 0x8822, v))
        add("Exposure program", enumName(int(v), {{0, "Not defined"}, {1, "Manual"}, {2, "Normal"}, {3, "Aperture priority"},
                                                   {4, "Shutter priority"}, {5, "Creative"}, {6, "Action"},
                                                   {7, "Portrait"}, {8, "Landscape"}}));
    if (numOf(exif, 0x9207, v))
        add("Metering mode", enumName(int(v), {{0, "Unknown"}, {1, "Average"}, {2, "Center-weighted"}, {3, "Spot"},
                                                {4, "Multi-spot"}, {5, "Pattern"}, {6, "Partial"}, {255, "Other"}}));
    if (numOf(exif, 0x9209, v)) add("Flash", (int(v) & 1) ? "Fired" : "Did not fire");
    if (numOf(exif, 0xA403, v)) add("White balance", int(v) == 0 ? "Auto" : "Manual");
    if (numOf(exif, 0xA001, v))
        add("Color space", int(v) == 1 ? "sRGB" : int(v) == 0xFFFF ? "Uncalibrated" : "(" + std::to_string(int(v)) + ")");
    {
        double w, h;
        if (numOf(exif, 0xA002, w) && numOf(exif, 0xA003, h)) add("EXIF size", std::to_string(int(w)) + "×" + std::to_string(int(h)));
    }
    {
        auto it = exif.find(0x9286); // UserComment: 8-byte charset prefix, then text
        if (it != exif.end() && it->second.data.size() > 8)
        {
            std::string c(it->second.data.begin() + 8, it->second.data.end());
            c.erase(std::find(c.begin(), c.end(), '\0'), c.end());
            add("Comment", c);
        }
    }

    // ---- GPS ----
    {
        auto lat = gps.find(2), lon = gps.find(4);
        if (lat != gps.end()) add("GPS latitude", gpsCoord(lat->second, str(gps, 1)));
        if (lon != gps.end()) add("GPS longitude", gpsCoord(lon->second, str(gps, 3)));
        if (numOf(gps, 6, v))
        {
            double ref = 0;
            numOf(gps, 5, ref);
            add("GPS altitude", fmt("%.1f", ref == 1 ? -v : v) + " m");
        }
    }
    return out;
}

bool readAt(std::ifstream& f, uint64_t off, void* dst, size_t n)
{
    f.clear();
    f.seekg(static_cast<std::streamoff>(off));
    f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount()) == n;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

// Blob that holds a TIFF header at offset 0 (possibly after a "Exif\0\0" prefix).
std::vector<ExifEntry> decodeBlob(std::vector<uint8_t> blob)
{
    size_t skip = 0;
    if (blob.size() >= 6 && std::memcmp(blob.data(), "Exif\0\0", 6) == 0) skip = 6;
    if (blob.size() <= skip) return {};
    blob.erase(blob.begin(), blob.begin() + skip);
    return decode(blobSource(blob), 0);
}


// ---- Header metadata of the float formats (no EXIF there) ----

std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

std::string fmtNum(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

// Radiance RGBE: "#?RADIANCE", KEY=value lines, '#' comments, blank line,
// then the resolution line.
std::vector<ExifEntry> readHdrHeader(std::ifstream& f)
{
    std::vector<ExifEntry> out;
    f.clear();
    f.seekg(0);
    std::string line;
    if (!std::getline(f, line) || line.compare(0, 2, "#?") != 0) return {};
    out.push_back({"Format", "Radiance RGBE (" + trimmed(line.substr(2)) + ")"});
    for (int guard = 0; guard < 1000 && std::getline(f, line); ++guard)
    {
        line = trimmed(line);
        if (line.empty())
        {
            std::string res;
            if (std::getline(f, res)) out.push_back({"Resolution / axes", trimmed(res)});
            break;
        }
        if (line[0] == '#') { out.push_back({"Comment", trimmed(line.substr(1))}); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.push_back({line.substr(0, eq), trimmed(line.substr(eq + 1))});
    }
    return out;
}

std::vector<ExifEntry> readPfmHeader(std::ifstream& f)
{
    f.clear();
    f.seekg(0);
    std::string type;
    double scale = 0;
    int w = 0, h = 0;
    if (!(f >> type >> w >> h >> scale) || (type != "PF" && type != "Pf")) return {};
    return {{"Format", type == "PF" ? "Portable Float Map, RGB" : "Portable Float Map, grayscale"},
            {"Byte order", scale < 0 ? "little-endian" : "big-endian"},
            {"Scale", fmtNum(std::fabs(scale))}};
}

// OpenEXR: magic, version word, then (name, type, size, value) attributes
// until an empty name. Only the first part's header is read.
std::vector<ExifEntry> readExrHeader(std::ifstream& f, uint64_t size)
{
    std::vector<ExifEntry> out;
    uint8_t h[8];
    if (!readAt(f, 0, h, 8) || h[0] != 0x76 || h[1] != 0x2f || h[2] != 0x31 || h[3] != 1) return {};
    const uint32_t flags = le32(h + 4);
    out.push_back({"Format", std::string("OpenEXR") + ((flags & 0x200) ? ", tiled" : ", scanline") +
                                 ((flags & 0x800) ? ", deep" : "") + ((flags & 0x1000) ? ", multipart" : "")});

    uint64_t off = 8;
    auto readStr = [&](std::string& s) {
        s.clear();
        for (int i = 0; i < 256; ++i)
        {
            uint8_t c;
            if (!readAt(f, off++, &c, 1)) return false;
            if (c == 0) return true;
            s.push_back(char(c));
        }
        return false;
    };
    auto i32 = [](const uint8_t* p) { return int32_t(le32(p)); };
    auto f32 = [](const uint8_t* p) { float v; uint32_t u = le32(p); std::memcpy(&v, &u, 4); return v; };
    static const char* kCompression[] = {"none", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A", "DWAA", "DWAB"};
    static const char* kPixelType[] = {"UINT", "HALF", "FLOAT"};

    for (int guard = 0; guard < 512; ++guard)
    {
        std::string name, type;
        if (!readStr(name) || name.empty()) break;
        uint8_t sz[4];
        if (!readStr(type) || !readAt(f, off, sz, 4)) break;
        off += 4;
        const uint32_t n = le32(sz);
        if (n > (1u << 20) || off + n > size) break;
        std::vector<uint8_t> v(n);
        if (n && !readAt(f, off, v.data(), n)) break;
        off += n;

        std::string val;
        if (type == "string") val.assign(v.begin(), v.end());
        else if (type == "int" && n == 4) val = std::to_string(i32(v.data()));
        else if (type == "float" && n == 4) val = fmtNum(f32(v.data()));
        else if (type == "double" && n == 8) { double d; std::memcpy(&d, v.data(), 8); val = fmtNum(d); }
        else if (type == "v2i" && n == 8) val = std::to_string(i32(v.data())) + ", " + std::to_string(i32(v.data() + 4));
        else if (type == "v2f" && n == 8) val = fmtNum(f32(v.data())) + ", " + fmtNum(f32(v.data() + 4));
        else if (type == "box2i" && n == 16)
            val = "(" + std::to_string(i32(v.data())) + ", " + std::to_string(i32(v.data() + 4)) + ") - (" +
                  std::to_string(i32(v.data() + 8)) + ", " + std::to_string(i32(v.data() + 12)) + ")";
        else if (type == "compression" && n == 1) val = v[0] < 10 ? kCompression[v[0]] : std::to_string(v[0]);
        else if (type == "lineOrder" && n == 1) val = v[0] == 0 ? "increasing Y" : v[0] == 1 ? "decreasing Y" : "random Y";
        else if (type == "rational" && n == 8)
            val = std::to_string(i32(v.data())) + "/" + std::to_string(le32(v.data() + 4));
        else if (type == "chromaticities" && n == 32)
        {
            static const char* lbl[] = {"R", "G", "B", "W"};
            for (int i = 0; i < 4; ++i)
                val += std::string(i ? "  " : "") + lbl[i] + "(" + fmtNum(f32(v.data() + i * 8)) + ", " + fmtNum(f32(v.data() + i * 8 + 4)) + ")";
        }
        else if (type == "chlist")
        {
            size_t p = 0;
            while (p < v.size() && v[p])
            {
                const size_t e = std::find(v.begin() + p, v.end(), uint8_t(0)) - v.begin();
                if (e + 17 > v.size()) break;
                const uint32_t pt = le32(v.data() + e + 1);
                if (!val.empty()) val += ", ";
                val += std::string(v.begin() + p, v.begin() + e) + " (" + (pt < 3 ? kPixelType[pt] : "?") + ")";
                p = e + 17;
            }
        }
        else continue; // opaque/unknown attribute types (tiles, previews, ...)
        out.push_back({name, trimmed(val)});
    }
    return out;
}

} // namespace

std::vector<ExifEntry> readExif(const std::string& path)
{
    std::ifstream f(giv::utf8ToPath(path), std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(f.tellg());

    uint8_t magic[12];
    if (!readAt(f, 0, magic, 12)) return {};

    // ---- Float formats: header text rather than EXIF ----
    if (magic[0] == '#' && magic[1] == '?') return readHdrHeader(f);
    if (magic[0] == 'P' && (magic[1] == 'F' || magic[1] == 'f') && std::isspace(magic[2])) return readPfmHeader(f);
    if (magic[0] == 0x76 && magic[1] == 0x2f && magic[2] == 0x31 && magic[3] == 1) return readExrHeader(f, size);

    // ---- TIFF ----
    if ((magic[0] == 'I' && magic[1] == 'I' && magic[2] == 42 && magic[3] == 0) ||
        (magic[0] == 'M' && magic[1] == 'M' && magic[2] == 0 && magic[3] == 42))
        return decode(fileSource(f, size), 0);

    // ---- JPEG: scan marker segments for APP1 "Exif" ----
    if (magic[0] == 0xFF && magic[1] == 0xD8)
    {
        uint64_t off = 2;
        for (int guard = 0; guard < 64; ++guard)
        {
            uint8_t h[4];
            if (!readAt(f, off, h, 4) || h[0] != 0xFF) break;
            const uint8_t marker = h[1];
            if (marker == 0xDA || marker == 0xD9) break; // start of scan / EOI
            const uint32_t len = uint32_t(h[2]) << 8 | h[3];
            if (len < 2) break;
            if (marker == 0xE1 && len > 8)
            {
                std::vector<uint8_t> blob(len - 2);
                if (readAt(f, off + 4, blob.data(), blob.size()) && blob.size() >= 6 && std::memcmp(blob.data(), "Exif\0\0", 6) == 0)
                    return decodeBlob(std::move(blob));
            }
            off += 2 + len;
        }
        return {};
    }

    // ---- PNG: eXIf chunk ----
    static const uint8_t pngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(magic, pngSig, 8) == 0)
    {
        uint64_t off = 8;
        for (int guard = 0; guard < 4096; ++guard)
        {
            uint8_t h[8];
            if (!readAt(f, off, h, 8)) break;
            const uint32_t len = be32(h);
            if (std::memcmp(h + 4, "eXIf", 4) == 0 && len < (1u << 20))
            {
                std::vector<uint8_t> blob(len);
                if (readAt(f, off + 8, blob.data(), len)) return decodeBlob(std::move(blob));
                break;
            }
            if (std::memcmp(h + 4, "IEND", 4) == 0) break;
            off += 12ull + len; // length + type + data + crc
        }
        return {};
    }

    // ---- WebP: RIFF "EXIF" chunk ----
    if (std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "WEBP", 4) == 0)
    {
        uint64_t off = 12;
        for (int guard = 0; guard < 4096; ++guard)
        {
            uint8_t h[8];
            if (!readAt(f, off, h, 8)) break;
            const uint32_t len = le32(h + 4);
            if (std::memcmp(h, "EXIF", 4) == 0 && len < (1u << 20))
            {
                std::vector<uint8_t> blob(len);
                if (readAt(f, off + 8, blob.data(), len)) return decodeBlob(std::move(blob));
                break;
            }
            off += 8ull + len + (len & 1); // chunks are padded to even size
        }
    }
    return {};
}

} // namespace giv
