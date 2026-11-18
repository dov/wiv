// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// dicom_tags.h - a curated DICOM data dictionary for the Image Info panel: the names and value
// representations of the tags people actually look at (identification, patient, acquisition, image
// plane, pixel format, enhanced multi-frame), plus the names of common transfer syntaxes and SOP
// classes. Not the full PS3.6 registry; unlisted tags are shown as "(gggg,eeee)".
//
#pragma once

#include <cstdint>
#include <cstring>

namespace dicom_tags
{

struct TagInfo
{
    uint32_t tag; // group << 16 | element
    const char* vr;
    const char* name;
};

#define T(g, e) ((uint32_t(0x##g) << 16) | 0x##e)

inline const TagInfo kTags[] = {
    // --- File meta information ---
    {T(0002, 0000), "UL", "File Meta Information Group Length"},
    {T(0002, 0001), "OB", "File Meta Information Version"},
    {T(0002, 0002), "UI", "Media Storage SOP Class UID"},
    {T(0002, 0003), "UI", "Media Storage SOP Instance UID"},
    {T(0002, 0010), "UI", "Transfer Syntax UID"},
    {T(0002, 0012), "UI", "Implementation Class UID"},
    {T(0002, 0013), "SH", "Implementation Version Name"},
    {T(0002, 0016), "AE", "Source Application Entity Title"},
    // --- Identification ---
    {T(0008, 0005), "CS", "Specific Character Set"},
    {T(0008, 0008), "CS", "Image Type"},
    {T(0008, 0012), "DA", "Instance Creation Date"},
    {T(0008, 0013), "TM", "Instance Creation Time"},
    {T(0008, 0014), "UI", "Instance Creator UID"},
    {T(0008, 0016), "UI", "SOP Class UID"},
    {T(0008, 0018), "UI", "SOP Instance UID"},
    {T(0008, 0020), "DA", "Study Date"},
    {T(0008, 0021), "DA", "Series Date"},
    {T(0008, 0022), "DA", "Acquisition Date"},
    {T(0008, 0023), "DA", "Content Date"},
    {T(0008, 0030), "TM", "Study Time"},
    {T(0008, 0031), "TM", "Series Time"},
    {T(0008, 0032), "TM", "Acquisition Time"},
    {T(0008, 0033), "TM", "Content Time"},
    {T(0008, 0050), "SH", "Accession Number"},
    {T(0008, 0060), "CS", "Modality"},
    {T(0008, 0064), "CS", "Conversion Type"},
    {T(0008, 0070), "LO", "Manufacturer"},
    {T(0008, 0080), "LO", "Institution Name"},
    {T(0008, 0081), "ST", "Institution Address"},
    {T(0008, 0090), "PN", "Referring Physician's Name"},
    {T(0008, 0096), "SQ", "Referring Physician Identification Sequence"},
    {T(0008, 002A), "DT", "Acquisition DateTime"},
    {T(0008, 0061), "CS", "Modalities in Study"},
    {T(0008, 0068), "CS", "Presentation Intent Type"},
    {T(0008, 0100), "SH", "Code Value"},
    {T(0008, 0102), "SH", "Coding Scheme Designator"},
    {T(0008, 0104), "LO", "Code Meaning"},
    {T(0008, 0201), "SH", "Timezone Offset From UTC"},
    {T(0008, 1010), "SH", "Station Name"},
    {T(0008, 1030), "LO", "Study Description"},
    {T(0008, 1032), "SQ", "Procedure Code Sequence"},
    {T(0008, 103E), "LO", "Series Description"},
    {T(0008, 1040), "LO", "Institutional Department Name"},
    {T(0008, 1050), "PN", "Performing Physician's Name"},
    {T(0008, 1060), "PN", "Name of Physician(s) Reading Study"},
    {T(0008, 1070), "PN", "Operators' Name"},
    {T(0008, 1090), "LO", "Manufacturer's Model Name"},
    {T(0008, 1110), "SQ", "Referenced Study Sequence"},
    {T(0008, 1111), "SQ", "Referenced Performed Procedure Step Sequence"},
    {T(0008, 1140), "SQ", "Referenced Image Sequence"},
    {T(0008, 1150), "UI", "Referenced SOP Class UID"},
    {T(0008, 1155), "UI", "Referenced SOP Instance UID"},
    {T(0008, 2111), "ST", "Derivation Description"},
    {T(0008, 2112), "SQ", "Source Image Sequence"},
    {T(0008, 2218), "SQ", "Anatomic Region Sequence"},
    // --- Patient ---
    {T(0010, 0010), "PN", "Patient's Name"},
    {T(0010, 0020), "LO", "Patient ID"},
    {T(0010, 0021), "LO", "Issuer of Patient ID"},
    {T(0010, 0030), "DA", "Patient's Birth Date"},
    {T(0010, 0032), "TM", "Patient's Birth Time"},
    {T(0010, 0040), "CS", "Patient's Sex"},
    {T(0010, 1000), "LO", "Other Patient IDs"},
    {T(0010, 1001), "PN", "Other Patient Names"},
    {T(0010, 1010), "AS", "Patient's Age"},
    {T(0010, 1020), "DS", "Patient's Size (m)"},
    {T(0010, 1030), "DS", "Patient's Weight (kg)"},
    {T(0010, 1040), "LO", "Patient's Address"},
    {T(0010, 2154), "SH", "Patient's Telephone Numbers"},
    {T(0010, 2160), "SH", "Ethnic Group"},
    {T(0010, 2180), "SH", "Occupation"},
    {T(0010, 21B0), "LT", "Additional Patient History"},
    {T(0010, 4000), "LT", "Patient Comments"},
    // --- Clinical trial / de-identification ---
    {T(0012, 0010), "LO", "Clinical Trial Sponsor Name"},
    {T(0012, 0020), "LO", "Clinical Trial Protocol ID"},
    {T(0012, 0062), "CS", "Patient Identity Removed"},
    {T(0012, 0063), "LO", "De-identification Method"},
    // --- Acquisition ---
    {T(0018, 0010), "LO", "Contrast/Bolus Agent"},
    {T(0018, 0015), "CS", "Body Part Examined"},
    {T(0018, 0020), "CS", "Scanning Sequence"},
    {T(0018, 0021), "CS", "Sequence Variant"},
    {T(0018, 0022), "CS", "Scan Options"},
    {T(0018, 0023), "CS", "MR Acquisition Type"},
    {T(0018, 0024), "SH", "Sequence Name"},
    {T(0018, 0025), "CS", "Angio Flag"},
    {T(0018, 0050), "DS", "Slice Thickness (mm)"},
    {T(0018, 0060), "DS", "KVP"},
    {T(0018, 0080), "DS", "Repetition Time (ms)"},
    {T(0018, 0081), "DS", "Echo Time (ms)"},
    {T(0018, 0082), "DS", "Inversion Time (ms)"},
    {T(0018, 0083), "DS", "Number of Averages"},
    {T(0018, 0084), "DS", "Imaging Frequency (MHz)"},
    {T(0018, 0085), "SH", "Imaged Nucleus"},
    {T(0018, 0086), "IS", "Echo Number(s)"},
    {T(0018, 0087), "DS", "Magnetic Field Strength (T)"},
    {T(0018, 0088), "DS", "Spacing Between Slices (mm)"},
    {T(0018, 0089), "IS", "Number of Phase Encoding Steps"},
    {T(0018, 0090), "DS", "Data Collection Diameter (mm)"},
    {T(0018, 0091), "IS", "Echo Train Length"},
    {T(0018, 0093), "DS", "Percent Sampling"},
    {T(0018, 0094), "DS", "Percent Phase Field of View"},
    {T(0018, 0095), "DS", "Pixel Bandwidth"},
    {T(0018, 1000), "LO", "Device Serial Number"},
    {T(0018, 1004), "LO", "Plate ID"},
    {T(0018, 1020), "LO", "Software Version(s)"},
    {T(0018, 1030), "LO", "Protocol Name"},
    {T(0018, 1041), "DS", "Contrast/Bolus Volume (ml)"},
    {T(0018, 1050), "DS", "Spatial Resolution (mm)"},
    {T(0018, 1060), "DS", "Trigger Time (ms)"},
    {T(0018, 1063), "DS", "Frame Time (ms)"},
    {T(0018, 1088), "IS", "Heart Rate"},
    {T(0018, 1100), "DS", "Reconstruction Diameter (mm)"},
    {T(0018, 1110), "DS", "Distance Source to Detector (mm)"},
    {T(0018, 1111), "DS", "Distance Source to Patient (mm)"},
    {T(0018, 1120), "DS", "Gantry/Detector Tilt (deg)"},
    {T(0018, 1130), "DS", "Table Height (mm)"},
    {T(0018, 1140), "CS", "Rotation Direction"},
    {T(0018, 1150), "IS", "Exposure Time (ms)"},
    {T(0018, 1151), "IS", "X-Ray Tube Current (mA)"},
    {T(0018, 1152), "IS", "Exposure (mAs)"},
    {T(0018, 1153), "IS", "Exposure in \xC2\xB5" "As"},
    {T(0018, 1160), "SH", "Filter Type"},
    {T(0018, 1170), "IS", "Generator Power (kW)"},
    {T(0018, 1190), "DS", "Focal Spot(s) (mm)"},
    {T(0018, 1008), "SH", "Gantry ID"},
    {T(0018, 1164), "DS", "Imager Pixel Spacing (mm)"},
    {T(0018, 1166), "CS", "Grid"},
    {T(0018, 1210), "SH", "Convolution Kernel"},
    {T(0018, 1250), "SH", "Receive Coil Name"},
    {T(0018, 1251), "SH", "Transmit Coil Name"},
    {T(0018, 1310), "US", "Acquisition Matrix"},
    {T(0018, 1312), "CS", "In-plane Phase Encoding Direction"},
    {T(0018, 1314), "DS", "Flip Angle (deg)"},
    {T(0018, 1315), "CS", "Variable Flip Angle Flag"},
    {T(0018, 1316), "DS", "SAR (W/kg)"},
    {T(0018, 1318), "DS", "dB/dt (T/s)"},
    {T(0018, 1405), "IS", "Relative X-Ray Exposure"},
    {T(0018, 1411), "DS", "Exposure Index"},
    {T(0018, 1412), "DS", "Target Exposure Index"},
    {T(0018, 1413), "DS", "Deviation Index"},
    {T(0018, 1508), "CS", "Positioner Type"},
    {T(0018, 5100), "CS", "Patient Position"},
    {T(0018, 5101), "CS", "View Position"},
    {T(0018, 6000), "DS", "Sensitivity"},
    {T(0018, 7004), "CS", "Detector Type"},
    {T(0018, 700A), "SH", "Detector ID"},
    {T(0018, 7050), "CS", "Filter Material"},
    // --- Image plane / relationship ---
    {T(0020, 000D), "UI", "Study Instance UID"},
    {T(0020, 000E), "UI", "Series Instance UID"},
    {T(0020, 0010), "SH", "Study ID"},
    {T(0020, 0011), "IS", "Series Number"},
    {T(0020, 0012), "IS", "Acquisition Number"},
    {T(0020, 0013), "IS", "Instance Number"},
    {T(0020, 0020), "CS", "Patient Orientation"},
    {T(0020, 0032), "DS", "Image Position (Patient) (mm)"},
    {T(0020, 0037), "DS", "Image Orientation (Patient)"},
    {T(0020, 0052), "UI", "Frame of Reference UID"},
    {T(0020, 0060), "CS", "Laterality"},
    {T(0020, 0062), "CS", "Image Laterality"},
    {T(0020, 0100), "IS", "Temporal Position Identifier"},
    {T(0020, 0105), "IS", "Number of Temporal Positions"},
    {T(0020, 1002), "IS", "Images in Acquisition"},
    {T(0020, 1040), "LO", "Position Reference Indicator"},
    {T(0020, 1041), "DS", "Slice Location (mm)"},
    {T(0020, 4000), "LT", "Image Comments"},
    {T(0020, 9111), "SQ", "Frame Content Sequence"},
    {T(0020, 9113), "SQ", "Plane Position Sequence"},
    {T(0020, 9116), "SQ", "Plane Orientation Sequence"},
    // --- Image pixel ---
    {T(0028, 0002), "US", "Samples per Pixel"},
    {T(0028, 0004), "CS", "Photometric Interpretation"},
    {T(0028, 0006), "US", "Planar Configuration"},
    {T(0028, 0008), "IS", "Number of Frames"},
    {T(0028, 0009), "AT", "Frame Increment Pointer"},
    {T(0028, 0010), "US", "Rows"},
    {T(0028, 0011), "US", "Columns"},
    {T(0028, 0014), "US", "Ultrasound Color Data Present"},
    {T(0028, 0030), "DS", "Pixel Spacing (mm)"},
    {T(0028, 0034), "IS", "Pixel Aspect Ratio"},
    {T(0028, 0100), "US", "Bits Allocated"},
    {T(0028, 0101), "US", "Bits Stored"},
    {T(0028, 0102), "US", "High Bit"},
    {T(0028, 0103), "US", "Pixel Representation"},
    {T(0028, 0106), "US", "Smallest Image Pixel Value"},
    {T(0028, 0107), "US", "Largest Image Pixel Value"},
    {T(0028, 0120), "US", "Pixel Padding Value"},
    {T(0028, 0300), "CS", "Quality Control Image"},
    {T(0028, 0301), "CS", "Burned In Annotation"},
    {T(0028, 1050), "DS", "Window Center"},
    {T(0028, 1051), "DS", "Window Width"},
    {T(0028, 1052), "DS", "Rescale Intercept"},
    {T(0028, 1053), "DS", "Rescale Slope"},
    {T(0028, 1054), "LO", "Rescale Type"},
    {T(0028, 1055), "LO", "Window Center & Width Explanation"},
    {T(0028, 1056), "CS", "VOI LUT Function"},
    {T(0028, 2110), "CS", "Lossy Image Compression"},
    {T(0028, 2112), "DS", "Lossy Image Compression Ratio"},
    {T(0028, 2114), "CS", "Lossy Image Compression Method"},
    {T(0028, 9110), "SQ", "Pixel Measures Sequence"},
    {T(0028, 9132), "SQ", "Frame VOI LUT Sequence"},
    {T(0028, 9145), "SQ", "Pixel Value Transformation Sequence"},
    // --- Procedure / requests ---
    {T(0032, 1032), "PN", "Requesting Physician"},
    {T(0032, 1060), "LO", "Requested Procedure Description"},
    {T(0040, 0244), "DA", "Performed Procedure Step Start Date"},
    {T(0040, 0245), "TM", "Performed Procedure Step Start Time"},
    {T(0040, 0253), "SH", "Performed Procedure Step ID"},
    {T(0040, 0254), "LO", "Performed Procedure Step Description"},
    {T(0040, 0275), "SQ", "Request Attributes Sequence"},
    {T(0040, 2016), "LO", "Placer Order Number"},
    {T(0040, 2017), "LO", "Filler Order Number"},
    // --- Nuclear medicine / PET ---
    {T(0054, 0016), "SQ", "Radiopharmaceutical Information Sequence"},
    {T(0054, 0081), "US", "Number of Slices"},
    {T(0054, 0101), "US", "Number of Time Slices"},
    {T(0054, 1001), "CS", "Units"},
    {T(0054, 1002), "CS", "Counts Source"},
    {T(0054, 1100), "CS", "Randoms Correction Method"},
    {T(0054, 1101), "LO", "Attenuation Correction Method"},
    {T(0054, 1102), "CS", "Decay Correction"},
    {T(0054, 1103), "LO", "Reconstruction Method"},
    {T(0054, 1300), "DS", "Frame Reference Time"},
    // --- Enhanced multi-frame ---
    {T(5200, 9229), "SQ", "Shared Functional Groups Sequence"},
    {T(5200, 9230), "SQ", "Per-frame Functional Groups Sequence"},
    // --- Pixel data ---
    {T(7FE0, 0008), "OF", "Float Pixel Data"},
    {T(7FE0, 0009), "OD", "Double Float Pixel Data"},
    {T(7FE0, 0010), "OW", "Pixel Data"},
};

#undef T

inline const TagInfo* findTag(uint32_t tag)
{
    for (const TagInfo& t : kTags)
        if (t.tag == tag) return &t;
    return nullptr;
}

struct UidName
{
    const char* uid;
    const char* name;
};

inline const UidName kUids[] = {
    // transfer syntaxes
    {"1.2.840.10008.1.2", "Implicit VR Little Endian"},
    {"1.2.840.10008.1.2.1", "Explicit VR Little Endian"},
    {"1.2.840.10008.1.2.1.99", "Deflated Explicit VR Little Endian"},
    {"1.2.840.10008.1.2.2", "Explicit VR Big Endian"},
    {"1.2.840.10008.1.2.4.50", "JPEG Baseline (Process 1)"},
    {"1.2.840.10008.1.2.4.51", "JPEG Extended (Process 2 & 4)"},
    {"1.2.840.10008.1.2.4.57", "JPEG Lossless (Process 14)"},
    {"1.2.840.10008.1.2.4.70", "JPEG Lossless (Process 14, SV1)"},
    {"1.2.840.10008.1.2.4.80", "JPEG-LS Lossless"},
    {"1.2.840.10008.1.2.4.81", "JPEG-LS Near-Lossless"},
    {"1.2.840.10008.1.2.4.90", "JPEG 2000 Lossless"},
    {"1.2.840.10008.1.2.4.91", "JPEG 2000"},
    {"1.2.840.10008.1.2.4.201", "High-Throughput JPEG 2000 Lossless"},
    {"1.2.840.10008.1.2.4.202", "High-Throughput JPEG 2000 RPCL Lossless"},
    {"1.2.840.10008.1.2.4.203", "High-Throughput JPEG 2000"},
    {"1.2.840.10008.1.2.5", "RLE Lossless"},
    // SOP classes
    {"1.2.840.10008.5.1.4.1.1.1", "CR Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.1.1", "Digital X-Ray Image Storage (for presentation)"},
    {"1.2.840.10008.5.1.4.1.1.2", "CT Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.2.1", "Enhanced CT Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.3.1", "Ultrasound Multi-frame Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.4", "MR Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.4.1", "Enhanced MR Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.6.1", "Ultrasound Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.7", "Secondary Capture Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.12.1", "X-Ray Angiographic Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.20", "Nuclear Medicine Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.104.1", "Encapsulated PDF Storage"},
    {"1.2.840.10008.5.1.4.1.1.128", "PET Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.130", "Enhanced PET Image Storage"},
    {"1.2.840.10008.5.1.4.1.1.481.2", "RT Dose Storage"},
    {"1.2.840.10008.5.1.4.1.1.481.3", "RT Structure Set Storage"},
};

inline const char* uidName(const char* uid)
{
    for (const UidName& u : kUids)
        if (std::strcmp(u.uid, uid) == 0) return u.name;
    return nullptr;
}

} // namespace dicom_tags
