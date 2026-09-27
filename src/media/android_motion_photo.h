#pragma once

#include "io/byte_range_device.h"

#include <QByteArrayView>
#include <QString>

#include <atomic>
#include <optional>

namespace Licasa {

enum class AndroidMotionPhotoStatus {
    NotMotionPhoto,
    Valid,
    Invalid,
};

struct AndroidMotionPhotoV1 {
    QString primaryMimeType;
    QString videoMimeType;
    quint64 primaryImageLength = 0;
    quint64 primaryPadding = 0;
    CheckedByteRange videoRange;
    std::optional<qint64> presentationTimestampUs;
};

struct AndroidMotionPhotoDetection {
    AndroidMotionPhotoStatus status = AndroidMotionPhotoStatus::NotMotionPhoto;
    std::optional<AndroidMotionPhotoV1> metadata;
    QString error;
};

// Parses Android Motion Photo format 1.0 XMP and validates the referenced video
// against the actual source file. Modern Container metadata takes precedence.
// For JPEG only, an independently valid Samsung SEF/SEFT record may narrow a
// co-located modern video range to exclude trailing Samsung directory/footer
// framing; it never changes the modern start or expands the range. Without a
// modern declaration, structured Samsung metadata is preferred over legacy Pixel
// MicroVideo V1 metadata; MicroVideoOffset remains the final compatibility fallback.
// The returned range is suitable for ByteRangeDevice and is never derived with
// unchecked offset arithmetic.
AndroidMotionPhotoDetection detectAndroidMotionPhotoV1(const QString& sourcePath,
                                                       QByteArrayView xmp,
                                                       const std::atomic_bool* cancelled = nullptr);

// Parses the classic Samsung JPEG SEF/SEFT trailer and locates an indexed
// MotionPhoto_Data record. The SEFT directory is read from EOF with bounded
// arithmetic; arbitrary marker/ftyp scanning is intentionally not used. XMP is
// optional and is consulted only for a presentation timestamp after the SEFT
// record itself has been structurally validated.
AndroidMotionPhotoDetection
detectSamsungMotionPhotoJpegSeft(const QString& sourcePath, QByteArrayView xmp = {},
                                 const std::atomic_bool* cancelled = nullptr);

} // namespace Licasa
