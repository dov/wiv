// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// wiv_plugin_metadata(): the format-specific entries of the Image Info panel, checked on small synthetic files.
#include "ImagePluginHost.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using Entries = std::vector<giv::MetadataEntry>;

class MetadataTest : public testing::Test
{
protected:
    void SetUp() override { setenv("WIV_PLUGIN_DIR", WIV_TEST_PLUGIN_DIR, 1); }

    static std::string tmp(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

    static void write(const std::string& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }

    // The value of `key` in `section`, or "<missing>".
    static std::string get(const Entries& e, const std::string& section, const std::string& key)
    {
        for (const auto& x : e)
            if (x.section == section && x.key == key) return x.value;
        return "<missing>";
    }
    static bool hasSection(const Entries& e, const std::string& section)
    {
        for (const auto& x : e)
            if (x.section == section) return true;
        return false;
    }
};

template <class T>
void put(std::vector<uint8_t>& b, size_t off, T v)
{
    std::memcpy(b.data() + off, &v, sizeof(T));
}
void putText(std::vector<uint8_t>& b, size_t off, const char* s) { std::memcpy(b.data() + off, s, std::strlen(s)); }

// A 4x3x2 uint8 NIfTI-1 (little-endian, .nii) with a 2.5/2.5/3 mm sform in LAS orientation and a comment extension.
std::vector<uint8_t> makeNifti()
{
    std::vector<uint8_t> b(352 + 16 + 24, 0); // header + extender + one 16-byte extension... + data below
    put<int32_t>(b, 0, 348);
    put<int16_t>(b, 40, 3);
    put<int16_t>(b, 42, 4);
    put<int16_t>(b, 44, 3);
    put<int16_t>(b, 46, 2);
    put<int16_t>(b, 70, 2);  // uint8
    put<int16_t>(b, 72, 8);
    put<float>(b, 76, 1.0f); // qfac
    put<float>(b, 80, 2.5f);
    put<float>(b, 84, 2.5f);
    put<float>(b, 88, 3.0f);
    put<float>(b, 108, 368.0f); // vox_offset: header(348) + extender(4) + extension(16)
    put<float>(b, 112, 2.0f);   // slope
    put<float>(b, 116, -10.0f); // inter
    b[123] = 2 | 8;             // mm, seconds
    put<float>(b, 124, 200.0f); // cal_max
    put<float>(b, 128, 5.0f);   // cal_min
    putText(b, 148, "T1 weighted test volume");
    put<int16_t>(b, 252, 0);
    put<int16_t>(b, 254, 1); // sform: scanner
    const float sx[4] = {-2.5f, 0, 0, 90.0f}, sy[4] = {0, 2.5f, 0, -126.0f}, sz[4] = {0, 0, 3.0f, -72.0f};
    std::memcpy(b.data() + 280, sx, 16);
    std::memcpy(b.data() + 296, sy, 16);
    std::memcpy(b.data() + 312, sz, 16);
    putText(b, 344, "n+1");
    b[348] = 1; // extender: extensions present
    // One 16-byte extension: esize=16, ecode=6 (comment), 8 bytes of text.
    put<int32_t>(b, 352, 16);
    put<int32_t>(b, 356, 6);
    putText(b, 360, "hello");
    b.resize(368 + 24, 0); // 4*3*2 voxels
    return b;
}
} // namespace

TEST_F(MetadataTest, NiftiHeader)
{
    const std::string path = tmp("wiv_meta_test.nii");
    write(path, makeNifti());
    const Entries e = giv::ImagePluginHost::readMetadata(path);
    ASSERT_FALSE(e.empty());
    EXPECT_EQ(get(e, "NIfTI header", "Version"), "NIfTI-1");
    EXPECT_EQ(get(e, "NIfTI header", "Dimensions"), "3D: 4 \xC3\x97 3 \xC3\x97 2");
    EXPECT_EQ(get(e, "NIfTI header", "Data type"), "uint8 (code 2)");
    EXPECT_EQ(get(e, "NIfTI header", "Voxel size"), "2.5 \xC3\x97 2.5 \xC3\x97 3 (mm, s)");
    EXPECT_EQ(get(e, "NIfTI header", "Scaling (slope, intercept)"), "2, -10");
    EXPECT_EQ(get(e, "NIfTI header", "Display range (cal_min, cal_max)"), "5, 200");
    EXPECT_EQ(get(e, "NIfTI header", "Description"), "T1 weighted test volume");
    EXPECT_EQ(get(e, "NIfTI orientation", "sform code"), "1 (scanner anatomical coordinates)");
    EXPECT_EQ(get(e, "NIfTI orientation", "Axis orientation"), "LAS (from sform)");
    EXPECT_EQ(get(e, "NIfTI orientation", "sform row x"), "-2.5  0  0  90");
    ASSERT_TRUE(hasSection(e, "NIfTI extensions"));
    EXPECT_EQ(get(e, "NIfTI extensions", "Extension 1 - comment"), "code 6 (16 bytes): hello");
    std::filesystem::remove(path);
}

TEST_F(MetadataTest, NiftiQformOrientation)
{
    // Identity quaternion, qfac=-1: the third axis is flipped, giving RAI.
    std::vector<uint8_t> b = makeNifti();
    put<int16_t>(b, 254, 0); // no sform
    put<int16_t>(b, 252, 1); // qform
    put<float>(b, 76, -1.0f);
    const std::string path = tmp("wiv_meta_qform.nii");
    write(path, b);
    const Entries e = giv::ImagePluginHost::readMetadata(path);
    EXPECT_EQ(get(e, "NIfTI orientation", "Axis orientation"), "RAI (from qform)");
    std::filesystem::remove(path);
}

TEST_F(MetadataTest, NoMetadataForUnknownFormats)
{
    EXPECT_TRUE(giv::ImagePluginHost::readMetadata(tmp("wiv_does_not_exist.nii")).empty());
    EXPECT_TRUE(giv::ImagePluginHost::readMetadata(tmp("wiv_does_not_exist.xyz")).empty());
}

TEST_F(MetadataTest, FitsHeaderCards)
{
    const std::string path = std::string(WIV_EXAMPLES_DIR) + "/m51.fits";
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "example file missing";
    const Entries e = giv::ImagePluginHost::readMetadata(path);
    if (e.empty()) GTEST_SKIP() << "FITS plugin not built";
    EXPECT_EQ(get(e, "FITS header", "SIMPLE"), "T   / Created by ImageJ FITS_Writer");
    EXPECT_EQ(get(e, "FITS header", "BITPIX"), "-32   / number of bits per data pixel");
    EXPECT_EQ(get(e, "FITS header", "NAXIS1"), "973   / length of data axis 1");
    EXPECT_EQ(get(e, "FITS header", "FILTER"), "Luminance");
    EXPECT_NE(get(e, "FITS header", "COMMENT").find("FITS (Flexible Image Transport System)"), std::string::npos);
}


// ---- DICOM -----------------------------------------------------------------------------------------------

namespace
{
struct Dcm
{
    std::vector<uint8_t> b;
    bool implicitVr = false;

    Dcm()
    {
        b.assign(128, 0);
        for (char ch : std::string("DICM")) b.push_back(uint8_t(ch));
    }
    void u16(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    void tag(uint16_t g, uint16_t e) { u16(g); u16(e); }
    static std::string even(std::string s, char pad = ' ')
    {
        if (s.size() % 2) s += pad;
        return s;
    }
    // Group 0002 is always explicit VR little endian.
    void meta(uint16_t e, const char* vr, const std::string& v)
    {
        tag(2, e);
        b.push_back(uint8_t(vr[0])); b.push_back(uint8_t(vr[1]));
        u16(uint16_t(v.size()));
        b.insert(b.end(), v.begin(), v.end());
    }
    void elem(uint16_t g, uint16_t e, const char* vr, const std::vector<uint8_t>& v, bool longForm = false, uint32_t lenOverride = 0xFFFFFFFF)
    {
        tag(g, e);
        const uint32_t len = lenOverride != 0xFFFFFFFF ? lenOverride : uint32_t(v.size());
        if (implicitVr) u32(len);
        else
        {
            b.push_back(uint8_t(vr[0])); b.push_back(uint8_t(vr[1]));
            if (longForm) { u16(0); u32(len); }
            else u16(uint16_t(len));
        }
        b.insert(b.end(), v.begin(), v.end());
    }
    void text(uint16_t g, uint16_t e, const char* vr, const std::string& s)
    {
        const std::string v = even(s, std::string(vr) == "UI" ? '\0' : ' ');
        elem(g, e, vr, std::vector<uint8_t>(v.begin(), v.end()));
    }
    void us(uint16_t g, uint16_t e, uint16_t v) { elem(g, e, "US", {uint8_t(v), uint8_t(v >> 8)}); }
    void begin(const std::string& transferSyntax)
    {
        meta(0x10, "UI", even(transferSyntax, '\0'));
        implicitVr = transferSyntax == "1.2.840.10008.1.2";
    }
    void save(const std::string& path) const
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    }
};
} // namespace

TEST_F(MetadataTest, DicomExplicitWithNestedSequenceAndPixelData)
{
    Dcm d;
    d.begin("1.2.840.10008.1.2.1");
    d.text(0x08, 0x60, "CS", "MR");
    d.text(0x10, 0x10, "PN", "Doe^John");
    d.text(0x18, 0x50, "DS", "2.5");
    // Anatomic Region Sequence: undefined length, one undefined-length item holding a Code Meaning.
    d.tag(0x08, 0x2218); d.b.insert(d.b.end(), {'S', 'Q', 0, 0}); d.u32(0xFFFFFFFF);
    d.tag(0xFFFE, 0xE000); d.u32(0xFFFFFFFF);
    d.text(0x08, 0x104, "LO", "Thoracic spine");
    d.tag(0xFFFE, 0xE00D); d.u32(0);
    d.tag(0xFFFE, 0xE0DD); d.u32(0);
    d.us(0x28, 0x10, 4);
    d.us(0x28, 0x11, 3);
    // 100 kB of pixel data that must be skipped, followed by one more element.
    d.elem(0x7FE0, 0x10, "OW", std::vector<uint8_t>(100000, 0x55), true);
    d.text(0x20, 0x13, "IS", "7");
    const std::string path = tmp("wiv_meta_explicit.dcm");
    d.save(path);

    const Entries e = giv::ImagePluginHost::readMetadata(path);
    EXPECT_EQ(get(e, "DICOM summary", "Modality"), "MR");
    EXPECT_EQ(get(e, "DICOM summary", "Transfer syntax"), "Explicit VR Little Endian");
    EXPECT_EQ(get(e, "DICOM summary", "Size (columns \xC3\x97 rows)"), "3 \xC3\x97 4");
    EXPECT_EQ(get(e, "DICOM summary", "Slice thickness (mm)"), "2.5");
    EXPECT_EQ(get(e, "DICOM patient", "(0010,0010) Patient's Name"), "Doe John");
    EXPECT_EQ(get(e, "DICOM identification", "(0008,2218) Anatomic Region Sequence"), "");
    EXPECT_EQ(get(e, "DICOM identification", "> Item 1"), "");
    EXPECT_EQ(get(e, "DICOM identification", "> (0008,0104) Code Meaning"), "Thoracic spine");
    EXPECT_EQ(get(e, "DICOM image pixel", "(7FE0,0010) Pixel Data"), "100000 bytes");
    EXPECT_EQ(get(e, "DICOM image relationship", "(0020,0013) Instance Number"), "7"); // read after the skipped pixel data
    EXPECT_EQ(get(e, "DICOM image pixel", "(0028,0010) Rows"), "4");
    std::filesystem::remove(path);
}

TEST_F(MetadataTest, DicomImplicitVrWithDefinedLengthSequence)
{
    Dcm d;
    d.begin("1.2.840.10008.1.2");
    d.text(0x08, 0x60, "CS", "CT");
    // Sequence with a defined length: item (defined length) holding Code Value.
    std::vector<uint8_t> item;
    {
        Dcm inner;
        inner.implicitVr = true;
        inner.b.clear();
        inner.text(0x08, 0x100, "SH", "T-11502");
        std::vector<uint8_t> hdr = {0x08 /*dummy*/};
        (void)hdr;
        const auto content = inner.b;
        Dcm wrap;
        wrap.implicitVr = true;
        wrap.b.clear();
        wrap.tag(0xFFFE, 0xE000);
        wrap.u32(uint32_t(content.size()));
        wrap.b.insert(wrap.b.end(), content.begin(), content.end());
        item = wrap.b;
    }
    d.elem(0x08, 0x2218, "SQ", item);
    d.us(0x28, 0x10, 2);
    const std::string path = tmp("wiv_meta_implicit.dcm");
    d.save(path);

    const Entries e = giv::ImagePluginHost::readMetadata(path);
    EXPECT_EQ(get(e, "DICOM summary", "Transfer syntax"), "Implicit VR Little Endian");
    EXPECT_EQ(get(e, "DICOM summary", "Modality"), "CT");
    EXPECT_EQ(get(e, "DICOM identification", "> (0008,0100) Code Value"), "T-11502");
    EXPECT_EQ(get(e, "DICOM image pixel", "(0028,0010) Rows"), "2"); // after the sequence
    std::filesystem::remove(path);
}

TEST_F(MetadataTest, DicomLatin1AndHebrewNames)
{
    Dcm d;
    d.begin("1.2.840.10008.1.2.1");
    d.text(0x08, 0x05, "CS", "ISO_IR 100");
    d.text(0x10, 0x10, "PN", std::string("M\xFC") + "ller^J\xF6rg"); // M u-umlaut ller^J o-umlaut rg in Latin-1
    const std::string path = tmp("wiv_meta_latin1.dcm");
    d.save(path);
    EXPECT_EQ(get(giv::ImagePluginHost::readMetadata(path), "DICOM patient", "(0010,0010) Patient's Name"),
              "M\xC3\xBC" "ller J\xC3\xB6rg");

    Dcm h;
    h.begin("1.2.840.10008.1.2.1");
    h.text(0x08, 0x05, "CS", "ISO_IR 138");
    h.text(0x10, 0x10, "PN", "\xF9\xEC\xE5\xED"); // shin lamed vav final-mem in ISO 8859-8
    h.save(path);
    EXPECT_EQ(get(giv::ImagePluginHost::readMetadata(path), "DICOM patient", "(0010,0010) Patient's Name"),
              "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");
    std::filesystem::remove(path);
}

TEST_F(MetadataTest, DicomTruncatedAndGarbageFilesDoNotCrash)
{
    Dcm d;
    d.begin("1.2.840.10008.1.2.1");
    d.text(0x08, 0x60, "CS", "MR");
    d.elem(0x08, 0x2218, "SQ", {}, true, 0xFFFFFFFF); // undefined-length sequence that never ends
    const std::string path = tmp("wiv_meta_trunc.dcm");
    d.save(path);
    const Entries e = giv::ImagePluginHost::readMetadata(path);
    EXPECT_EQ(get(e, "DICOM summary", "Modality"), "MR");

    Dcm g;
    g.begin("1.2.840.10008.1.2.1");
    for (int i = 0; i < 4000; ++i) g.b.push_back(uint8_t((i * 131 + 7) & 0xFF));
    g.save(path);
    giv::ImagePluginHost::readMetadata(path); // must simply return
    // A length that runs past the end of the file.
    Dcm h;
    h.begin("1.2.840.10008.1.2.1");
    h.tag(0x08, 0x70); h.b.insert(h.b.end(), {'L', 'O'}); h.u16(60000);
    h.save(path);
    giv::ImagePluginHost::readMetadata(path);
    std::filesystem::remove(path);
}

// ---- NumPy, HDF5, netCDF (fixtures made by tests/data/make_fixtures.sh) ------------------------------------

namespace
{
std::string data(const char* name) { return std::string(WIV_TEST_DATA_DIR) + "/" + name; }
} // namespace

TEST_F(MetadataTest, NpyHeader)
{
    const Entries e = giv::ImagePluginHost::readMetadata(data("meta_test.npy"));
    if (e.empty()) GTEST_SKIP() << "npy plugin not built";
    EXPECT_EQ(get(e, "NumPy array", "dtype"), "<f4");
    EXPECT_EQ(get(e, "NumPy array", "Type"), "float, 4 bytes");
    EXPECT_EQ(get(e, "NumPy array", "Byte order"), "little-endian");
    EXPECT_EQ(get(e, "NumPy array", "Shape"), "(3, 5)");
    EXPECT_EQ(get(e, "NumPy array", "Memory order"), "C (row-major)");
    EXPECT_EQ(get(e, "NumPy array", "Data size"), "60");
}

TEST_F(MetadataTest, Hdf5DatasetAndAttributesFollowThePage)
{
    const Entries e0 = giv::ImagePluginHost::readMetadata(data("meta_test.h5"), 0);
    if (e0.empty()) GTEST_SKIP() << "hdf5 plugin not built";
    EXPECT_EQ(get(e0, "HDF5 dataset", "Path"), "/scan/image");
    EXPECT_EQ(get(e0, "HDF5 dataset", "Shape"), "16 \xC3\x97 32");
    EXPECT_EQ(get(e0, "HDF5 dataset", "Data type"), "float32");
    EXPECT_EQ(get(e0, "HDF5 dataset", "Storage"), "chunked");
    EXPECT_EQ(get(e0, "HDF5 dataset", "Chunk shape"), "8 \xC3\x97 8");
    EXPECT_EQ(get(e0, "HDF5 dataset", "Filters"), "shuffle, deflate (gzip)");
    EXPECT_EQ(get(e0, "HDF5 dataset attributes", "units"), "counts");
    EXPECT_EQ(get(e0, "HDF5 dataset attributes", "gain"), "1.5");
    EXPECT_EQ(get(e0, "HDF5 group attributes", "operator"), "nobody");
    EXPECT_EQ(get(e0, "HDF5 file attributes", "instrument"), "wiv test rig");
    EXPECT_EQ(get(e0, "HDF5 file attributes", "exposures"), "1, 2, 3");
    EXPECT_EQ(get(e0, "HDF5 file", "Image datasets"), "2");

    // The other dataset: its own description, and none of the first one's attributes.
    const Entries e1 = giv::ImagePluginHost::readMetadata(data("meta_test.h5"), 1);
    EXPECT_EQ(get(e1, "HDF5 dataset", "Path"), "/thumb");
    EXPECT_EQ(get(e1, "HDF5 dataset", "Data type"), "uint16");
    EXPECT_EQ(get(e1, "HDF5 dataset", "Filters"), "none");
    EXPECT_EQ(get(e1, "HDF5 dataset attributes", "units"), "<missing>");
    EXPECT_EQ(get(e1, "HDF5 file attributes", "instrument"), "wiv test rig");
}

TEST_F(MetadataTest, NetcdfVariableAndGlobalAttributesFollowThePage)
{
    const Entries e0 = giv::ImagePluginHost::readMetadata(data("meta_test.nc"), 0);
    if (e0.empty()) GTEST_SKIP() << "netcdf plugin not built";
    EXPECT_EQ(get(e0, "netCDF variable", "Name"), "temp");
    EXPECT_EQ(get(e0, "netCDF variable", "Data type"), "float32");
    EXPECT_EQ(get(e0, "netCDF variable", "Dimensions"), "time=2, lat=4, lon=6");
    EXPECT_EQ(get(e0, "netCDF file", "Format"), "classic (CDF-1)");
    EXPECT_EQ(get(e0, "netCDF file", "Dimensions"), "lat = 4, lon = 6, time = 2 (unlimited)");
    EXPECT_EQ(get(e0, "netCDF variable attributes", "units"), "K");
    EXPECT_EQ(get(e0, "netCDF variable attributes", "_FillValue"), "-999");
    EXPECT_EQ(get(e0, "netCDF variable attributes", "valid_range"), "200, 330");
    EXPECT_EQ(get(e0, "netCDF global attributes", "title"), "wiv metadata test");
    EXPECT_EQ(get(e0, "netCDF global attributes", "Conventions"), "CF-1.8");
    // A 12000-character attribute is read in full (no buffer overrun) and then cut for display.
    const std::string history = get(e0, "netCDF global attributes", "history");
    EXPECT_EQ(history.size(), 4000u + 4u);
    EXPECT_EQ(history.substr(history.size() - 4), " ...");
    EXPECT_EQ(history.substr(0, 10), "hhhhhhhhhh");

    const Entries e1 = giv::ImagePluginHost::readMetadata(data("meta_test.nc"), 1);
    EXPECT_EQ(get(e1, "netCDF variable", "Name"), "count");
    EXPECT_EQ(get(e1, "netCDF variable", "Data type"), "int16");
    EXPECT_EQ(get(e1, "netCDF variable attributes", "scale_factor"), "0.5");
    EXPECT_EQ(get(e1, "netCDF variable attributes", "units"), "<missing>");
}
