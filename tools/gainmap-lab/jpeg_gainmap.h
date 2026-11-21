#pragma once

#include "io/external_file_identity.h"

#include <QImage>

#include <atomic>
#include <functional>
#include <optional>

namespace Licasa::GainmapLab {
inline constexpr quint64 maximumRasterPixels = 16 * 1024 * 1024;

struct Limits {
    quint64 compressedBytes = 16 * 1024 * 1024;
    quint64 rasterPixels =
        4 * 1024 * 1024; // Combined primary and gain-map pixels, capped at 16 Mi.
};

struct Result {
    QImage linear; // Tagged linear RGBA16F relative to the SDR white level.
    QSize gainSize;
    qsizetype jpegBytes;
};

// Memory: the shared budget refused the allowance for a valid raster. The
// refusal can end when another holder retires, unlike the other failures.
enum class DecodeFailure { Unavailable, NoGainMap, Memory };

std::optional<Result> decode(const QString& path, const ExternalFileIdentity& expected,
                             float headroom, QString& error, Limits limits = {},
                             const std::atomic_bool* cancelled = nullptr,
                             DecodeFailure* failure = nullptr,
                             const std::function<bool(quint64)>& reserveRaster = {});
} // namespace Licasa::GainmapLab
