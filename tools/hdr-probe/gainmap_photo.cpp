#include "gainmap_photo.h"

#include <QColorSpace>
#include <QColorTransform>
#include <QtGui/qrgbafloat.h>

#include <array>
#include <cstring>
#include <utility>

namespace GainmapPhotoProbe {
bool prepareUpload(QImage& upload, QString& sourcePrimaries, QString& error)
{
    const QColorSpace source = upload.colorSpace();
    if (upload.format() != QImage::Format_RGBA16FPx4 || !upload.isDetached() || !source.isValid() ||
        source.transferFunction() != QColorSpace::TransferFunction::Linear) {
        error = QStringLiteral("The decoder did not return an owned linear FP16 raster.");
        return false;
    }
    const auto transform = source.transformationToColorSpace(QColorSpace::SRgbLinear);
    const auto* storage = upload.constBits();
    using Pixel = std::array<qfloat16, 4>;
    for (int y = 0; y < upload.height(); ++y) {
        auto* row = upload.scanLine(y);
        for (int x = 0; x < upload.width(); ++x, row += sizeof(Pixel)) {
            Pixel rgba;
            std::memcpy(rgba.data(), row, sizeof(Pixel));
            if (float(rgba[3]) != 1.0f) {
                error = QStringLiteral("JPEG gain-map output must be opaque.");
                return false;
            }
            // The reconstruction is already relative to SDR white. Apply
            // only the extended gamut matrix and preserve signed components.
            const auto mapped =
                transform.map(QRgbaFloat32{float(rgba[0]), float(rgba[1]), float(rgba[2]), 1.0f});
            const Pixel pixel{qfloat16(mapped.r), qfloat16(mapped.g), qfloat16(mapped.b), rgba[3]};
            std::memcpy(row, pixel.data(), sizeof(Pixel));
        }
    }

    // With opaque alpha the premultiplied layout uses the same samples.
    if (!upload.reinterpretAsFormat(QImage::Format_RGBA16FPx4_Premultiplied)) {
        error = QStringLiteral("Unable to select the FP16 upload layout.");
        return false;
    }
    upload.setColorSpace(QColorSpace()); // Already expressed in extended linear sRGB.
    if (upload.constBits() != storage) {
        error = QStringLiteral("FP16 upload did not retain the decoded raster allocation.");
        return false;
    }
    sourcePrimaries = source.primaries() == QColorSpace::Primaries::DciP3D65
                               ? QStringLiteral("display_p3")
                               : QStringLiteral("srgb");
    return true;
}
} // namespace GainmapPhotoProbe
