#include "ExifReader.h"

#include <algorithm>
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

} // namespace

std::vector<ExifEntry> readExif(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(f.tellg());

    uint8_t magic[12];
    if (!readAt(f, 0, magic, 12)) return {};

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
