// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// dicom_plugin.cpp - wiv image plugin for DICOM, built on GDCM (Grassroots DICOM).
//
// GDCM does the parsing, the transfer-syntax handling (implicit/explicit, big endian, deflate,
// JPEG, JPEG-LS, JPEG 2000, RLE), the character-set conversion and the data dictionary. Here:
//   - Grayscale images keep their raw samples (U8 / U16; signed, 32-bit and floating-point data is
//     widened to float) so contrast and colour tables work on the real values. Multi-frame datasets
//     become multi-slice images (WivPluginImage::depth).
//   - 8-bit colour images (RGB, or YBR converted to RGB) are shown as plain RGBA.
//   - File > Image Info lists every element (sequences nested, indented with "> "), with a summary
//     of the values people usually want ahead of the full listing.
//
#include "../wiv_plugin_common.h"

#include <gdcmDataSet.h>
#include <gdcmDict.h>
#include <gdcmDicts.h>
#include <gdcmGlobal.h>
#include <gdcmImage.h>
#include <gdcmImageChangePhotometricInterpretation.h>
#include <gdcmImageReader.h>
#include <gdcmReader.h>
#include <gdcmSequenceOfItems.h>
#include <gdcmStringFilter.h>
#include <gdcmUIDs.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// Opens `filename` (UTF-8, also on Windows) for GDCM, which reads from a stream.
bool openStream(const char* filename, std::ifstream& in)
{
    in.open(std::filesystem::path(reinterpret_cast<const char8_t*>(filename)), std::ios::binary);
    return in.is_open();
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
    auto fail = [&](const char* fmt) -> WivPluginImage* {
        if (error_msg)
            *error_msg = wiv_plugin::makeError(fmt, filename);
        return nullptr;
    };

    std::ifstream in;
    if (!openStream(filename, in))
        return fail("dicom: failed to open %s");

    gdcm::ImageReader reader;
    reader.SetStream(in);
    if (!reader.Read())
        return fail("dicom: not a readable DICOM image: %s");

    gdcm::Image image = reader.GetImage();
    if (image.GetNumberOfDimensions() < 2)
        return fail("dicom: not a 2D image: %s");

    // Colour: convert YBR (and friends) to RGB; the result is shown as plain RGBA.
    const gdcm::PixelFormat pf0 = image.GetPixelFormat();
    const bool color = pf0.GetSamplesPerPixel() == 3;
    if (color)
    {
        if (pf0.GetScalarType() != gdcm::PixelFormat::UINT8)
            return fail("dicom: only 8-bit colour images are supported: %s");
        if (image.GetPhotometricInterpretation() != gdcm::PhotometricInterpretation::RGB)
        {
            gdcm::ImageChangePhotometricInterpretation change;
            change.SetInput(image);
            change.SetPhotometricInterpretation(gdcm::PhotometricInterpretation::RGB);
            if (!change.Change())
                return fail("dicom: cannot convert the colour space to RGB: %s");
            image = change.GetOutput();
        }
    }
    else if (pf0.GetSamplesPerPixel() != 1 ||
             image.GetPhotometricInterpretation() == gdcm::PhotometricInterpretation::PALETTE_COLOR)
        return fail("dicom: unsupported dataset (need grayscale or RGB pixels): %s");

    const unsigned* dims = image.GetDimensions();
    const int width = int(dims[0]), height = int(dims[1]);
    const int frames = image.GetNumberOfDimensions() > 2 ? std::max(1, int(dims[2])) : 1;
    if (width <= 0 || height <= 0)
        return fail("dicom: empty image: %s");

    std::vector<char> buf(image.GetBufferLength());
    if (!image.GetBuffer(buf.data()))
        return fail("dicom: failed to decode the pixel data of %s");

    const gdcm::PixelFormat pf = image.GetPixelFormat();
    const size_t sliceElemCount = size_t(width) * height;
    const size_t total = sliceElemCount * size_t(frames);

    if (color)
    {
        if (buf.size() < total * 3)
            return fail("dicom: pixel data shorter than Rows*Columns*Frames in %s");
        WivPluginImage* img = wiv_plugin::allocImage(width, height);
        const bool planar = image.GetPlanarConfiguration() == 1; // RRR...GGG...BBB...
        const unsigned char* src = reinterpret_cast<const unsigned char*>(buf.data());
        for (size_t i = 0; i < sliceElemCount; ++i)
        {
            for (int c = 0; c < 3; ++c)
                img->rgba[i * 4 + c] = planar ? src[c * sliceElemCount + i] : src[i * 3 + c];
            img->rgba[i * 4 + 3] = 255;
        }
        return img; // first frame only
    }

    WivSampleType sampleType;
    size_t bytesPerSample;
    switch (pf.GetScalarType())
    {
        case gdcm::PixelFormat::UINT8: sampleType = WIV_SAMPLE_U8; bytesPerSample = 1; break;
        case gdcm::PixelFormat::UINT16: sampleType = WIV_SAMPLE_U16; bytesPerSample = 2; break;
        default: sampleType = WIV_SAMPLE_FLOAT; bytesPerSample = 4; break;
    }
    const size_t srcBytes = pf.GetBitsAllocated() / 8;
    if (buf.size() < total * srcBytes)
        return fail("dicom: pixel data shorter than Rows*Columns*Frames in %s");

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    wiv_plugin::allocSamples(img, sampleType, frames);

    if (sampleType == WIV_SAMPLE_FLOAT)
    {
        float* dst = static_cast<float*>(img->samples);
        auto widen = [&](auto* src) { for (size_t i = 0; i < total; ++i) dst[i] = float(src[i]); };
        switch (pf.GetScalarType())
        {
            case gdcm::PixelFormat::INT8: widen(reinterpret_cast<const int8_t*>(buf.data())); break;
            case gdcm::PixelFormat::INT16: widen(reinterpret_cast<const int16_t*>(buf.data())); break;
            case gdcm::PixelFormat::UINT32: widen(reinterpret_cast<const uint32_t*>(buf.data())); break;
            case gdcm::PixelFormat::INT32: widen(reinterpret_cast<const int32_t*>(buf.data())); break;
            case gdcm::PixelFormat::FLOAT32: widen(reinterpret_cast<const float*>(buf.data())); break;
            case gdcm::PixelFormat::FLOAT64: widen(reinterpret_cast<const double*>(buf.data())); break;
            default: std::memset(dst, 0, total * sizeof(float)); break;
        }
    }
    else
        std::memcpy(img->samples, buf.data(), total * bytesPerSample);

    // Slice-0 grayscale preview - never actually displayed for a raw-sample
    // image (see DisplayImage.h), just a reasonable placeholder.
    float lo = 0, hi = 1;
    if (sampleType == WIV_SAMPLE_FLOAT)
    {
        const float* s = static_cast<const float*>(img->samples);
        lo = hi = s[0];
        for (size_t i = 0; i < sliceElemCount; ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
        if (hi <= lo) hi = lo + 1;
    }
    for (size_t i = 0; i < sliceElemCount; ++i)
    {
        uint8_t g;
        if (sampleType == WIV_SAMPLE_U8) g = static_cast<const uint8_t*>(img->samples)[i];
        else if (sampleType == WIV_SAMPLE_U16) g = static_cast<uint8_t>(static_cast<const uint16_t*>(img->samples)[i] >> 8);
        else g = static_cast<uint8_t>(255.f * (static_cast<const float*>(img->samples)[i] - lo) / (hi - lo));
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

namespace
{

struct MetaEntry
{
    std::string section, key, value;
};

struct DumpCtx
{
    struct Filter : gdcm::StringFilter
    {
        using gdcm::StringFilter::ToStringPair; // public here: the (tag, dataset) form formats nested elements
    } filter;
    std::vector<MetaEntry> entries;
    std::map<uint32_t, std::string> top; // top-level element values by tag, for the summary
    bool truncated = false;
    static constexpr size_t kMaxEntries = 4000;
    static constexpr int kMaxDepth = 6;
    static constexpr int kMaxItems = 8; // items shown per sequence
};

std::string hex4(unsigned v)
{
    char b[8];
    std::snprintf(b, sizeof(b), "%04X", v);
    return b;
}
std::string tagText(uint32_t tag) { return "(" + hex4(tag >> 16) + "," + hex4(tag & 0xFFFF) + ")"; }

std::string trimText(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// Replaces invalid UTF-8 so the panel never shows mojibake.
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

bool isBinaryVr(const gdcm::VR& vr)
{
    return vr == gdcm::VR::OB || vr == gdcm::VR::OW || vr == gdcm::VR::OF || vr == gdcm::VR::OD ||
           vr == gdcm::VR::OL || vr == gdcm::VR::OV || vr == gdcm::VR::UN || vr == gdcm::VR::INVALID;
}

std::string hexBytes(const gdcm::ByteValue* bv)
{
    std::string text;
    char b[4];
    const char* p = bv->GetPointer();
    for (size_t i = 0; i < bv->GetLength(); ++i)
    {
        std::snprintf(b, sizeof(b), "%02X", static_cast<unsigned char>(p[i]));
        text += (i ? " " : "") + std::string(b);
    }
    return text;
}

std::string uidWithName(const std::string& uid)
{
    gdcm::UIDs u;
    if (u.SetFromUID(uid.c_str()))
        if (const char* n = u.GetName())
            if (*n)
            {
                std::string name = n; // GDCM appends a prose note to some transfer syntaxes after ": "
                const size_t colon = name.find(": ");
                return colon == std::string::npos ? name : name.substr(0, colon);
            }
    return uid;
}

// The displayed text of one element; `rawUid` receives a bare UID value (the summary names it itself).
std::string formatElement(DumpCtx& c, const gdcm::DataSet& ds, const gdcm::DataElement& de, std::string* rawUid)
{
    const gdcm::Tag tag = de.GetTag();
    const gdcm::DictEntry& entry = gdcm::Global::GetInstance().GetDicts().GetDictEntry(tag);
    gdcm::VR vr = de.GetVR();
    if (vr == gdcm::VR::INVALID || vr == gdcm::VR::UN) vr = entry.GetVR();
    const gdcm::ByteValue* bv = de.GetByteValue();
    const size_t len = bv ? size_t(uint32_t(bv->GetLength())) : size_t(0);

    if (!bv) return "";
    if (isBinaryVr(vr))
    {
        // Small binary values (File Meta Information Version, ...) as hex bytes.
        return len <= 16 ? hexBytes(bv) : "<" + std::to_string(len) + " bytes>";
    }

    std::string t = c.filter.ToStringPair(tag, ds).second;
    t = sanitizeUtf8(trimText(t));
    // Free text keeps its characters; multi-valued strings come back backslash-separated.
    if (vr != gdcm::VR::LT && vr != gdcm::VR::ST && vr != gdcm::VR::UT)
    {
        std::string r;
        for (char ch : t) r += ch == '\\' ? std::string(", ") : std::string(1, ch);
        t = r;
    }
    if (vr == gdcm::VR::UI && rawUid) *rawUid = t;
    if (vr == gdcm::VR::PN)
    {
        std::string r;
        for (char ch : t) r += ch == '^' ? std::string(" ") : ch == '=' ? std::string(" / ") : std::string(1, ch);
        t = r;
    }
    if (vr == gdcm::VR::DA && t.size() == 8 && t.find_first_not_of("0123456789") == std::string::npos)
        t = t.substr(0, 4) + "-" + t.substr(4, 2) + "-" + t.substr(6, 2);
    if (vr == gdcm::VR::TM && t.size() >= 6 && t.find_first_not_of("0123456789.") == std::string::npos)
        t = t.substr(0, 2) + ":" + t.substr(2, 2) + ":" + t.substr(4);
    if (vr == gdcm::VR::AS && t.size() == 4)
    {
        const char unit = t[3];
        const std::string count = std::to_string(std::atoi(t.c_str()));
        t = count + (unit == 'Y' ? " years" : unit == 'M' ? " months" : unit == 'W' ? " weeks" : unit == 'D' ? " days" : "");
    }
    if (vr == gdcm::VR::UI)
        t = uidWithName(t) == t ? t : t + "  (" + uidWithName(t) + ")";
    if (t.size() > 2048) t = t.substr(0, 2048) + " ...";
    return t;
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

void dumpDataset(DumpCtx& c, const gdcm::DataSet& ds, int depth, bool emit, const std::string& parentSection)
{
    for (const gdcm::DataElement& de : ds.GetDES())
    {
        const gdcm::Tag tag = de.GetTag();
        const unsigned group = tag.GetGroup(), element = tag.GetElement();
        if (group == 0xFFFE) continue;
        const uint32_t tag32 = tag.GetElementTag();

        const gdcm::DictEntry& entry = gdcm::Global::GetInstance().GetDicts().GetDictEntry(tag);
        std::string name = entry.GetName();
        const bool isPrivate = (group & 1) != 0;
        if (name.empty() || name == "?")
            name = isPrivate ? (element >= 0x0010 && element <= 0x00FF ? "Private Creator" : "Private") : "";
        const std::string indent = depth == 0 ? "" : std::string(size_t(depth), '>') + " ";
        const std::string key = indent + tagText(tag32) + (name.empty() ? "" : " " + name);
        const std::string section = depth == 0 ? sectionForGroup(group) : parentSection;

        // Pixel data: never listed in detail (the dump stops before it, but nested copies may exist).
        if (tag32 == 0x7FE00010 || tag32 == 0x7FE00008 || tag32 == 0x7FE00009)
        {
            if (emit) addEntry(c, section, key, std::to_string(de.GetVL()) + " bytes");
            continue;
        }

        // GDCM parses sequences (defined or undefined length, private ones too) into SequenceOfItems.
        gdcm::SmartPointer<gdcm::SequenceOfItems> sq;
        if (!de.IsEmpty() && (de.GetVR() == gdcm::VR::SQ || de.IsUndefinedLength() || !de.GetByteValue()))
            sq = de.GetValueAsSQ();
        if (sq)
        {
            if (emit) addEntry(c, section, key, "");
            if (depth >= DumpCtx::kMaxDepth) continue;
            int itemNo = 0;
            for (size_t i = 1; i <= sq->GetNumberOfItems(); ++i)
            {
                ++itemNo;
                const bool showItem = emit && itemNo <= DumpCtx::kMaxItems;
                if (showItem) addEntry(c, section, std::string(size_t(depth + 1), '>') + " Item " + std::to_string(itemNo), "");
                else if (emit && itemNo == DumpCtx::kMaxItems + 1)
                    addEntry(c, section, std::string(size_t(depth + 1), '>') + " (further items not shown)", "");
                if (!showItem) continue;
                dumpDataset(c, sq->GetItem(i).GetNestedDataSet(), depth + 1, showItem, section);
            }
            continue;
        }

        std::string rawUid;
        const std::string text = formatElement(c, ds, de, &rawUid);
        if (emit) addEntry(c, section, key, text);
        if (depth == 0) c.top[tag32] = rawUid.empty() ? text : rawUid;
    }
}

} // namespace

extern "C" char* wiv_plugin_metadata(const char* filename, int /*page*/)
{
    std::ifstream in;
    if (!openStream(filename, in))
        return nullptr;

    gdcm::Reader reader;
    reader.SetStream(in);
    // Stop before the pixel data: headers only, however big the image.
    if (!reader.ReadUpToTag(gdcm::Tag(0x7FE0, 0x0010)))
        return nullptr;
    const gdcm::File& file = reader.GetFile();

    DumpCtx c;
    c.filter.SetFile(file);
    dumpDataset(c, file.GetHeader(), 0, true, "");
    dumpDataset(c, file.GetDataSet(), 0, true, "");

    const std::string transferSyntax = trimText(c.top.count(0x00020010) ? c.top[0x00020010] : std::string());

    // A summary of what matters most, ahead of the full listing.
    wiv_plugin::MetadataBuilder md;
    auto top = [&](uint32_t tag) -> std::string {
        auto it = c.top.find(tag);
        return it == c.top.end() ? std::string() : it->second;
    };
    auto addIf = [&](const char* key, const std::string& v) {
        if (!v.empty()) md.add("DICOM summary", key, v);
    };
    addIf("SOP class", uidWithName(top(0x00080016).empty() ? top(0x00020002) : top(0x00080016)));
    addIf("Modality", top(0x00080060));
    addIf("Transfer syntax", transferSyntax.empty() ? "" : uidWithName(transferSyntax));
    const std::string cols = top(0x00280011), rows = top(0x00280010);
    if (!cols.empty() && !rows.empty())
    {
        std::string size = cols + " \xC3\x97 " + rows;
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
