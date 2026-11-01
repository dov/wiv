//
// fits_plugin.cpp - wiv image plugin for FITS (Flexible Image Transport
// System) files, via cfitsio. Modeled on giv's
// ~/hd/github/giv/src/plugins/fits.c, simplified by letting cfitsio itself
// do the bitpix -> output-type conversion (applying BSCALE/BZERO) instead
// of manually mapping every FITS storage type: 8/16-bit data is read as
// unsigned (giv's own bitpix_to_data_type() already treated 16-bit as
// unsigned rather than the FITS-standard signed int16, most likely because
// real instrument data stores unsigned samples via BZERO=32768 anyway - kept
// for compatibility), everything else (32-bit int, 32/64-bit float) is read
// out as float since wiv's raw sample buffer (see wiv_plugin.h's
// WivSampleType) has no 32-bit-int/double type. 3D files (NAXIS=3) are
// exposed as a multi-slice image (WivPluginImage::depth) for the Up/Down
// slice navigation - giv's own fits.c read the same NAXIS3 volume into one
// flat buffer but relied on its own (separate) slice-cycling UI code to page
// through it.
//
#include "../wiv_plugin_common.h"

#include <fitsio.h>

#include <cctype>
#include <cstring>
#include <mutex>
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

char* fitsError(int status, const char* filename)
{
    char statusMsg[FLEN_STATUS];
    fits_get_errstatus(status, statusMsg);
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "fits: %s (%s)", statusMsg, filename);
    return strdup(buf);
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasExtension(filename, ".fits") || hasExtension(filename, ".fit");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    // cfitsio keeps global state (I/O driver table, file handle table) and is
    // only thread-safe when built reentrant, which the Windows vcpkg build is
    // not. The Open dialog decodes thumbnails on several worker threads, so
    // serialize all calls.
    static std::mutex cfitsioMutex;
    std::lock_guard<std::mutex> lock(cfitsioMutex);

    fitsfile* fptr = nullptr;
    int status = 0;

    if (fits_open_file(&fptr, wiv_plugin::asciiSafePath(filename).c_str(), READONLY, &status))
    {
        if (error_msg)
            *error_msg = fitsError(status, filename);
        return nullptr;
    }

    int naxis = 0;
    if (fits_get_img_dim(fptr, &naxis, &status) || naxis < 2 || naxis > 3)
    {
        fits_close_file(fptr, &status);
        if (error_msg)
            *error_msg = naxis < 2 || naxis > 3
                             ? wiv_plugin::makeError("fits: only 2D/3D images are supported: %s", filename)
                             : fitsError(status, filename);
        return nullptr;
    }

    long axes[3] = {0, 0, 0};
    if (fits_get_img_size(fptr, naxis, axes, &status))
    {
        fits_close_file(fptr, &status);
        if (error_msg)
            *error_msg = fitsError(status, filename);
        return nullptr;
    }
    const int width = static_cast<int>(axes[0]);
    const int height = static_cast<int>(axes[1]);
    const int depth = naxis == 3 ? static_cast<int>(axes[2]) : 1;

    int bitpix = 0;
    if (fits_get_img_type(fptr, &bitpix, &status))
    {
        fits_close_file(fptr, &status);
        if (error_msg)
            *error_msg = fitsError(status, filename);
        return nullptr;
    }

    WivSampleType sampleType;
    int cfitsioType;
    switch (bitpix)
    {
        case BYTE_IMG: sampleType = WIV_SAMPLE_U8; cfitsioType = TBYTE; break;
        case SHORT_IMG: sampleType = WIV_SAMPLE_U16; cfitsioType = TUSHORT; break;
        default: sampleType = WIV_SAMPLE_FLOAT; cfitsioType = TFLOAT; break;
    }

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    wiv_plugin::allocSamples(img, sampleType, depth);

    const long nelements = static_cast<long>(width) * height * depth;
    int anynul = 0;
    if (fits_read_img(fptr, cfitsioType, 1, nelements, nullptr, img->samples, &anynul, &status))
    {
        fits_close_file(fptr, &status);
        wiv_plugin_free_image(img);
        if (error_msg)
            *error_msg = fitsError(status, filename);
        return nullptr;
    }
    fits_close_file(fptr, &status);

    // Slice-0 grayscale preview - never actually displayed for a raw-sample
    // image (see DisplayImage.h), just a reasonable placeholder.
    const size_t sliceElemCount = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < sliceElemCount; ++i)
    {
        double v;
        switch (sampleType)
        {
            case WIV_SAMPLE_U8: v = static_cast<const unsigned char*>(img->samples)[i]; break;
            case WIV_SAMPLE_U16: v = static_cast<const unsigned short*>(img->samples)[i]; break;
            default: v = static_cast<const float*>(img->samples)[i]; break;
        }
        unsigned char g = static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : v));
        img->rgba[i * 4 + 0] = g;
        img->rgba[i * 4 + 1] = g;
        img->rgba[i * 4 + 2] = g;
        img->rgba[i * 4 + 3] = 255;
    }

    return img;
}
