// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// ExportWriter.h - File > Export: writes a rendered view to PNG, JPEG, WebP, TIFF, BMP or QOI
// with per-format save options (quality, compression, ...).
//
#include <QImage>
#include <QString>

namespace givqt
{

enum class ExportFormat
{
    Png,
    Jpeg,
    WebP,
    Tiff,
    Bmp,
    Qoi,
    Other, // anything else QImageWriter knows by suffix; no options
};

// What the user can tune; -1 / false mean "the format's default".
struct ExportOptions
{
    int quality = -1;         // JPEG, WebP: 0-100 (higher is better)
    bool lossless = false;    // WebP
    bool progressive = false; // JPEG
    int pngCompression = -1;  // PNG: zlib level 0 (none) - 9 (smallest)
    int tiffCompression = -1; // TIFF: 0 none, 1 LZW
};

ExportFormat exportFormatFromPath(const QString& path);

// Which options apply to `format` (drives the Save Options dialog).
struct ExportOptionSupport
{
    bool quality = false, lossless = false, progressive = false, pngCompression = false, tiffCompression = false;
    bool any() const { return quality || lossless || progressive || pngCompression || tiffCompression; }
};
ExportOptionSupport exportOptionSupport(ExportFormat format);

// Writes `image`; false and *error on failure (a partial file is removed).
bool writeExportImage(const QImage& image, const QString& path, const ExportOptions& options, QString* error = nullptr);

} // namespace givqt
