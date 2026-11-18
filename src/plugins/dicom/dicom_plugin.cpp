// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// dicom_plugin.cpp - wiv image plugin for a minimal subset of DICOM: an
// uncompressed, single-sample-per-pixel (grayscale) 8- or 16-bit dataset,
// Explicit VR Little Endian (the overwhelmingly common transfer syntax for
// simple CT/MR/US exports). Modeled on giv's standalone parser
// (~/hd/github/giv/src/plugins/simple_dicom.c - not the dcmtk-based
// dicom.cc, to avoid pulling in dcmtk as a new dependency), with its
// `return =` typo fixed and its ad-hoc "does this look like a VR code"
// heuristic for implicit-VR/undictioned tags kept as-is (same heuristic
// giv used). Multi-frame datasets (NumberOfFrames, tag 0028,0008) are
// exposed as a multi-slice image (WivPluginImage::depth) - giv's simple
// parser didn't read that tag at all; only its dcmtk-based loader did.
//
#include "../wiv_plugin_common.h"
#include "dicom_tags.h"

#ifndef _WIN32
#include <iconv.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{

bool hasExtension(const std::string& filename, const char* ext)
{
    size_t n = std::strlen(ext);
    if (filename.size() < n)
        return false;
    std::string tail = filename.substr(filename.size() - n);
    for (char& c : tail)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ext;
}

uint16_t readU16(FILE* fh, bool* ok)
{
    uint16_t v = 0;
    if (std::fread(&v, 1, 2, fh) != 2)
        *ok = false;
    return v; // little-endian file, assumed little-endian host
}

uint32_t readU32(FILE* fh, bool* ok)
{
    uint32_t v = 0;
    if (std::fread(&v, 1, 4, fh) != 4)
        *ok = false;
    return v;
}

// A DICOM data element's value representation is either a genuine 2-letter
// explicit-VR code (both bytes uppercase A-Z, e.g. "US", "OB"), or - for
// implicit-VR-encoded elements - the first two bytes of a 4-byte length
// field instead. giv's heuristic (kept as-is here): if it doesn't look like
// two uppercase letters, assume implicit VR and re-interpret accordingly.
bool looksLikeVrCode(const char vr[2])
{
    return vr[0] >= 'A' && vr[0] <= 'Z' && vr[1] >= 'A' && vr[1] <= 'Z';
}

bool isLongLengthVr(const char vr[2])
{
    return std::memcmp(vr, "OB", 2) == 0 || std::memcmp(vr, "OW", 2) == 0 ||
           std::memcmp(vr, "SQ", 2) == 0 || std::memcmp(vr, "UN", 2) == 0;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasExtension(filename, ".dcm");
}

// Content check for extensionless/oddly named files (e.g. "MR.NO.EXTENSION"):
// a DICOM Part 10 file has "DICM" at offset 128.
extern "C" bool wiv_plugin_sniff_file(const char* filename)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
        return false;
    char preamble[132];
    bool match = std::fread(preamble, 1, sizeof(preamble), fh) == sizeof(preamble) &&
                 std::memcmp(preamble + 128, "DICM", 4) == 0;
    std::fclose(fh);
    return match;
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("dicom: failed to open %s", filename);
        return nullptr;
    }

    char preamble[132];
    bool ok = std::fread(preamble, 1, sizeof(preamble), fh) == sizeof(preamble);
    if (!ok || std::memcmp(preamble + 128, "DICM", 4) != 0)
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("dicom: not a DICOM file (missing DICM magic): %s", filename);
        return nullptr;
    }

    int width = 0, height = 0, samplesPerPixel = 0, bitsAllocated = 0, numFrames = 1;
    std::vector<unsigned char> pixelData;

    while (ok)
    {
        uint16_t groupWord = readU16(fh, &ok);
        if (!ok)
            break; // clean EOF between elements
        uint16_t elementWord = readU16(fh, &ok);
        char vr[2];
        if (!ok || std::fread(vr, 1, 2, fh) != 2)
            break;

        uint32_t elementLength;
        if (looksLikeVrCode(vr))
        {
            if (isLongLengthVr(vr))
            {
                uint16_t reserved = readU16(fh, &ok); // skip 2 reserved bytes
                (void)reserved;
                elementLength = readU32(fh, &ok);
            }
            else
            {
                elementLength = readU16(fh, &ok);
            }
        }
        else
        {
            // Implicit VR: the 2 bytes just read (misread as `vr`) are the
            // low half of a 4-byte length field - read the other 2.
            unsigned char lenBytes[4] = {static_cast<unsigned char>(vr[0]), static_cast<unsigned char>(vr[1]), 0, 0};
            if (std::fread(lenBytes + 2, 1, 2, fh) != 2)
                ok = false;
            std::memcpy(&elementLength, lenBytes, 4);
        }
        if (!ok)
            break;

        if (elementLength == 0xffffffffu || (groupWord == 0xfffe && elementWord == 0xe000))
            continue; // sequence-of-items delimiter/item tag - not supported, skip

        std::vector<unsigned char> value(elementLength + 1, 0);
        if (elementLength > 0 && std::fread(value.data(), 1, elementLength, fh) != elementLength)
        {
            ok = false;
            break;
        }

        const uint16_t valueU16 = elementLength >= 2 ? (value[0] | (value[1] << 8)) : 0;

        if (groupWord == 0x0028)
        {
            switch (elementWord)
            {
                case 0x0002: samplesPerPixel = valueU16; break;
                case 0x0008: // NumberOfFrames (IS - ASCII integer string)
                    numFrames = std::max(1, std::atoi(reinterpret_cast<const char*>(value.data())));
                    break;
                case 0x0010: height = valueU16; break;
                case 0x0011: width = valueU16; break;
                case 0x0100: bitsAllocated = valueU16; break;
            }
        }
        else if (groupWord == 0x7fe0 && elementWord == 0x0010)
        {
            pixelData = std::move(value);
        }
    }
    std::fclose(fh);

    if (width <= 0 || height <= 0 || (bitsAllocated != 8 && bitsAllocated != 16) || samplesPerPixel != 1)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError(
                "dicom: unsupported dataset (need 8/16-bit single-sample pixels): %s", filename);
        return nullptr;
    }

    const size_t bytesPerSample = static_cast<size_t>(bitsAllocated) / 8;
    const size_t sliceElemCount = static_cast<size_t>(width) * height;
    const size_t neededBytes = sliceElemCount * bytesPerSample * static_cast<size_t>(numFrames);
    if (pixelData.size() < neededBytes)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("dicom: pixel data shorter than Rows*Columns*Frames in %s", filename);
        return nullptr;
    }

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    const WivSampleType sampleType = bitsAllocated == 16 ? WIV_SAMPLE_U16 : WIV_SAMPLE_U8;
    wiv_plugin::allocSamples(img, sampleType, numFrames);
    std::memcpy(img->samples, pixelData.data(), sliceElemCount * bytesPerSample * static_cast<size_t>(numFrames));

    // Slice-0 grayscale preview - never actually displayed for a raw-sample
    // image (see DisplayImage.h), just a reasonable placeholder.
    for (size_t i = 0; i < sliceElemCount; ++i)
    {
        uint8_t g = bitsAllocated == 8 ? static_cast<const uint8_t*>(img->samples)[i]
                                       : static_cast<uint8_t>(static_cast<const uint16_t*>(img->samples)[i] >> 8);
        img->rgba[i * 4 + 0] = g;
        img->rgba[i * 4 + 1] = g;
        img->rgba[i * 4 + 2] = g;
        img->rgba[i * 4 + 3] = 255;
    }

    return img;
}

// ===========================================================================
// Metadata (File > Image Info)
// ===========================================================================
//
// A tolerant walk over the whole dataset that names every element it can. It
// understands the three uncompressed-header transfer syntaxes (implicit LE,
// explicit LE, explicit BE; the encapsulated/compressed syntaxes use explicit
// LE for everything but the pixel data), nested sequences (shown indented with
// "> "), and seeks over large binary values such as the pixel data instead of
// reading them.

namespace
{

using dicom_tags::findTag;

int64_t tellPos(FILE* f)
{
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}
bool seekPos(FILE* f, int64_t pos)
{
#ifdef _WIN32
    return _fseeki64(f, pos, SEEK_SET) == 0;
#else
    return fseeko(f, pos, SEEK_SET) == 0;
#endif
}


// ---- Specific Character Set (0008,0005): text in ISO 8859-x / UTF-8 / Thai / GB, shown as UTF-8 ----

struct Charset
{
    std::string iconvName; // Linux/macOS
    unsigned codePage = 0; // Windows; 65001 = UTF-8 (passthrough), 0 = plain ASCII
    bool isLatin1 = false;
    bool active() const { return isLatin1 || !iconvName.empty(); }
};

// The first value of a Specific Character Set; a code-extension (ISO 2022) list falls back to its first entry.
Charset charsetFor(std::string term)
{
    // "\\"-separated: take the first non-empty term.
    std::string first;
    size_t pos = 0;
    while (pos <= term.size())
    {
        size_t e = term.find('\\', pos);
        if (e == std::string::npos) e = term.size();
        std::string part = term.substr(pos, e - pos);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) { first = part; break; }
        pos = e + 1;
    }
    // "ISO 2022 IR n" uses the same repertoire as "ISO_IR n" for the single-byte sets.
    if (first.rfind("ISO 2022 IR ", 0) == 0) first = "ISO_IR " + first.substr(12);
    struct M { const char* term; const char* iconv; unsigned cp; };
    static const M table[] = {
        {"ISO_IR 100", "ISO-8859-1", 28591}, {"ISO_IR 101", "ISO-8859-2", 28592}, {"ISO_IR 109", "ISO-8859-3", 28593},
        {"ISO_IR 110", "ISO-8859-4", 28594}, {"ISO_IR 144", "ISO-8859-5", 28595}, {"ISO_IR 127", "ISO-8859-6", 28596},
        {"ISO_IR 126", "ISO-8859-7", 28597}, {"ISO_IR 138", "ISO-8859-8", 28598}, {"ISO_IR 148", "ISO-8859-9", 28599},
        {"ISO_IR 166", "TIS-620", 874},      {"ISO_IR 13", "SHIFT_JIS", 932},     {"GB18030", "GB18030", 54936},
        {"GBK", "GBK", 936},
    };
    Charset cs;
    if (first == "ISO_IR 192") { cs.codePage = 65001; return cs; } // UTF-8: nothing to do
    if (first == "ISO_IR 100") { cs.isLatin1 = true; cs.iconvName = "ISO-8859-1"; cs.codePage = 28591; return cs; }
    for (const M& m : table)
        if (first == m.term) { cs.iconvName = m.iconv; cs.codePage = m.cp; return cs; }
    return cs; // ASCII / unsupported: bytes are left as they are
}

// Replaces invalid UTF-8 (bytes that no known character set could place) so the panel never shows mojibake.
std::string sanitizeUtf8(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (ok) { out.append(s, i, n); i += n; }
        else { out += "\xEF\xBF\xBD"; ++i; }
    }
    return out;
}

std::string toUtf8(const std::string& in, const Charset& cs)
{
    bool ascii = true;
    for (unsigned char ch : in) if (ch >= 0x80) { ascii = false; break; }
    if (ascii) return in;
    if (cs.codePage == 65001 && !cs.active()) return sanitizeUtf8(in);
    if (cs.isLatin1) // ISO 8859-1 is the first 256 code points
    {
        std::string out;
        for (unsigned char ch : in) { if (ch < 0x80) out += char(ch); else { out += char(0xC0 | (ch >> 6)); out += char(0x80 | (ch & 0x3F)); } }
        return out;
    }
    if (!cs.active()) return sanitizeUtf8(in);
#ifdef _WIN32
    if (cs.codePage)
    {
        const int wn = MultiByteToWideChar(cs.codePage, 0, in.data(), int(in.size()), nullptr, 0);
        if (wn > 0)
        {
            std::wstring w(size_t(wn), L'\0');
            MultiByteToWideChar(cs.codePage, 0, in.data(), int(in.size()), &w[0], wn);
            const int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
            if (un > 0)
            {
                std::string out(size_t(un), '\0');
                WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &out[0], un, nullptr, nullptr);
                return out;
            }
        }
    }
    return sanitizeUtf8(in);
#else
    iconv_t cd = iconv_open("UTF-8", cs.iconvName.c_str());
    if (cd == reinterpret_cast<iconv_t>(-1)) return sanitizeUtf8(in);
    std::string out(in.size() * 4 + 8, '\0');
    char* src = const_cast<char*>(in.data());
    size_t srcLeft = in.size();
    char* dst = &out[0];
    size_t dstLeft = out.size();
    const size_t r = iconv(cd, &src, &srcLeft, &dst, &dstLeft);
    iconv_close(cd);
    if (r == static_cast<size_t>(-1) && srcLeft > 0) return sanitizeUtf8(in);
    out.resize(out.size() - dstLeft);
    return sanitizeUtf8(out);
#endif
}

bool isLocalizedTextVr(const char* vr)
{
    static const char* t[] = {"SH", "LO", "ST", "LT", "UT", "PN", "UC"};
    for (const char* x : t)
        if (std::memcmp(vr, x, 2) == 0) return true;
    return false;
}

struct MetaEntry
{
    std::string section, key, value;
};

struct DumpCtx
{
    FILE* f = nullptr;
    bool bigEndian = false;
    bool implicitVr = false;
    Charset charset;
    int64_t fileSize = 0;
    std::vector<MetaEntry> entries;
    std::map<uint32_t, std::string> top; // top-level element values by tag, for the summary
    bool truncated = false;
    bool stop = false; // pixel data reached with an undefined length, or a read error
    static constexpr size_t kMaxEntries = 4000;
    static constexpr int kMaxDepth = 6;
    static constexpr int kMaxItems = 8; // items shown per sequence
};

bool readRaw(DumpCtx& c, void* dst, size_t n)
{
    return std::fread(dst, 1, n, c.f) == n;
}
bool readTag16(DumpCtx& c, uint16_t& v, bool bigEndian)
{
    uint8_t b[2];
    if (!readRaw(c, b, 2)) return false;
    v = bigEndian ? uint16_t(b[0] << 8 | b[1]) : uint16_t(b[1] << 8 | b[0]);
    return true;
}
bool readTag32(DumpCtx& c, uint32_t& v, bool bigEndian)
{
    uint8_t b[4];
    if (!readRaw(c, b, 4)) return false;
    v = bigEndian ? (uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3])
                  : (uint32_t(b[3]) << 24 | uint32_t(b[2]) << 16 | uint32_t(b[1]) << 8 | b[0]);
    return true;
}

std::string hex4(unsigned v)
{
    char b[8];
    std::snprintf(b, sizeof(b), "%04X", v);
    return b;
}
std::string tagText(uint32_t tag) { return "(" + hex4(tag >> 16) + "," + hex4(tag & 0xFFFF) + ")"; }

bool isVrCode(const char* vr) { return vr[0] >= 'A' && vr[0] <= 'Z' && vr[1] >= 'A' && vr[1] <= 'Z'; }

bool isLongVr(const char* vr)
{
    static const char* longVrs[] = {"OB", "OD", "OF", "OL", "OV", "OW", "SQ", "UC", "UR", "UT", "UN"};
    for (const char* l : longVrs)
        if (std::memcmp(vr, l, 2) == 0) return true;
    return false;
}

bool isTextVr(const char* vr)
{
    static const char* t[] = {"AE", "AS", "CS", "DA", "DS", "DT", "IS", "LO", "LT", "PN", "SH", "ST", "TM", "UC", "UI", "UR", "UT"};
    for (const char* x : t)
        if (std::memcmp(vr, x, 2) == 0) return true;
    return false;
}

std::string trimText(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

bool mostlyPrintable(const std::vector<uint8_t>& v, size_t n)
{
    if (n == 0) return false;
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i)
        if (v[i] != 0 && (v[i] < 32 || v[i] == 127) && v[i] != '\n' && v[i] != '\r' && v[i] != '\t') ++bad;
    return bad == 0;
}

// Formats one value of a known VR from its (possibly truncated) bytes.
std::string formatValue(const DumpCtx& c, const char* vr, const std::vector<uint8_t>& v, uint64_t fullLen, uint32_t tag)
{
    const size_t n = v.size();
    auto num16 = [&](size_t i) { return c.bigEndian ? uint16_t(v[i] << 8 | v[i + 1]) : uint16_t(v[i + 1] << 8 | v[i]); };
    auto num32 = [&](size_t i) {
        return c.bigEndian ? (uint32_t(v[i]) << 24 | uint32_t(v[i + 1]) << 16 | uint32_t(v[i + 2]) << 8 | v[i + 3])
                           : (uint32_t(v[i + 3]) << 24 | uint32_t(v[i + 2]) << 16 | uint32_t(v[i + 1]) << 8 | v[i]);
    };
    auto join = [&](size_t count, size_t width, auto&& one) {
        std::string out;
        const size_t shown = std::min<size_t>(count, 16);
        for (size_t k = 0; k < shown; ++k) out += (k ? ", " : "") + one(k * width);
        if (count > shown) out += ", ... (" + std::to_string(count) + " values)";
        return out;
    };

    if (!std::memcmp(vr, "US", 2)) return join(n / 2, 2, [&](size_t i) { return std::to_string(num16(i)); });
    if (!std::memcmp(vr, "SS", 2)) return join(n / 2, 2, [&](size_t i) { return std::to_string(int16_t(num16(i))); });
    if (!std::memcmp(vr, "UL", 2)) return join(n / 4, 4, [&](size_t i) { return std::to_string(num32(i)); });
    if (!std::memcmp(vr, "SL", 2)) return join(n / 4, 4, [&](size_t i) { return std::to_string(int32_t(num32(i))); });
    if (!std::memcmp(vr, "FL", 2))
        return join(n / 4, 4, [&](size_t i) {
            uint32_t b = num32(i);
            float f;
            std::memcpy(&f, &b, 4);
            return wiv_plugin::MetadataBuilder::number(f);
        });
    if (!std::memcmp(vr, "FD", 2))
        return join(n / 8, 8, [&](size_t i) {
            uint64_t b = 0;
            for (int k = 0; k < 8; ++k) b = c.bigEndian ? (b << 8 | v[i + k]) : (b | uint64_t(v[i + k]) << (8 * k));
            double d;
            std::memcpy(&d, &b, 8);
            return wiv_plugin::MetadataBuilder::number(d);
        });
    if (!std::memcmp(vr, "AT", 2))
        return join(n / 4, 4, [&](size_t i) { return tagText(uint32_t(num16(i)) << 16 | num16(i + 2)); });

    if (isTextVr(vr) || (!std::memcmp(vr, "UN", 2) && mostlyPrintable(v, n)))
    {
        std::string t(reinterpret_cast<const char*>(v.data()), n);
        if (isLocalizedTextVr(vr)) t = toUtf8(t, c.charset);
        // Multi-valued strings are separated by backslashes (not in free text).
        if (std::memcmp(vr, "LT", 2) && std::memcmp(vr, "ST", 2) && std::memcmp(vr, "UT", 2))
        {
            std::string r;
            for (char ch : t) r += ch == '\\' ? std::string(", ") : std::string(1, ch);
            t = r;
        }
        t = trimText(t);
        if (!std::memcmp(vr, "PN", 2))
        {
            std::string r;
            for (char ch : t) r += ch == '^' ? std::string(" ") : ch == '=' ? std::string(" / ") : std::string(1, ch);
            t = r;
        }
        if (!std::memcmp(vr, "DA", 2) && t.size() == 8 && t.find_first_not_of("0123456789") == std::string::npos)
            t = t.substr(0, 4) + "-" + t.substr(4, 2) + "-" + t.substr(6, 2);
        if (!std::memcmp(vr, "TM", 2) && t.size() >= 6 && t.find_first_not_of("0123456789.") == std::string::npos)
            t = t.substr(0, 2) + ":" + t.substr(2, 2) + ":" + t.substr(4);
        if (!std::memcmp(vr, "AS", 2) && t.size() == 4)
        {
            const char unit = t[3];
            const std::string count = std::to_string(std::atoi(t.c_str()));
            t = count + (unit == 'Y' ? " years" : unit == 'M' ? " months" : unit == 'W' ? " weeks" : unit == 'D' ? " days" : "");
        }
        if (!std::memcmp(vr, "UI", 2))
            if (const char* name = dicom_tags::uidName(t.c_str())) t += std::string("  (") + name + ")";
        if (fullLen > n) t += " ...";
        return t;
    }
    (void)tag;
    return "<" + std::to_string(fullLen) + " bytes>";
}

void addEntry(DumpCtx& c, const std::string& section, const std::string& key, const std::string& value)
{
    if (c.entries.size() >= DumpCtx::kMaxEntries)
    {
        c.truncated = true;
        return;
    }
    c.entries.push_back({section, key, value});
}

std::string sectionForGroup(unsigned group)
{
    if (group & 1) return "DICOM private data";
    switch (group)
    {
        case 0x0002: return "DICOM file meta";
        case 0x0008: return "DICOM identification";
        case 0x0010: return "DICOM patient";
        case 0x0012: return "DICOM clinical trial";
        case 0x0018: return "DICOM acquisition";
        case 0x0020: return "DICOM image relationship";
        case 0x0028: return "DICOM image pixel";
        case 0x7FE0: return "DICOM image pixel";
        default: return "DICOM other";
    }
}

// Walks elements until `end` (or the enclosing item/sequence delimiter, for undefined lengths).
// `emit` false parses without recording (items beyond the display limit).
void dumpDataset(DumpCtx& c, int64_t end, int depth, bool emit, const std::string& parentSection)
{
    while (!c.stop)
    {
        const int64_t start = tellPos(c.f);
        if (start < 0 || (end >= 0 && start >= end)) return;
        uint16_t group = 0, element = 0;
        if (!readTag16(c, group, c.bigEndian) || !readTag16(c, element, c.bigEndian)) return; // EOF
        const uint32_t tag = uint32_t(group) << 16 | element;

        // Item / delimiters carry a 4-byte length and no VR in every syntax.
        if (group == 0xFFFE)
        {
            uint32_t len;
            if (!readTag32(c, len, c.bigEndian)) return;
            if (element == 0xE00D || element == 0xE0DD) return; // item / sequence delimiter
            continue; // a stray item header: keep walking its content
        }

        const dicom_tags::TagInfo* info = findTag(tag);
        char vr[3] = {0, 0, 0};
        uint32_t len = 0;
        if (c.implicitVr)
        {
            if (!readTag32(c, len, c.bigEndian)) return;
            std::memcpy(vr, info ? info->vr : "UN", 2);
        }
        else
        {
            if (!readRaw(c, vr, 2)) return;
            if (!isVrCode(vr)) return; // not an explicit-VR element: corrupt, or implicit data in an explicit file
            if (isLongVr(vr))
            {
                uint16_t reserved;
                if (!readTag16(c, reserved, c.bigEndian) || !readTag32(c, len, c.bigEndian)) return;
            }
            else
            {
                uint16_t len16;
                if (!readTag16(c, len16, c.bigEndian)) return;
                len = len16;
            }
        }
        // A private-group element whose VR is unknown in an implicit file may still be a sequence: peek for an item tag.
        bool isSequence = !std::memcmp(vr, "SQ", 2) || (len == 0xFFFFFFFFu && std::memcmp(vr, "OB", 2) && tag != 0x7FE00010);
        if (c.implicitVr && !isSequence && len >= 8 && len != 0xFFFFFFFFu && !std::memcmp(vr, "UN", 2))
        {
            const int64_t save = tellPos(c.f);
            uint16_t g = 0, e = 0;
            if (readTag16(c, g, c.bigEndian) && readTag16(c, e, c.bigEndian) && g == 0xFFFE && e == 0xE000) isSequence = true;
            seekPos(c.f, save);
        }

        const std::string section = depth == 0 ? sectionForGroup(group) : parentSection;
        const std::string indent = depth == 0 ? "" : std::string(size_t(depth), '>') + " ";
        const bool isPrivate = (group & 1) != 0;
        std::string name;
        if (info) name = info->name;
        else if (isPrivate && element >= 0x0010 && element <= 0x00FF) name = "Private Creator";
        else name = isPrivate ? "Private" : "";
        const std::string key = indent + tagText(tag) + (name.empty() ? "" : " " + name);

        // Pixel data: never read.
        if (tag == 0x7FE00010 || tag == 0x7FE00008 || tag == 0x7FE00009)
        {
            if (len == 0xFFFFFFFFu)
            {
                if (emit) addEntry(c, section, key, "encapsulated (compressed), fragments follow");
                if (depth == 0) c.top[tag] = "encapsulated";
                c.stop = true; // the fragments run to the sequence delimiter; nothing useful follows
                return;
            }
            if (emit) addEntry(c, section, key, std::to_string(len) + " bytes");
            if (depth == 0) c.top[tag] = std::to_string(len) + " bytes";
            if (!seekPos(c.f, tellPos(c.f) + len)) return;
            continue;
        }

        if (isSequence)
        {
            const bool undefined = len == 0xFFFFFFFFu;
            const int64_t seqEnd = undefined ? -1 : tellPos(c.f) + int64_t(len);
            if (seqEnd > c.fileSize && !undefined) return; // corrupt length
            int itemNo = 0;
            if (emit) addEntry(c, section, key, "");
            if (depth >= DumpCtx::kMaxDepth)
            {
                if (!undefined) seekPos(c.f, seqEnd);
                else return;
                continue;
            }
            while (!c.stop)
            {
                const int64_t pos = tellPos(c.f);
                if (!undefined && pos >= seqEnd) break;
                uint16_t g = 0, e = 0;
                uint32_t itemLen = 0;
                if (!readTag16(c, g, c.bigEndian) || !readTag16(c, e, c.bigEndian) || !readTag32(c, itemLen, c.bigEndian)) return;
                if (g == 0xFFFE && e == 0xE0DD) break; // sequence delimiter
                if (!(g == 0xFFFE && e == 0xE000)) return; // not an item: corrupt
                ++itemNo;
                const bool showItem = emit && itemNo <= DumpCtx::kMaxItems;
                if (showItem) addEntry(c, section, std::string(size_t(depth + 1), '>') + " Item " + std::to_string(itemNo), "");
                else if (emit && itemNo == DumpCtx::kMaxItems + 1)
                    addEntry(c, section, std::string(size_t(depth + 1), '>') + " (further items not shown)", "");
                if (itemLen != 0xFFFFFFFFu && !showItem)
                {
                    if (!seekPos(c.f, tellPos(c.f) + itemLen)) return;
                    continue;
                }
                const int64_t itemEnd = itemLen == 0xFFFFFFFFu ? -1 : tellPos(c.f) + int64_t(itemLen);
                dumpDataset(c, itemEnd, depth + 1, showItem, section);
            }
            continue;
        }

        if (len == 0xFFFFFFFFu) return; // undefined length on a non-sequence we cannot walk

        // Ordinary element: read up to a few KB of the value, skip the rest.
        const size_t keep = std::min<uint64_t>(len, isTextVr(vr) ? 2048 : 128);
        std::vector<uint8_t> value(keep);
        if (keep && !readRaw(c, value.data(), keep)) return;
        if (len > keep && !seekPos(c.f, tellPos(c.f) + (len - keep))) return;

        std::string text;
        if (!std::memcmp(vr, "OB", 2) || !std::memcmp(vr, "OW", 2) || !std::memcmp(vr, "OF", 2) || !std::memcmp(vr, "OD", 2) ||
            !std::memcmp(vr, "OL", 2) || !std::memcmp(vr, "OV", 2) || (!std::memcmp(vr, "UN", 2) && !mostlyPrintable(value, keep)))
        {
            // Small binary values (File Meta Information Version, ...) as hex bytes.
            if (len <= 16)
            {
                char b[4];
                for (size_t i = 0; i < keep; ++i)
                {
                    std::snprintf(b, sizeof(b), "%02X", value[i]);
                    text += (i ? " " : "") + std::string(b);
                }
            }
            else
                text = "<" + std::to_string(len) + " bytes>";
        }
        else
            text = formatValue(c, vr, value, len, tag);
        if (emit) addEntry(c, section, key, text);
        if (depth == 0)
        {
            // For the summary: the formatted value, but a UID stays bare (the summary adds its name).
            c.top[tag] = !std::memcmp(vr, "UI", 2) ? trimText(std::string(reinterpret_cast<const char*>(value.data()), keep)) : text;
            if (tag == 0x00080005) c.charset = charsetFor(trimText(std::string(reinterpret_cast<const char*>(value.data()), keep)));
        }
    }
}

} // namespace

extern "C" char* wiv_plugin_metadata(const char* filename, int /*page*/)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
        return nullptr;
    char preamble[132];
    if (std::fread(preamble, 1, sizeof(preamble), fh) != sizeof(preamble) || std::memcmp(preamble + 128, "DICM", 4) != 0)
    {
        std::fclose(fh);
        return nullptr;
    }
    DumpCtx c;
    c.f = fh;
    seekPos(fh, 0);
    std::fseek(fh, 0, SEEK_END);
    c.fileSize = tellPos(fh);
    seekPos(fh, 132);

    // 1) File meta information: group 0002, always explicit VR little endian.
    std::string transferSyntax;
    for (;;)
    {
        const int64_t pos = tellPos(fh);
        uint16_t group = 0, element = 0;
        if (!readTag16(c, group, false) || !readTag16(c, element, false)) break;
        if (group != 0x0002)
        {
            seekPos(fh, pos);
            break;
        }
        // Reuse the generic walker for a single element: rewind and let it run over exactly this element.
        seekPos(fh, pos);
        char vr[2];
        uint32_t len = 0;
        uint16_t g2, e2;
        readTag16(c, g2, false);
        readTag16(c, e2, false);
        if (!readRaw(c, vr, 2) || !isVrCode(vr)) break;
        if (isLongVr(vr))
        {
            uint16_t reserved;
            readTag16(c, reserved, false);
            readTag32(c, len, false);
        }
        else
        {
            uint16_t l16;
            readTag16(c, l16, false);
            len = l16;
        }
        if (len > 65536) break;
        std::vector<uint8_t> value(len);
        if (len && !readRaw(c, value.data(), len)) break;
        const uint32_t tag = uint32_t(group) << 16 | element;
        const dicom_tags::TagInfo* info = findTag(tag);
        std::string text;
        if (tag == 0x00020010) transferSyntax = trimText(std::string(reinterpret_cast<const char*>(value.data()), len));
        if (len <= 16 && (!std::memcmp(vr, "OB", 2) || !std::memcmp(vr, "UN", 2)))
        {
            char b[4];
            for (size_t i = 0; i < len; ++i)
            {
                std::snprintf(b, sizeof(b), "%02X", value[i]);
                text += (i ? " " : "") + std::string(b);
            }
        }
        else
            text = formatValue(c, vr, value, len, tag);
        addEntry(c, "DICOM file meta", tagText(tag) + (info ? std::string(" ") + info->name : std::string()), text);
        c.top[tag] = !std::memcmp(vr, "UI", 2) ? trimText(std::string(reinterpret_cast<const char*>(value.data()), len)) : text;
    }

    // 2) The dataset, in the declared transfer syntax.
    c.implicitVr = transferSyntax == "1.2.840.10008.1.2";
    c.bigEndian = transferSyntax == "1.2.840.10008.1.2.2";
    const bool deflated = transferSyntax == "1.2.840.10008.1.2.1.99";
    if (!deflated)
        dumpDataset(c, -1, 0, true, "");
    std::fclose(fh);

    // 3) A summary of what matters most, ahead of the full listing.
    wiv_plugin::MetadataBuilder md;
    auto top = [&](uint32_t tag) -> std::string {
        auto it = c.top.find(tag);
        return it == c.top.end() ? std::string() : it->second;
    };
    auto addIf = [&](const char* key, const std::string& v) {
        if (!v.empty()) md.add("DICOM summary", key, v);
    };
    auto uidWithName = [&](const std::string& uid) {
        const char* n = dicom_tags::uidName(uid.c_str());
        return n ? std::string(n) : uid;
    };
    addIf("SOP class", uidWithName(top(0x00080016).empty() ? top(0x00020002) : top(0x00080016)));
    addIf("Modality", top(0x00080060));
    addIf("Transfer syntax", transferSyntax.empty() ? "" : uidWithName(transferSyntax));
    if (deflated) md.add("DICOM summary", "Note", "The dataset is deflate-compressed; its elements are not shown.");
    const std::string cols = top(0x00280011), rows = top(0x00280010);
    if (!cols.empty() && !rows.empty())
    {
        std::string size;
        // US values were formatted as decimal text already.
        size = cols + " \xC3\x97 " + rows;
        const std::string frames = top(0x00280008);
        if (!frames.empty() && frames != "1") size += " \xC3\x97 " + frames + " frames";
        md.add("DICOM summary", "Size (columns \xC3\x97 rows)", size);
    }
    {
        const std::string alloc = top(0x00280100), stored = top(0x00280101), rep = top(0x00280103), spp = top(0x00280002);
        std::string bits;
        if (!alloc.empty()) bits = alloc + " bits allocated";
        if (!stored.empty()) bits += (bits.empty() ? "" : ", ") + stored + " stored";
        if (!rep.empty()) bits += std::string(bits.empty() ? "" : ", ") + (rep == "1" ? "signed" : "unsigned");
        if (!spp.empty() && spp != "1") bits += ", " + spp + " samples per pixel";
        addIf("Pixel format", bits);
    }
    addIf("Photometric interpretation", top(0x00280004));
    addIf("Pixel spacing (mm)", top(0x00280030));
    addIf("Slice thickness (mm)", top(0x00180050));
    addIf("Spacing between slices (mm)", top(0x00180088));
    addIf("Window center / width", [&] {
        const std::string wc = top(0x00281050), ww = top(0x00281051);
        return wc.empty() && ww.empty() ? std::string() : wc + " / " + ww;
    }());
    addIf("Rescale slope / intercept", [&] {
        const std::string sl = top(0x00281053), ic = top(0x00281052);
        return sl.empty() && ic.empty() ? std::string() : sl + " / " + ic;
    }());
    addIf("Study date", top(0x00080020));
    addIf("Series description", top(0x0008103E));
    addIf("Study description", top(0x00081030));
    addIf("Manufacturer", [&] {
        std::string m = top(0x00080070);
        const std::string model = top(0x00081090);
        return model.empty() ? m : (m.empty() ? model : m + " " + model);
    }());
    if (c.truncated) md.add("DICOM summary", "Note", "Very long header: the listing below is cut off.");

    for (const MetaEntry& e : c.entries) md.add(e.section, e.key, e.value);
    return md.release();
}
