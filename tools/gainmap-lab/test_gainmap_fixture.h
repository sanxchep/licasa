#pragma once

// Test-only. Encodes a synthetic Ultra HDR JPEG with the lab codec, so that
// limit tests can use exact dimensions without a checked-in camera-sized file.

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QtCore/qfloat16.h>

#include <ultrahdr_api.h>

#include <algorithm>
#include <random>
#include <vector>

namespace Licasa::GainmapLab::TestFixture {
enum class Texture { Ramp, Noise };

// A horizontal ramp or seeded noise from black to four times SDR white in linear BT.709.
// The gain map has size / gainScale pixels on each side. Dimensions must be even.
// A cool ramp is blue where the warm one is red, so two photos can be told apart.
inline QByteArray encodeGainMapJpeg(QSize size, int gainScale, bool cool = false,
                                    QString* error = nullptr, Texture texture = Texture::Ramp)
{
    const auto fail = [&](const QString& message) {
        if (error) {
            *error = message;
        }
        return QByteArray();
    };
    if (size.width() < 8 || size.height() < 8 || size.width() % 2 || size.height() % 2 ||
        gainScale < 1) {
        return fail(QStringLiteral("Unsupported synthetic fixture geometry."));
    }
    std::vector<qfloat16> pixels(size_t(size.width()) * size.height() * 4);
    if (texture == Texture::Noise) {
        // Keep the three channels equal so the high entropy is in luma, where
        // JPEG subsampling does not remove it. The seed fixes the encoded input.
        std::mt19937 random(21);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const qfloat16 level(4.0f * float(random() >> 24) / 255.0f);
            pixels[i] = pixels[i + 1] = pixels[i + 2] = level;
            pixels[i + 3] = qfloat16(1.0f);
        }
    } else {
        std::vector<qfloat16> row(size_t(size.width()) * 4);
        for (int x = 0; x < size.width(); ++x) {
            const float level = 4.0f * float(x) / float(size.width() - 1);
            row[size_t(x) * 4] = qfloat16(level * (cool ? 0.25f : 1.0f));
            row[size_t(x) * 4 + 1] = qfloat16(level * 0.6f);
            row[size_t(x) * 4 + 2] = qfloat16(level * (cool ? 1.0f : 0.25f));
            row[size_t(x) * 4 + 3] = qfloat16(1.0f);
        }
        for (int y = 0; y < size.height(); ++y) {
            std::copy(row.begin(), row.end(), pixels.begin() + size_t(y) * row.size());
        }
    }

    uhdr_raw_image_t raw{};
    raw.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
    raw.cg = UHDR_CG_BT_709;
    raw.ct = UHDR_CT_LINEAR;
    raw.range = UHDR_CR_FULL_RANGE;
    raw.w = unsigned(size.width());
    raw.h = unsigned(size.height());
    raw.planes[UHDR_PLANE_PACKED] = pixels.data();
    raw.stride[UHDR_PLANE_PACKED] = unsigned(size.width());

    auto* encoder = uhdr_create_encoder();
    if (!encoder) {
        return fail(QStringLiteral("The lab codec has no encoder."));
    }
    QByteArray bytes;
    const auto accepted = [&](uhdr_error_info_t status, const char* step) {
        if (status.error_code == UHDR_CODEC_OK) {
            return true;
        }
        if (error) {
            *error = QStringLiteral("%1: %2").arg(
                QLatin1String(step),
                status.has_detail ? QString::fromUtf8(status.detail) : QString());
        }
        return false;
    };
    const int baseQuality = texture == Texture::Noise ? 100 : 60;
    if (accepted(uhdr_enc_set_raw_image(encoder, &raw, UHDR_HDR_IMG), "raw image") &&
        accepted(uhdr_enc_set_quality(encoder, baseQuality, UHDR_BASE_IMG), "base quality") &&
        accepted(uhdr_enc_set_quality(encoder, 60, UHDR_GAIN_MAP_IMG), "gain-map quality") &&
        accepted(uhdr_enc_set_gainmap_scale_factor(encoder, gainScale), "gain-map scale") &&
        accepted(uhdr_encode(encoder), "encode")) {
        if (const auto* stream = uhdr_get_encoded_stream(encoder)) {
            bytes = QByteArray(static_cast<const char*>(stream->data), qsizetype(stream->data_sz));
        }
    }
    uhdr_release_encoder(encoder);
    return bytes;
}
} // namespace Licasa::GainmapLab::TestFixture
