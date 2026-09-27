#pragma once

#include "io/external_file_identity.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <atomic>
#include <optional>

namespace Licasa {

enum class AppleLivePhotoPairStatus {
    NoCandidate,
    NotPaired,
    Valid,
    Invalid,
};

struct AppleLivePhotoPairMetadata {
    QUrl videoUrl;
    QString contentIdentifier;
    ExternalFileIdentity videoIdentity;
};

struct AppleLivePhotoPairDetection {
    AppleLivePhotoPairStatus status = AppleLivePhotoPairStatus::NoCandidate;
    std::optional<AppleLivePhotoPairMetadata> metadata;
    QString error;
};

enum class AppleLivePhotoIdentifierStatus {
    Absent,
    Valid,
    Invalid,
};

struct AppleLivePhotoIdentifierDetection {
    AppleLivePhotoIdentifierStatus status = AppleLivePhotoIdentifierStatus::Absent;
    QString contentIdentifier;
    QString error;
};

// Parse the TIFF payload after an Exif header, or a complete HEIF Exif metadata
// item as returned by libheif. HEIF Exif items start with a big-endian offset
// to the TIFF header; both entry points keep the same Stage-1 bounds.
AppleLivePhotoIdentifierDetection appleLivePhotoIdentifierFromExifTiff(const QByteArray& tiff);
AppleLivePhotoIdentifierDetection
appleLivePhotoIdentifierFromHeifExifBlock(const QByteArray& metadataBlock);

// Cheap candidate check only. It never promotes a Live Photo and never parses
// still/movie metadata; callers must still require an exact identifier match.
bool hasAppleLivePhotoMovieCandidate(const QString& stillPath);

// Pair an already-validated still identifier with a same-basename MOV. This is
// the HEIC integration boundary: still metadata remains decoder-independent and
// the MOV is promoted only after its QuickTime identifier matches exactly.
AppleLivePhotoPairDetection
detectAppleLivePhotoPairForIdentifier(const QString& stillPath, const QString& contentIdentifier,
                                      const std::atomic_bool* cancelled = nullptr);

// Convenience Apple pairing entry point for JPEG still metadata. HEIC/HEIF
// supplies its already-validated identifier through the bridge above: the still's Apple MakerNote
// tag 0x0011 must match the paired MOV's mdta/com.apple.quicktime.content.identifier. Same-basename
// MOV discovery is only a cheap candidate lookup; matching metadata is required before Valid.
// Parsing is bounded and metadata-only: no image raster or media decoder is
// created. HEIC MakerNote extraction is a separate integration increment.
AppleLivePhotoPairDetection detectAppleLivePhotoPair(const QString& stillPath,
                                                     const std::atomic_bool* cancelled = nullptr);

} // namespace Licasa
