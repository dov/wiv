// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ExportWriter.h"

#include "Qoi.h"

#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>

#include <algorithm>

namespace givqt
{

ExportFormat exportFormatFromPath(const QString& path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    if (s == "png") return ExportFormat::Png;
    if (s == "jpg" || s == "jpeg") return ExportFormat::Jpeg;
    if (s == "webp") return ExportFormat::WebP;
    if (s == "tif" || s == "tiff") return ExportFormat::Tiff;
    if (s == "bmp") return ExportFormat::Bmp;
    if (s == "qoi") return ExportFormat::Qoi;
    return ExportFormat::Other;
}

ExportOptionSupport exportOptionSupport(ExportFormat format)
{
    ExportOptionSupport s;
    switch (format)
    {
        case ExportFormat::Png: s.pngCompression = true; break;
        case ExportFormat::Jpeg: s.quality = s.progressive = true; break;
        case ExportFormat::WebP: s.quality = s.lossless = true; break;
        case ExportFormat::Tiff: s.tiffCompression = true; break;
        default: break;
    }
    return s;
}

bool writeExportImage(const QImage& image, const QString& path, const ExportOptions& options, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        QFile::remove(path);
        return false;
    };
    const ExportFormat format = exportFormatFromPath(path);

    if (format == ExportFormat::Qoi)
    {
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
        const bool opaque = !image.hasAlphaChannel();
        std::vector<uint8_t> bytes;
        if (rgba.bytesPerLine() == rgba.width() * 4)
            bytes = giv::qoi::encode(rgba.constBits(), rgba.width(), rgba.height(), opaque ? 3 : 4);
        else
        {
            std::vector<uint8_t> tight(size_t(rgba.width()) * rgba.height() * 4);
            for (int y = 0; y < rgba.height(); ++y)
                std::copy_n(rgba.constScanLine(y), rgba.width() * 4, tight.data() + size_t(y) * rgba.width() * 4);
            bytes = giv::qoi::encode(tight.data(), rgba.width(), rgba.height(), opaque ? 3 : 4);
        }
        if (bytes.empty()) return fail("could not encode the image as QOI");
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) ||
            f.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()))
            return fail(QString("could not write %1").arg(path));
        return true;
    }

    QImageWriter writer(path);
    switch (format)
    {
        case ExportFormat::Jpeg:
            if (options.quality >= 0) writer.setQuality(std::clamp(options.quality, 0, 100));
            writer.setProgressiveScanWrite(options.progressive);
            break;
        case ExportFormat::WebP:
            // Qt's WebP writer is lossless at quality 100.
            writer.setQuality(options.lossless ? 100 : (options.quality >= 0 ? std::min(options.quality, 99) : 75));
            break;
        case ExportFormat::Png:
            if (options.pngCompression >= 0)
            {
                // Qt maps quality q to zlib level (100 - q) * 9 / 91.
                const int level = std::clamp(options.pngCompression, 0, 9);
                writer.setQuality(level == 0 ? 100 : std::max(0, 100 - (level * 91 + 8) / 9));
            }
            break;
        case ExportFormat::Tiff:
            if (options.tiffCompression >= 0) writer.setCompression(options.tiffCompression);
            break;
        default: break;
    }
    // JPEG has no alpha: flatten onto white (the export is a render of the view).
    QImage toWrite = image;
    if (format == ExportFormat::Jpeg && image.hasAlphaChannel())
    {
        toWrite = QImage(image.size(), QImage::Format_RGB32);
        toWrite.fill(Qt::white);
        QPainter p(&toWrite);
        p.drawImage(0, 0, image);
    }
    if (!writer.write(toWrite)) return fail(QString("could not write %1: %2").arg(path, writer.errorString()));
    return true;
}

} // namespace givqt
